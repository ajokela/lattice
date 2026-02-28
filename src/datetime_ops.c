#define _XOPEN_SOURCE 700 /* for strptime */
#include "datetime_ops.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif

/* Portable timegm: convert struct tm (UTC) to time_t without timezone adjustment.
 * Uses the trick of setting TZ=UTC, calling mktime, then restoring TZ.
 * For better portability, we compute it manually. */
static time_t portable_timegm(struct tm *tm) {
    /* Save tm_isdst, force to 0 for UTC */
    tm->tm_isdst = 0;
    /* Use mktime (which interprets as local time) then compensate */
    time_t local_epoch = mktime(tm);
    if (local_epoch == (time_t)-1) return (time_t)-1;
    /* Compute the offset mktime applied (local vs UTC) */
    struct tm utc_check;
    gmtime_r(&local_epoch, &utc_check);
    time_t utc_epoch = mktime(&utc_check);
    /* The difference is the local timezone offset that mktime applied */
    return local_epoch + (local_epoch - utc_epoch);
}

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
    tm.tm_isdst = -1; /* let mktime determine DST */

    char *rest = strptime(str, fmt, &tm);
    if (rest == NULL) {
        char msg[256];
        snprintf(msg, sizeof(msg), "time_parse: failed to parse \"%s\" with format \"%s\"", str, fmt);
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

int datetime_year(int64_t epoch_ms) { return epoch_to_tm(epoch_ms).tm_year + 1900; }
int datetime_month(int64_t epoch_ms) { return epoch_to_tm(epoch_ms).tm_mon + 1; }
int datetime_day(int64_t epoch_ms) { return epoch_to_tm(epoch_ms).tm_mday; }
int datetime_hour(int64_t epoch_ms) { return epoch_to_tm(epoch_ms).tm_hour; }
int datetime_minute(int64_t epoch_ms) { return epoch_to_tm(epoch_ms).tm_min; }
int datetime_second(int64_t epoch_ms) { return epoch_to_tm(epoch_ms).tm_sec; }
int datetime_weekday(int64_t epoch_ms) { return epoch_to_tm(epoch_ms).tm_wday; }

int64_t datetime_add(int64_t epoch_ms, int64_t delta_ms) { return epoch_ms + delta_ms; }

bool datetime_is_leap_year(int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

/* ── Calendar utilities ── */

int datetime_days_in_month(int year, int month) {
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return -1;
    if (month == 2 && datetime_is_leap_year(year)) return 29;
    return days[month];
}

int datetime_day_of_year(int year, int month, int day) {
    static const int cum[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (month < 1 || month > 12) return -1;
    int doy = cum[month] + day;
    if (month > 2 && datetime_is_leap_year(year)) doy++;
    return doy;
}

int datetime_day_of_week(int year, int month, int day) {
    /* Tomohiko Sakamoto's algorithm: returns 0=Sunday..6=Saturday */
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
}

/* ── Timezone ── */

int datetime_tz_offset_seconds(void) {
    time_t now = time(NULL);
    struct tm local_tm, utc_tm;
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);

    /* Convert both to approximate seconds-since-epoch for comparison */
    int64_t local_sec = (int64_t)local_tm.tm_hour * 3600 + (int64_t)local_tm.tm_min * 60 + local_tm.tm_sec +
                        (int64_t)local_tm.tm_yday * 86400;
    int64_t utc_sec =
        (int64_t)utc_tm.tm_hour * 3600 + (int64_t)utc_tm.tm_min * 60 + utc_tm.tm_sec + (int64_t)utc_tm.tm_yday * 86400;

    /* Handle year boundary (e.g. Dec 31 UTC vs Jan 1 local) */
    if (local_tm.tm_year > utc_tm.tm_year) {
        local_sec += 365 * 86400; /* approximate */
    } else if (utc_tm.tm_year > local_tm.tm_year) {
        utc_sec += 365 * 86400;
    }

    return (int)(local_sec - utc_sec);
}

/* ── ISO 8601 parsing/formatting ── */

int64_t datetime_parse_iso(const char *str, char **err) {
    int year, month, day, hour = 0, minute = 0, second = 0;
    int tz_hour = 0, tz_min = 0;
    char tz_sign = '+';
    bool has_time = false;
    bool is_utc = false;

    /* Try full ISO 8601: YYYY-MM-DDTHH:MM:SS[Z|+HH:MM|-HH:MM] */
    int n = sscanf(str, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    if (n >= 3) {
        if (n >= 4) has_time = true;

        /* Look for timezone suffix */
        const char *p = str;
        while (*p && *p != 'Z' && *p != '+') {
            if (*p == '-' && p > str + 10) break; /* timezone minus (not date minus) */
            p++;
        }
        if (*p == 'Z') {
            is_utc = true;
        } else if (*p == '+' || (*p == '-' && p > str + 10)) {
            tz_sign = *p;
            sscanf(p + 1, "%d:%d", &tz_hour, &tz_min);
        }
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "datetime_from_iso: failed to parse \"%s\"", str);
        *err = strdup(msg);
        return 0;
    }

    /* Validate ranges */
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        *err = strdup("datetime_from_iso: date/time components out of range");
        return 0;
    }

    int tz_offset_sec = 0;
    if (is_utc) {
        tz_offset_sec = 0;
    } else if (has_time) {
        tz_offset_sec = (tz_hour * 3600 + tz_min * 60);
        if (tz_sign == '-') tz_offset_sec = -tz_offset_sec;
    }

    return datetime_from_components(year, month, day, hour, minute, second, tz_offset_sec);
}

char *datetime_to_iso(int64_t epoch_sec) {
    int year, month, day, hour, minute, second;
    datetime_to_utc_components(epoch_sec, &year, &month, &day, &hour, &minute, &second);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day, hour, minute, second);
    return strdup(buf);
}

void datetime_to_utc_components(int64_t epoch_sec, int *year, int *month, int *day, int *hour, int *minute,
                                int *second) {
    time_t t = (time_t)epoch_sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    *year = tm.tm_year + 1900;
    *month = tm.tm_mon + 1;
    *day = tm.tm_mday;
    *hour = tm.tm_hour;
    *minute = tm.tm_min;
    *second = tm.tm_sec;
}

int64_t datetime_from_components(int year, int month, int day, int hour, int minute, int second, int tz_offset_sec) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0; /* no DST adjustment for explicit timezone */

    /* Use portable_timegm to get UTC epoch, then subtract the tz offset */
    time_t utc = portable_timegm(&tm);
    if (utc == (time_t)-1) return -1;
    return (int64_t)utc - (int64_t)tz_offset_sec;
}
