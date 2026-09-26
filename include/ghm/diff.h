#ifndef GHM_DIFF_H
#define GHM_DIFF_H

#include <ghm/ghm.h>

/* Unified diff for one repository-relative path. staged=0 compares the real
 * index to the working tree; staged=1 compares HEAD to the real index.
 * Output is capped at 512 KiB and owned by the caller. */
int ghm_repo_diff_file(const char *repository_path, const char *relative_path,
                       int staged, char **out_patch, GhmError *error);

#endif
