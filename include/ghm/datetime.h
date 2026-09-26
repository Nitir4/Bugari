#ifndef GHM_DATETIME_H
#define GHM_DATETIME_H

#include <ghm/ghm.h>

/* Parse an ISO-8601 timestamp with an explicit UTC offset or Z. */
int ghm_datetime_parse(const char *text, int64_t *out_timestamp,
                       int *out_offset_minutes, GhmError *error);

#endif
