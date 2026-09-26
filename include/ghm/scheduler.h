#ifndef GHM_SCHEDULER_H
#define GHM_SCHEDULER_H

#include <ghm/commit.h>

typedef struct {
    int64_t id;
    char *branch;
    char *message;
    char *status;
    char *error_message;
    char *result_oid;
    int64_t execute_at;
    int push_after_commit;
    char *push_status;
    int64_t push_retry_at;
    int push_attempts;
} GhmScheduledJob;

typedef struct {
    GhmScheduledJob *items;
    size_t count;
} GhmScheduledJobList;

/* Most recent jobs for a registered repository, up to 100 entries. */
int ghm_schedule_list(GhmContext *context, const char *repository_path,
                      GhmScheduledJobList *out, GhmError *error);
void ghm_schedule_list_free(GhmScheduledJobList *list);

/*
 * Freeze the current working tree into a persistent job. Repository path must
 * already be registered with ghm_repo_register(). Jobs for the same branch
 * execute in strictly increasing execute_at order.
 */
int ghm_schedule_add(GhmContext *context, const char *repository_path,
                     const char *message, const GhmCommitSignature *author,
                     const GhmCommitSignature *committer, int64_t execute_at,
                     int64_t *out_job_id, GhmError *error);

/* stage_all=1 captures every current non-ignored worktree change into a
 * private snapshot now. stage_all=0 overlays only real-index staged paths.
 * Neither mode stages more files when the worker executes the job. */
int ghm_schedule_add_mode(GhmContext *context, const char *repository_path,
                          const char *message, const GhmCommitSignature *author,
                          const GhmCommitSignature *committer, int64_t execute_at,
                          int stage_all, int64_t *out_job_id, GhmError *error);

/* Like add_mode, with an optional non-force push of exactly the resulting
 * commit. GitHub pushes require a saved OAuth session and public client ID. */
int ghm_schedule_add_options(GhmContext *context, const char *repository_path,
                             const char *message, const GhmCommitSignature *author,
                             const GhmCommitSignature *committer, int64_t execute_at,
                             int stage_all, int push_after_commit,
                             int64_t *out_job_id, GhmError *error);

/* Only a pending job without dependent pending jobs can be changed. */
int ghm_schedule_cancel(GhmContext *context, int64_t job_id, GhmError *error);
int ghm_schedule_reschedule(GhmContext *context, int64_t job_id,
                            int64_t execute_at, GhmError *error);
/* Editing metadata never changes the frozen file snapshot. */
int ghm_schedule_edit(GhmContext *context, int64_t job_id,
                      const char *message, int push_after_commit, GhmError *error);
/* Requeue a failed push of an already-created commit; never force pushes. */
int ghm_schedule_retry_push(GhmContext *context, int64_t job_id, GhmError *error);

/* Mark jobs interrupted by a previous worker as failed, never blindly retry. */
int ghm_schedule_recover(GhmContext *context, int64_t now, GhmError *error);

/* Execute at most one due job. Returns 1 if a job was handled, 0 if none, -1 on infrastructure error. */
int ghm_schedule_run_one_due(GhmContext *context, int64_t now, GhmError *error);

/* Handle at most one committed job awaiting its requested push. */
int ghm_schedule_run_one_push(GhmContext *context, GhmError *error);
/* Called on OS connectivity changes; rate-limit deadlines remain intact. */
int ghm_schedule_network_changed(GhmContext *context, GhmError *error);

/* Return the next due Unix time, or -1 when no pending jobs exist. */
int ghm_schedule_next_due(GhmContext *context, int64_t *out_execute_at, GhmError *error);

#endif
