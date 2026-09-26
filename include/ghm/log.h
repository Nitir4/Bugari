#ifndef GHM_LOG_H
#define GHM_LOG_H

#include <ghm/ghm.h>

typedef enum {
    GHM_LOG_DEBUG,
    GHM_LOG_INFO,
    GHM_LOG_WARNING,
    GHM_LOG_ERROR
} GhmLogLevel;

/* Best-effort JSON Lines logging; never prints from the core library. */
void ghm_log_event(GhmLogLevel level, const char *component,
                   const char *event, const char *detail);

/* Read at most max_bytes from the end of the log. Caller frees out_text. */
int ghm_log_tail(size_t max_bytes, char **out_text, GhmError *error);

#endif
