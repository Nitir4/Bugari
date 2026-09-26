#include "core/mutation.h"
#include "core/index_lock.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Worktree-specific record, not committed content. Ref names are validated
 * before deriving paths; no path from the record is ever passed to unlink. */
typedef struct {
    char magic[8];
    char branch[1024], source[1024];
    char original[GHM_OID_HEX_CAPACITY], target[GHM_OID_HEX_CAPACITY];
    char index_tree[GHM_OID_HEX_CAPACITY];
    int switch_branch, checkout_files, sync_index, unborn;
    uint64_t device[4], inode[4];
} MutationRecord;

static char *join(const char *root, const char *name)
{
    size_t length = strlen(root) + strlen(name) + 2;
    char *path = malloc(length);
    if (path != NULL) (void)snprintf(path, length, "%s/%s", root, name);
    return path;
}

static char *record_path(git_repository *repo)
{ return join(git_repository_path(repo), "ghm-mutation.pending"); }

/* A terminated process in another worktree may still own native locks and a
 * recovery record. Refuse shared mutations until that worktree is recovered. */
int ghm_mutation_other_worktrees(git_repository *repo, GhmError *error)
{
    const char *common = git_repository_commondir(repo);
    const char *current = git_repository_path(repo);
    char *pending = join(common, "ghm-mutation.pending");
    struct stat metadata;
    int blocked = strcmp(common, current) != 0 && pending != NULL && lstat(pending, &metadata) == 0;
    free(pending);
    char *worktrees = join(common, "worktrees");
    DIR *directory = worktrees != NULL ? opendir(worktrees) : NULL;
    if (directory == NULL && errno != ENOENT) blocked = 1;
    if (directory != NULL) {
        struct dirent *entry;
        while (!blocked && (entry = readdir(directory)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char *path = join(worktrees, entry->d_name);
            pending = path != NULL ? join(path, "ghm-mutation.pending") : NULL;
            struct stat here, there;
            int same = path != NULL && stat(path, &there) == 0 && stat(current, &here) == 0 &&
                here.st_dev == there.st_dev && here.st_ino == there.st_ino;
            if (!same && pending != NULL && lstat(pending, &metadata) == 0) blocked = 1;
            free(path); free(pending);
        }
        closedir(directory);
    }
    free(worktrees);
    if (blocked) ghm_error_set(error, GHM_ERROR_BUSY,
        "Another linked worktree has an interrupted operation. Open that worktree in GHM to recover it before changing shared refs");
    return blocked ? -1 : 0;
}

static char *native_lock_path(git_repository *repo, const MutationRecord *record, size_t slot)
{
    if (slot == 3) {
        git_buf index = {0};
        char *path = NULL;
        if (git_repository_item_path(&index, repo, GIT_REPOSITORY_ITEM_INDEX) == 0) {
            size_t length = strlen(index.ptr) + sizeof(".lock");
            path = malloc(length);
            if (path != NULL) (void)snprintf(path, length, "%s.lock", index.ptr);
        }
        git_buf_dispose(&index);
        return path;
    }
    const char *name = slot == 0 ? "HEAD" : slot == 1 ? record->branch : record->source;
    if (name[0] == '\0') return NULL;
    char *path = join(slot == 0 ? git_repository_path(repo) : git_repository_commondir(repo), name);
    if (path == NULL) return NULL;
    char *locked = malloc(strlen(path) + sizeof(".lock"));
    if (locked != NULL) (void)snprintf(locked, strlen(path) + sizeof(".lock"), "%s.lock", path);
    free(path);
    return locked;
}

int ghm_mutation_pending(git_repository *repo)
{
    char *path = record_path(repo);
    struct stat metadata;
    int result = path != NULL && lstat(path, &metadata) == 0;
    free(path);
    return result;
}

void ghm_mutation_end(git_repository *repo, int keep_record)
{
    if (keep_record) return;
    char *path = record_path(repo);
    if (path != NULL) (void)unlink(path);
    free(path);
}

int ghm_mutation_begin(git_repository *repo, const git_reference *head,
    git_commit *target, const git_reference *source, int switch_branch,
    int checkout_files, int sync_index, git_index *index, GhmError *error)
{
    MutationRecord record = {.magic = "GHMTXN1", .switch_branch = switch_branch,
        .checkout_files = checkout_files, .sync_index = sync_index,
        .unborn = git_reference_type(head) == GIT_REFERENCE_SYMBOLIC};
    const char *branch = record.unborn ? git_reference_symbolic_target(head) : git_reference_name(head);
    if (strlen(branch) >= sizeof(record.branch) ||
        (source != NULL && strlen(git_reference_name(source)) >= sizeof(record.source))) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Branch name is too long for recovery record"); return -1;
    }
    (void)snprintf(record.branch, sizeof(record.branch), "%s", branch);
    if (source != NULL) (void)snprintf(record.source, sizeof(record.source), "%s", git_reference_name(source));
    if (!record.unborn) (void)git_oid_tostr(record.original, sizeof(record.original), git_reference_target(head));
    (void)git_oid_tostr(record.target, sizeof(record.target), git_commit_id(target));
    git_oid index_oid;
    if (git_index_write_tree_to(&index_oid, index, repo) < 0) {
        ghm_error_from_git(error, "Record index before mutation"); return -1;
    }
    (void)git_oid_tostr(record.index_tree, sizeof(record.index_tree), &index_oid);
    for (size_t i = 0; i < 4; ++i) {
        char *path = native_lock_path(repo, &record, i);
        struct stat metadata;
        if (path != NULL && lstat(path, &metadata) == 0 && S_ISREG(metadata.st_mode)) {
            record.device[i] = (uint64_t)metadata.st_dev;
            record.inode[i] = (uint64_t)metadata.st_ino;
        }
        free(path);
    }
    char *path = record_path(repo);
    int descriptor = path != NULL ? open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
    int result = -1;
    if (descriptor >= 0) {
        size_t offset = 0;
        while (offset < sizeof(record)) {
            ssize_t wrote = write(descriptor, (const char *)&record + offset, sizeof(record) - offset);
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) break;
            offset += (size_t)wrote;
        }
        if (offset == sizeof(record) && fsync(descriptor) == 0) {
            int parent = open(git_repository_path(repo), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (parent >= 0 && fsync(parent) == 0) result = 0;
            if (parent >= 0) close(parent);
        }
        close(descriptor);
        if (result != 0) (void)unlink(path);
    }
    free(path);
    if (result != 0) ghm_error_set(error, GHM_ERROR_IO, "Cannot persist recovery record; mutation was not started");
    return result;
}

