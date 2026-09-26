#ifndef GHM_CORE_COMMIT_PRIVATE_H
#define GHM_CORE_COMMIT_PRIVATE_H
#include <ghm/commit.h>
typedef int (*GhmCommitPrepared)(const char *oid, void *payload, GhmError *error);
/* Persist a planned OID before publishing HEAD. Failure prevents publication. */
int ghm_commit_snapshot_prepared(const char *path, const char *snapshot,
    const char *branch, const char *parent, const char *message,
    const GhmCommitSignature *author, const GhmCommitSignature *committer,
    GhmCommitPrepared prepared, void *payload,
    char output[GHM_OID_HEX_CAPACITY], GhmError *error);
#endif
