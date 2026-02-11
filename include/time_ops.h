#ifndef TIME_OPS_H
#define TIME_OPS_H

#include <stdint.h>
#include <stdbool.h>

/* Return current time as milliseconds since Unix epoch. */
int64_t time_now_ms(void);

/* Sleep for the given number of milliseconds.
 * Returns true on success, sets *err on failure. */
bool time_sleep_ms(int64_t ms, char **err);

#endif
