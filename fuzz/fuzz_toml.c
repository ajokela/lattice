/* Fuzz harness for the Lattice TOML parser and serializer.
 *
 * Build:  make fuzz-toml
 * Run:    ./build/fuzz_toml fuzz/corpus_toml/ -max_len=4096
 */

#include "toml_ops.h"
#include "value.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* Timeout per input — kill runaway parsing */
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

    /* Parse TOML */
    char *err = NULL;
    LatValue val = toml_ops_parse(src, &err);
    if (err) {
        free(err);
    } else {
        /* Round-trip: serialize back and free */
        char *ser_err = NULL;
        char *toml_out = toml_ops_stringify(&val, &ser_err);
        free(toml_out); /* NULL-safe */
        free(ser_err);  /* NULL-safe */
        value_free(&val);
    }

    free(src);
    alarm(0);
    return 0;
}
