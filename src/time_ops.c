#include "time_ops.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

int64_t time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

bool time_sleep_ms(int64_t ms, char **err) {
    if (ms < 0) {
        *err = strdup("sleep() expects non-negative milliseconds");
        return false;
    }
#ifdef __EMSCRIPTEN__
    /* sleep() would block the single-threaded WASM event loop */
    (void)ms;
    *err = strdup("sleep: not available in browser (would block event loop)");
    return false;
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000);
    if (nanosleep(&ts, NULL) != 0 && errno != EINTR) {
        *err = strdup("sleep: nanosleep failed");
        return false;
    }
    return true;
#endif
}
