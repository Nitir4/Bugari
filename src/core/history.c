#include <ghm/history.h>
#include "core/git.h"

#include <git2.h>
#include <stdlib.h>
#include <string.h>

void ghm_history_free(GhmHistory *history)
{
    if (history == NULL) return;
    for (size_t i = 0; i < history->count; ++i) {
        free(history->items[i].summary);
        free(history->items[i].message);
        free(history->items[i].author_name);
        free(history->items[i].author_email);
    }
    free(history->items);
    *history = (GhmHistory){0};
}

int ghm_commit_get_history(const char *repository_path, size_t limit,
                           GhmHistory *out, GhmError *error)
{
    git_repository *repository = NULL;
    git_revwalk *walk = NULL;
    git_oid oid;
    int result = -1, step;
    if (repository_path == NULL || repository_path[0] == '\0' || out == NULL ||
        limit == 0 || limit > 500) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository, output and limit (1–500) are required");
        return -1;
    }
    *out = (GhmHistory){0};
    if (git_repository_open(&repository, repository_path) < 0) {
        ghm_error_from_git(error, "Open repository");
        goto done;
    }
    if (git_revwalk_new(&walk, repository) < 0) {
        ghm_error_from_git(error, "Create history walk");
        goto done;
    }
    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
    step = git_revwalk_push_head(walk);
    if (step == GIT_EUNBORNBRANCH || step == GIT_ENOTFOUND) {
        result = 0;
        goto done;
    }
    if (step < 0) { ghm_error_from_git(error, "Read branch history"); goto done; }
    while (out->count < limit && (step = git_revwalk_next(&oid, walk)) == 0) {
        git_commit *commit = NULL;
        GhmHistoryEntry *grown;
        GhmHistoryEntry *entry;
        const git_signature *author, *committer;
        if (git_commit_lookup(&commit, repository, &oid) < 0) {
            ghm_error_from_git(error, "Read commit history");
            goto done;
        }
        grown = realloc(out->items, (out->count + 1) * sizeof(*grown));
        if (grown == NULL) {
            git_commit_free(commit);
            ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
            goto done;
        }
        out->items = grown;
        entry = &out->items[out->count++];
        *entry = (GhmHistoryEntry){0};
        (void)git_oid_tostr(entry->oid, sizeof(entry->oid), &oid);
        author = git_commit_author(commit);
        committer = git_commit_committer(commit);
        entry->summary = strdup(git_commit_summary(commit) != NULL ? git_commit_summary(commit) : "");
        entry->message = strdup(git_commit_message(commit) != NULL ? git_commit_message(commit) : "");
        entry->author_name = strdup(author->name != NULL ? author->name : "");
        entry->author_email = strdup(author->email != NULL ? author->email : "");
        entry->author_timestamp = (int64_t)author->when.time;
        entry->author_offset = author->when.offset;
        entry->committer_timestamp = (int64_t)committer->when.time;
        entry->committer_offset = committer->when.offset;
        git_commit_free(commit);
        if (entry->summary == NULL || entry->message == NULL ||
            entry->author_name == NULL || entry->author_email == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
            goto done;
        }
    }
    if (step != GIT_ITEROVER && out->count < limit) {
        ghm_error_from_git(error, "Walk commit history");
        goto done;
    }
    result = 0;
done:
    git_revwalk_free(walk);
    git_repository_free(repository);
    if (result != 0) ghm_history_free(out);
    return result;
}
