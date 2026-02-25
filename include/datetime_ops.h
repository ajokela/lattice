#ifndef DATETIME_OPS_H
#define DATETIME_OPS_H

#include <stdint.h>
#include <stdbool.h>

/* Format a Unix timestamp (milliseconds) using strftime-style format string.
 * Returns heap-allocated formatted string. Sets *err on failure.
 *
 * Common format specifiers:
 *   %Y  four-digit year       %m  month (01-12)      %d  day (01-31)
 *   %H  hour (00-23)          %M  minute (00-59)      %S  second (00-59)
 *   %Y-%m-%d   ISO date       %H:%M:%S  time
 */
char *datetime_format(int64_t epoch_ms, const char *fmt, char **err);

/* Parse a date/time string using strftime-style format string.
 * Returns milliseconds since epoch. Sets *err on failure. */
int64_t datetime_parse(const char *str, const char *fmt, char **err);

/* Component extraction from epoch milliseconds */
int datetime_year(int64_t epoch_ms);
int datetime_month(int64_t epoch_ms);
int datetime_day(int64_t epoch_ms);
int datetime_hour(int64_t epoch_ms);
int datetime_minute(int64_t epoch_ms);
int datetime_second(int64_t epoch_ms);
int datetime_weekday(int64_t epoch_ms);    /* 0=Sunday, 6=Saturday */
int64_t datetime_add(int64_t epoch_ms, int64_t delta_ms);
bool datetime_is_leap_year(int year);

/* ── Calendar utilities ── */

/* Number of days in a given month (1-12) of a given year */
int datetime_days_in_month(int year, int month);

/* Day of year (1-366) for a given date */
int datetime_day_of_year(int year, int month, int day);

/* Day of week (0=Sunday, 6=Saturday) using Zeller-like algorithm */
int datetime_day_of_week(int year, int month, int day);

/* ── Timezone ── */

/* Current local timezone offset from UTC in seconds */
int datetime_tz_offset_seconds(void);

/* ── ISO 8601 ── */

/* Parse ISO 8601 string (e.g. "2026-02-24T10:30:00Z") into epoch seconds.
 * Sets *err on failure. */
int64_t datetime_parse_iso(const char *str, char **err);

/* Format epoch seconds as ISO 8601 string.
 * Returns heap-allocated string. */
char *datetime_to_iso(int64_t epoch_sec);

/* Convert epoch seconds to UTC struct components.
 * Fills year, month (1-12), day (1-31), hour, minute, second. */
void datetime_to_utc_components(int64_t epoch_sec, int *year, int *month, int *day,
                                int *hour, int *minute, int *second);

/* Convert UTC components to epoch seconds. Returns -1 on error. */
int64_t datetime_from_components(int year, int month, int day,
                                 int hour, int minute, int second, int tz_offset_sec);

#endif
