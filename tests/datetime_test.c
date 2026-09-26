#include <ghm/datetime.h>

#include <stdio.h>

int main(void)
{
    int64_t timestamp = 0, utc_timestamp = 0;
    int offset = 0;
    GhmError error = {0};
    if (ghm_datetime_parse("2026-09-20T14:30:00+05:30", &timestamp, &offset, &error) != 0 ||
        offset != 330 ||
        ghm_datetime_parse("2026-09-20T09:00:00Z", &utc_timestamp, &offset, &error) != 0 ||
        timestamp != utc_timestamp || offset != 0 ||
        ghm_datetime_parse("2026-02-30T14:30:00+05:30", &timestamp, &offset, &error) == 0 ||
        ghm_datetime_parse("2026-09-20T14:30:00", &timestamp, &offset, &error) == 0) {
        fprintf(stderr, "Date parser test failed: %s\n", error.message);
        return 1;
    }
    return 0;
}
