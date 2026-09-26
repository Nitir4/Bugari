#include <ghm/diff.h>
#include "core/git.h"

#include <git2.h>
#include <stdlib.h>
#include <string.h>

#define GHM_DIFF_LIMIT (512u * 1024u)

typedef struct {
    char *text;
    size_t length;
    size_t capacity;
    int overflow;
} DiffCollector;

static int collect_line(const git_diff_delta *delta, const git_diff_hunk *hunk,
                        const git_diff_line *line, void *payload)
{
    DiffCollector *collector = payload;
    size_t required;
    size_t prefix = line->origin == GIT_DIFF_LINE_ADDITION ||
                    line->origin == GIT_DIFF_LINE_DELETION ||
                    line->origin == GIT_DIFF_LINE_CONTEXT ? 1u : 0u;
    char *grown;
    (void)delta;
    (void)hunk;
    if (collector->length > GHM_DIFF_LIMIT ||
        prefix > GHM_DIFF_LIMIT - collector->length ||
        line->content_len > GHM_DIFF_LIMIT - collector->length - prefix) {
        collector->overflow = 1;
        return -1;
    }
    required = collector->length + prefix + line->content_len + 1;
    if (required > collector->capacity) {
        size_t capacity = collector->capacity == 0 ? 4096 : collector->capacity;
        while (capacity < required && capacity < GHM_DIFF_LIMIT + 2) capacity *= 2;
        if (capacity < required) capacity = required;
        grown = realloc(collector->text, capacity);
        if (grown == NULL) return -1;
        collector->text = grown;
        collector->capacity = capacity;
    }
    if (prefix != 0)
        collector->text[collector->length++] = line->origin;
    memcpy(collector->text + collector->length, line->content, line->content_len);
    collector->length += line->content_len;
    collector->text[collector->length] = '\0';
    return 0;
}

int ghm_repo_diff_file(const char *repository_path, const char *relative_path,
                       int staged, char **out_patch, GhmError *error)
{
    git_repository *repository = NULL;
    git_index *index = NULL;
    git_tree *head_tree = NULL;
    git_diff *diff = NULL;
    git_diff_options options = {0};
    git_object *head = NULL;
    DiffCollector collector = {0};
    int result = -1, head_result;
    if (repository_path == NULL || repository_path[0] == '\0' ||
        relative_path == NULL || relative_path[0] == '\0' || out_patch == NULL ||
        (staged != 0 && staged != 1)) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository, file path and diff output are required");
        return -1;
    }
    *out_patch = NULL;
    if (git_repository_open(&repository, repository_path) < 0 ||
        git_repository_index(&index, repository) < 0) {
        ghm_error_from_git(error, "Open repository index");
        goto done;
    }
    if (git_diff_options_init(&options, GIT_DIFF_OPTIONS_VERSION) < 0) {
        ghm_error_from_git(error, "Initialize diff options");
        goto done;
    }
    options.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH | GIT_DIFF_INCLUDE_UNTRACKED |
                    GIT_DIFF_SHOW_UNTRACKED_CONTENT | GIT_DIFF_RECURSE_UNTRACKED_DIRS;
    options.pathspec.strings = (char **)&relative_path;
    options.pathspec.count = 1;
    if (staged) {
        head_result = git_revparse_single(&head, repository, "HEAD^{commit}");
        if (head_result == 0) {
            if (git_commit_tree(&head_tree, (git_commit *)head) < 0) {
                ghm_error_from_git(error, "Read HEAD tree");
                goto done;
            }
        } else if (head_result != GIT_EUNBORNBRANCH && head_result != GIT_ENOTFOUND) {
            ghm_error_from_git(error, "Read HEAD for staged diff");
            goto done;
        }
        if (git_diff_tree_to_index(&diff, repository, head_tree, index, &options) < 0) {
            ghm_error_from_git(error, "Create staged diff");
            goto done;
        }
    } else if (git_diff_index_to_workdir(&diff, repository, index, &options) < 0) {
        ghm_error_from_git(error, "Create working-tree diff");
        goto done;
    }
    if (git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, collect_line, &collector) < 0) {
        ghm_error_set(error, collector.overflow ? GHM_ERROR_ARGUMENT : GHM_ERROR_MEMORY,
                      collector.overflow ? "Diff exceeds the 512 KiB preview limit" :
                                           "Cannot render diff preview");
        goto done;
    }
    if (collector.text == NULL) {
        collector.text = strdup("");
        if (collector.text == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
    }
    *out_patch = collector.text;
    collector.text = NULL;
    result = 0;
done:
    free(collector.text);
    git_diff_free(diff);
    git_tree_free(head_tree);
    git_object_free(head);
    git_index_free(index);
    git_repository_free(repository);
    return result;
}
