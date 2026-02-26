/* Fuzz harness for the Lattice bytecode deserializer (.latc / .rlatc).
 *
 * Build:  make fuzz-latc
 * Run:    ./build/fuzz_latc fuzz/corpus_latc/ -max_len=65536
 */

#include "latc.h"
#include "regvm.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* Timeout per input — kill runaway deserialization */
static void alarm_handler(int sig) {
    (void)sig;
    _exit(0);  /* clean exit, not a bug */
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Cap input size to avoid spending time on huge inputs */
    if (size > 65536) return 0;

    /* Set a 1-second alarm to bail out of hangs */
    signal(SIGALRM, alarm_handler);
    alarm(1);

    /* Try stack VM chunk deserialization */
    {
        char *err = NULL;
        Chunk *c = chunk_deserialize(data, size, &err);
        if (c) {
            chunk_free(c);
        }
        free(err);  /* NULL-safe */
    }

    /* Try register VM chunk deserialization */
    {
        char *err = NULL;
        RegChunk *c = regchunk_deserialize(data, size, &err);
        if (c) {
            regchunk_free(c);
        }
        free(err);  /* NULL-safe */
    }

    alarm(0);
    return 0;
}
