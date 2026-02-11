#ifndef DATETIME_OPS_H
#define DATETIME_OPS_H

#include <stdint.h>

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

#endif
