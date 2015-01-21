/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2015 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <sys/prctl.h>
#include <curl/curl.h>

#include "utf8.h"
#include "strv.h"
#include "copy.h"
#include "btrfs-util.h"
#include "util.h"
#include "macro.h"
#include "mkdir.h"
#include "curl-util.h"
#include "import-job.h"
#include "import-util.h"
#include "import-tar.h"

struct TarImport {
        sd_event *event;
        CurlGlue *glue;

        char *image_root;

        ImportJob *tar_job;

        TarImportFinished on_finished;
        void *userdata;

        char *local;
        bool force_local;

        pid_t tar_pid;

        char *temp_path;
        char *final_path;
};

TarImport* tar_import_unref(TarImport *i) {
        if (!i)
                return NULL;

        if (i->tar_pid > 0) {
                kill(i->tar_pid, SIGKILL);
                wait_for_terminate(i->tar_pid, NULL);
        }

        import_job_unref(i->tar_job);

        curl_glue_unref(i->glue);
        sd_event_unref(i->event);

        if (i->temp_path) {
                (void) btrfs_subvol_remove(i->temp_path);
                (void) rm_rf_dangerous(i->temp_path, false, true, false);
                free(i->temp_path);
        }

        free(i->final_path);
        free(i->image_root);
        free(i->local);

        free(i);

        return NULL;
}

int tar_import_new(TarImport **ret, sd_event *event, const char *image_root, TarImportFinished on_finished, void *userdata) {
        _cleanup_(tar_import_unrefp) TarImport *i = NULL;
        int r;

        assert(ret);
        assert(event);

        i = new0(TarImport, 1);
        if (!i)
                return -ENOMEM;

        i->on_finished = on_finished;
        i->userdata = userdata;

        i->image_root = strdup(image_root ?: "/var/lib/machines");
        if (!i->image_root)
                return -ENOMEM;

        if (event)
                i->event = sd_event_ref(event);
        else {
                r = sd_event_default(&i->event);
                if (r < 0)
                        return r;
        }

        r = curl_glue_new(&i->glue, i->event);
        if (r < 0)
                return r;

        i->glue->on_finished = import_job_curl_on_finished;
        i->glue->userdata = i;

        *ret = i;
        i = NULL;

        return 0;
}

static int tar_import_make_local_copy(TarImport *i) {
        int r;

        assert(i);
        assert(i->tar_job);

        if (!i->local)
                return 0;

        if (!i->final_path) {
                r = import_make_path(i->tar_job->url, i->tar_job->etag, i->image_root, ".tar-", NULL, &i->final_path);
                if (r < 0)
                        return log_oom();

                r = import_make_local_copy(i->final_path, i->image_root, i->local, i->force_local);
                if (r < 0)
                        return r;
        }

        return 0;
}

static void tar_import_job_on_finished(ImportJob *j) {
        TarImport *i;
        int r;

        assert(j);
        assert(j->userdata);

        i = j->userdata;
        if (j->error != 0) {
                r = j->error;
                goto finish;
        }

        /* This is invoked if either the download completed
         * successfully, or the download was skipped because we
         * already have the etag. */

        j->disk_fd = safe_close(j->disk_fd);

        if (i->tar_pid > 0) {
                r = wait_for_terminate_and_warn("tar", i->tar_pid, true);
                i->tar_pid = 0;
                if (r < 0)
                        goto finish;
        }

        if (i->temp_path) {
                r = import_make_read_only(i->temp_path);
                if (r < 0)
                        goto finish;

                if (rename(i->temp_path, i->final_path) < 0) {
                        r = log_error_errno(errno, "Failed to rename to final image name: %m");
                        goto finish;
                }

                free(i->temp_path);
                i->temp_path = NULL;
        }

        r = tar_import_make_local_copy(i);
        if (r < 0)
                goto finish;

        r = 0;

finish:
        if (i->on_finished)
                i->on_finished(i, r, i->userdata);
        else
                sd_event_exit(i->event, r);
}

static int tar_import_job_on_open_disk(ImportJob *j) {
        _cleanup_close_pair_ int pipefd[2] = { -1 , -1 };
        TarImport *i;
        int r;

        assert(j);
        assert(j->userdata);

        i = j->userdata;

        r = import_make_path(j->url, j->etag, i->image_root, ".tar-", NULL, &i->final_path);
        if (r < 0)
                return log_oom();

        r = tempfn_random(i->final_path, &i->temp_path);
        if (r < 0)
                return log_oom();

        mkdir_parents_label(i->temp_path, 0700);

        r = btrfs_subvol_make(i->temp_path);
        if (r == -ENOTTY) {
                if (mkdir(i->temp_path, 0755) < 0)
                        return log_error_errno(errno, "Failed to create directory %s: %m", i->temp_path);
        } else if (r < 0)
                return log_error_errno(errno, "Failed to create subvolume %s: %m", i->temp_path);

        if (pipe2(pipefd, O_CLOEXEC) < 0)
                return log_error_errno(errno, "Failed to create pipe for tar: %m");

        i->tar_pid = fork();
        if (i->tar_pid < 0)
                return log_error_errno(errno, "Failed to fork off tar: %m");
        if (i->tar_pid == 0) {
                int null_fd;

                reset_all_signal_handlers();
                reset_signal_mask();
                assert_se(prctl(PR_SET_PDEATHSIG, SIGTERM) == 0);

                pipefd[1] = safe_close(pipefd[1]);

                if (dup2(pipefd[0], STDIN_FILENO) != STDIN_FILENO) {
                        log_error_errno(errno, "Failed to dup2() fd: %m");
                        _exit(EXIT_FAILURE);
                }

                if (pipefd[0] != STDIN_FILENO)
                        safe_close(pipefd[0]);

                null_fd = open("/dev/null", O_WRONLY|O_NOCTTY);
                if (null_fd < 0) {
                        log_error_errno(errno, "Failed to open /dev/null: %m");
                        _exit(EXIT_FAILURE);
                }

                if (dup2(null_fd, STDOUT_FILENO) != STDOUT_FILENO) {
                        log_error_errno(errno, "Failed to dup2() fd: %m");
                        _exit(EXIT_FAILURE);
                }

                if (null_fd != STDOUT_FILENO)
                        safe_close(null_fd);

                execlp("tar", "tar", "--numeric-owner", "-C", i->temp_path, "-px", NULL);
                log_error_errno(errno, "Failed to execute tar: %m");
                _exit(EXIT_FAILURE);
        }

        pipefd[0] = safe_close(pipefd[0]);

        j->disk_fd = pipefd[1];
        pipefd[1] = -1;

        return 0;
}

int tar_import_pull(TarImport *i, const char *url, const char *local, bool force_local) {
        int r;

        assert(i);

        if (i->tar_job)
                return -EBUSY;

        if (!http_url_is_valid(url))
                return -EINVAL;

        if (local && !machine_name_is_valid(local))
                return -EINVAL;

        r = free_and_strdup(&i->local, local);
        if (r < 0)
                return r;

        i->force_local = force_local;

        r = import_job_new(&i->tar_job, url, i->glue, i);
        if (r < 0)
                return r;

        i->tar_job->on_finished = tar_import_job_on_finished;
        i->tar_job->on_open_disk = tar_import_job_on_open_disk;

        r = import_find_old_etags(url, i->image_root, DT_DIR, ".tar-", NULL, &i->tar_job->old_etags);
        if (r < 0)
                return r;

        return import_job_begin(i->tar_job);
}
