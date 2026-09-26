#ifndef GHM_COMMIT_H
#define GHM_COMMIT_H

#include <ghm/ghm.h>
#include <stdint.h>

/* Room for a SHA-256 object ID and its terminating NUL. */
#define GHM_OID_HEX_CAPACITY 65

typedef struct {
    const char *name;
    const char *email;
    int64_t timestamp;
    int offset_minutes;
} GhmCommitSignature;

/* Resolve user.name and user.email from the repository's Git configuration.
 * Caller owns both returned strings and frees them with free(). */
int ghm_commit_default_identity(const char *repository_path, char **out_name,
                                char **out_email, GhmError *error);

/* Stage the current non-ignored working tree and create a local commit now.
 * Supports an unborn branch and explicit author/committer metadata times. */
int ghm_commit_now(const char *repository_path, const char *message,
                   const GhmCommitSignature *author, const GhmCommitSignature *committer,
                   char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error);

/* Commit only the real Git index, preserving unstaged working-tree changes. */
int ghm_commit_staged(const char *repository_path, const char *message,
                      const GhmCommitSignature *author, const GhmCommitSignature *committer,
                      char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error);

/* Advanced, local-only rewrite of HEAD's message and timestamps. The caller
 * must show a destructive-history confirmation and pass confirm=1. A pushed
 * branch cannot be force-pushed by this API. Identities/tree/parents are kept.
 * Pending/running scheduled commits block rewriting through any path alias. */
int ghm_commit_rewrite_head(GhmContext *context, const char *repository_path,
                            const char *expected_oid, const char *message,
                            int64_t author_timestamp, int author_offset,
                            int64_t committer_timestamp, int committer_offset,
                            int confirm, char out_oid[GHM_OID_HEX_CAPACITY], GhmError *error);

/*
 * Capture the current working tree without modifying .git/index. The base is
 * HEAD's tree when base_tree_oid is NULL, or an earlier job's snapshot tree.
 * snapshot_ref must be a unique name under refs/ghm/jobs/. It protects the
 * captured tree and blobs from garbage collection until the job is finished.
 */
int ghm_snapshot_capture(const char *repository_path, const char *base_tree_oid,
                         const char *snapshot_ref, char out_tree_oid[GHM_OID_HEX_CAPACITY],
                         GhmError *error);

/* Like capture, but choose either all current worktree changes or only paths
 * explicitly staged in the real index. Neither mode changes the real index. */
int ghm_snapshot_capture_mode(const char *repository_path, const char *base_tree_oid,
                              const char *snapshot_ref, int stage_all,
                              char out_tree_oid[GHM_OID_HEX_CAPACITY], GhmError *error);

/*
 * Create a commit from a frozen snapshot. Fails if HEAD no longer matches
 * expected_parent_oid, rather than overwriting intervening branch changes.
 * The caller is responsible for removing the snapshot ref after completion.
 */
int ghm_commit_from_snapshot(const char *repository_path, const char *snapshot_ref,
                             const char *expected_branch, const char *expected_parent_oid,
                             const char *message,
                             const GhmCommitSignature *author,
                             const GhmCommitSignature *committer,
                             char out_commit_oid[GHM_OID_HEX_CAPACITY], GhmError *error);

/* Release an internal job snapshot only if it still points to the expected tree. */
int ghm_snapshot_release(const char *repository_path, const char *snapshot_ref,
                         const char *expected_tree_oid, GhmError *error);

#endif
