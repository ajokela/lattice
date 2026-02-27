/* Fuzz harness for the Lattice source code formatter.
 *
 * Build:  make fuzz-formatter
 * Run:    ./build/fuzz_formatter fuzz/corpus/ -max_len=4096
 * Seed:   make fuzz-seed
 */

#include "formatter.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* Timeout per input — kill runaway formatting */
static void alarm_handler(int sig) {
    (void)sig;
    _exit(0); /* clean exit, not a bug */
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Cap input size to avoid spending time on huge inputs */
    if (size > 8192) return 0;

    /* Null-terminate the input */
    char *src = malloc(size + 1);
    if (!src) return 0;
    memcpy(src, data, size);
    src[size] = '\0';

    /* Set a 1-second alarm to bail out of hangs */
    signal(SIGALRM, alarm_handler);
    alarm(1);

    /* Format */
    char *err = NULL;
    char *formatted = lat_format(src, &err);
    free(formatted); /* NULL-safe */
    free(err);       /* NULL-safe */

    free(src);
    alarm(0);
    return 0;
}
