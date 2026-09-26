#ifndef GHM_GITHUB_AUTH_INTERNAL_H
#define GHM_GITHUB_AUTH_INTERNAL_H

#include <ghm/github.h>

int ghm_auth_parse_device_response(const char *body, long http_status,
                                   GhmDeviceCode *out, GhmError *error);
int ghm_auth_parse_poll_response(const char *body, long http_status, unsigned current_interval,
                                 GhmAuthPollStatus *status, GhmOAuthToken *token,
                                 unsigned *next_interval, GhmError *error);

#endif
