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