static int valid_branch(const char *name, size_t capacity, int allow_empty, int allow_remote)
{
    int valid = 0;
    if (memchr(name, '\0', capacity) == NULL) return 0;
    if (name[0] == '\0') return allow_empty;
    return (strncmp(name, "refs/heads/", 11) == 0 || (allow_remote && strncmp(name, "refs/remotes/", 13) == 0)) &&
           git_reference_name_is_valid(&valid, name) == 0 && valid;
}

int ghm_mutation_recover(git_repository *repo, GhmError *error)
{
    MutationRecord record;
    git_reference *head = NULL;
    git_commit *target = NULL, *original = NULL;
    git_tree *baseline = NULL, *index_tree = NULL;
    git_index *index = NULL;
    GhmIndexLock native_index = {.descriptor = -1};
    char *path = record_path(repo);
    int descriptor = path != NULL ? open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
    int result = -1;
    if (descriptor < 0) {
        if (path != NULL && errno == ENOENT) { free(path); return 0; }
        goto inspection;
    }
    struct stat metadata;
    ssize_t received = read(descriptor, &record, sizeof(record));
    int safe = fstat(descriptor, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
        metadata.st_uid == geteuid() && metadata.st_nlink == 1 &&
        metadata.st_size == (off_t)sizeof(record) && received == (ssize_t)sizeof(record);
    close(descriptor);
    if (!safe || memcmp(record.magic, "GHMTXN1", 8) != 0 ||
        !valid_branch(record.branch, sizeof(record.branch), 0, 0) ||
        !valid_branch(record.source, sizeof(record.source), 1, !record.switch_branch) ||
        memchr(record.original, '\0', sizeof(record.original)) == NULL ||
        memchr(record.target, '\0', sizeof(record.target)) == NULL ||
        memchr(record.index_tree, '\0', sizeof(record.index_tree)) == NULL) goto inspection;
    git_oid target_oid, original_oid, before_index_oid;
    if (git_oid_fromstr(&target_oid, record.target) < 0 ||
        git_oid_fromstr(&before_index_oid, record.index_tree) < 0 ||
        (!record.unborn && git_oid_fromstr(&original_oid, record.original) < 0) ||
        git_commit_lookup(&target, repo, &target_oid) < 0 ||
        (!record.unborn && git_commit_lookup(&original, repo, &original_oid) < 0)) goto inspection;
    int head_result = git_repository_head(&head, repo);
    int published = head_result == 0 && git_reference_target(head) != NULL &&
        git_oid_equal(git_reference_target(head), &target_oid) &&
        strcmp(git_reference_name(head), record.switch_branch ? record.source : record.branch) == 0;
    int untouched = record.unborn ? head_result == GIT_EUNBORNBRANCH :
        head_result == 0 && git_reference_target(head) != NULL &&
        git_oid_equal(git_reference_target(head), &original_oid) && strcmp(git_reference_name(head), record.branch) == 0;
    if (record.unborn && untouched) {
        git_reference *symbolic = NULL;
        untouched = git_reference_lookup(&symbolic, repo, "HEAD") == 0 &&
            git_reference_type(symbolic) == GIT_REFERENCE_SYMBOLIC &&
            strcmp(git_reference_symbolic_target(symbolic), record.branch) == 0;
        git_reference_free(symbolic);
    }
    if (!published && !untouched) goto inspection;
    /* Verify every leftover native lock before removing any. Missing locks
     * were already published/released. A replaced inode is never removed. */
    for (size_t i = 0; i < 4; ++i) {
        char *locked = native_lock_path(repo, &record, i);
        int differs = 0;
        if (locked != NULL && lstat(locked, &metadata) == 0)
            differs = !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
                (uint64_t)metadata.st_dev != record.device[i] || (uint64_t)metadata.st_ino != record.inode[i];
        else if (locked != NULL && errno != ENOENT) differs = 1;
        free(locked);
        if (differs) goto inspection;
    }
    for (size_t i = 0; i < 4; ++i) {
        char *locked = native_lock_path(repo, &record, i);
        if (locked != NULL && unlink(locked) != 0 && errno != ENOENT) { free(locked); goto inspection; }
        free(locked);
    }
    if (ghm_index_lock_acquire(&native_index, repo, error) != 0 ||
        git_repository_index(&index, repo) < 0 || git_index_read(index, 1) < 0 || git_index_has_conflicts(index)) goto inspection;
    git_oid current_index_oid;
    if (git_index_write_tree_to(&current_index_oid, index, repo) < 0 ||
        (!git_oid_equal(&current_index_oid, &before_index_oid) &&
         !git_oid_equal(&current_index_oid, git_commit_tree_id(target)))) goto inspection;
    if (record.checkout_files && untouched) {
        git_checkout_options checkout = {0};
        if (git_checkout_options_init(&checkout, GIT_CHECKOUT_OPTIONS_VERSION) < 0 ||
            original == NULL || git_commit_tree(&baseline, target) < 0) goto inspection;
        checkout.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_DONT_WRITE_INDEX;
        checkout.baseline = baseline;
        if (git_checkout_tree(repo, (git_object *)original, &checkout) < 0) goto inspection;
    }
    if ((record.checkout_files && untouched) || (record.sync_index && published)) {
        if (published ? git_commit_tree(&index_tree, target) < 0 : git_tree_lookup(&index_tree, repo, &before_index_oid) < 0)
            goto inspection;
        if (git_index_read_tree(index, index_tree) < 0 || ghm_index_lock_write(&native_index, index, error) != 0) goto inspection;
    }
    if (unlink(path) != 0) goto inspection;
    result = 0;
    goto done;
inspection:
    ghm_error_set(error, GHM_ERROR_IO,
        "Interrupted operation needs inspection: unexpected edits, locks, or failed safe rollback. Recovery record was kept");
done:
    free(path); git_reference_free(head); git_commit_free(target); git_commit_free(original);
    git_tree_free(baseline); git_tree_free(index_tree); git_index_free(index);
    ghm_index_lock_release(&native_index);
    return result;
}
