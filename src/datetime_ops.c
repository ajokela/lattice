#define _XOPEN_SOURCE 700  /* for strptime */
#include "datetime_ops.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *datetime_format(int64_t epoch_ms, const char *fmt, char **err) {
    time_t secs = (time_t)(epoch_ms / 1000);
    struct tm tm;
    if (localtime_r(&secs, &tm) == NULL) {
        *err = strdup("time_format: failed to convert timestamp");
        return NULL;
    }

    char buf[512];
    size_t n = strftime(buf, sizeof(buf), fmt, &tm);
    if (n == 0) {
        *err = strdup("time_format: format produced empty string or exceeded buffer");
        return NULL;
    }

    return strdup(buf);
}

int64_t datetime_parse(const char *str, const char *fmt, char **err) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_isdst = -1;  /* let mktime determine DST */

    char *rest = strptime(str, fmt, &tm);
    if (rest == NULL) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "time_parse: failed to parse \"%s\" with format \"%s\"",
                 str, fmt);
        *err = strdup(msg);
        return 0;
    }

    time_t secs = mktime(&tm);
    if (secs == (time_t)-1) {
        *err = strdup("time_parse: mktime failed to convert parsed time");
        return 0;
    }

    return (int64_t)secs * 1000;
}

/* ── Component extraction ── */

static struct tm epoch_to_tm(int64_t epoch_ms) {
    time_t secs = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&secs, &tm);
    return tm;
}

int datetime_year(int64_t epoch_ms)    { return epoch_to_tm(epoch_ms).tm_year + 1900; }
int datetime_month(int64_t epoch_ms)   { return epoch_to_tm(epoch_ms).tm_mon + 1; }
int datetime_day(int64_t epoch_ms)     { return epoch_to_tm(epoch_ms).tm_mday; }
int datetime_hour(int64_t epoch_ms)    { return epoch_to_tm(epoch_ms).tm_hour; }
int datetime_minute(int64_t epoch_ms)  { return epoch_to_tm(epoch_ms).tm_min; }
int datetime_second(int64_t epoch_ms)  { return epoch_to_tm(epoch_ms).tm_sec; }
int datetime_weekday(int64_t epoch_ms) { return epoch_to_tm(epoch_ms).tm_wday; }

int64_t datetime_add(int64_t epoch_ms, int64_t delta_ms) {
    return epoch_ms + delta_ms;
}

bool datetime_is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}
