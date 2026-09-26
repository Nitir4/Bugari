#ifndef GHM_FILE_OPS_H
#define GHM_FILE_OPS_H

#include <ghm/ghm.h>

typedef struct {
    int64_t size;
    int64_t mtime_seconds;
    long mtime_nanoseconds;
} GhmFileVersion;

/* All paths are relative to the working tree. Parent symlinks and .git paths
 * are rejected. The caller owns returned text and frees it with free(). */
int ghm_file_read_text(const char *repository_path, const char *relative_path,
                       char **out_text, size_t *out_length, GhmFileVersion *out_version,
                       GhmError *error);
int ghm_file_create(const char *repository_path, const char *relative_path, GhmError *error);
int ghm_directory_create(const char *repository_path, const char *relative_path, GhmError *error);
int ghm_file_save_text(const char *repository_path, const char *relative_path,
                       const char *text, size_t length, const GhmFileVersion *expected,
                       GhmFileVersion *out_version, GhmError *error);
int ghm_file_delete(const char *repository_path, const char *relative_path, GhmError *error);
int ghm_file_rename(const char *repository_path, const char *old_relative_path,
                     const char *new_relative_path, GhmError *error);

#endif
