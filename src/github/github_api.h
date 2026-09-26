#ifndef GHM_GITHUB_API_H
#define GHM_GITHUB_API_H

#include <ghm/ghm.h>

typedef struct {
    char *body;
    long status;
    int rate_remaining_zero;
    int64_t rate_reset_at;
    int64_t retry_at;
} GhmHttpResponse;

int ghm_http_request(const char *url, const char *post_body, const char *accept,
                     const char *bearer_token, GhmHttpResponse *out, GhmError *error);
void ghm_http_response_clear(GhmHttpResponse *response);
void ghm_http_read_header(GhmHttpResponse *response, const char *line, size_t length);

#endif
