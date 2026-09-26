#ifndef GHM_HISTORY_H
#define GHM_HISTORY_H

#include <ghm/commit.h>

typedef struct {
    char oid[GHM_OID_HEX_CAPACITY];
    char *summary;
    char *message;
    char *author_name;
    char *author_email;
    int64_t author_timestamp;
    int author_offset;
    int64_t committer_timestamp;
    int committer_offset;
} GhmHistoryEntry;

typedef struct {
    GhmHistoryEntry *items;
    size_t count;
} GhmHistory;

/* Walk the checked-out branch newest-first, up to limit commits (max 500). */
int ghm_commit_get_history(const char *repository_path, size_t limit,
                           GhmHistory *out, GhmError *error);
void ghm_history_free(GhmHistory *history);

#endif
