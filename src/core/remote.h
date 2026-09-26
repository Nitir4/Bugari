#ifndef GHM_CORE_REMOTE_PRIVATE_H
#define GHM_CORE_REMOTE_PRIVATE_H

#include <ghm/ghm.h>

/* Local URL helper for the core and temporary-repository tests. */
int ghm_remote_clone_url(const char *url, const char *destination,
                         const char *github_token, GhmError *error);
void ghm_remote_error_classify(const char *message, int error_class, GhmError *error);

#endif
