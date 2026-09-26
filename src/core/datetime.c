#include <ghm/datetime.h>
#include "core/git.h"

#include <glib.h>
#include <string.h>

int ghm_datetime_parse(const char *text, int64_t *out_timestamp,
                       int *out_offset_minutes, GhmError *error)
{
    GDateTime *date;
    const char *time_part;
    if (text == NULL || out_timestamp == NULL || out_offset_minutes == NULL ||
        (time_part = strchr(text, 'T')) == NULL ||
        (strchr(time_part, 'Z') == NULL && strchr(time_part, '+') == NULL &&
         strchr(time_part + 1, '-') == NULL)) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT,
                      "Use an ISO date/time with offset, e.g. 2026-09-20T14:30:00+05:30");
        return -1;
    }
    date = g_date_time_new_from_iso8601(text, NULL);
    if (date == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Invalid ISO date/time or UTC offset");
        return -1;
    }
    *out_timestamp = g_date_time_to_unix(date);
    *out_offset_minutes = (int)(g_date_time_get_utc_offset(date) / G_TIME_SPAN_MINUTE);
    g_date_time_unref(date);
    return 0;
}
