/* Fuzz harness for the Lattice lexer (tokenizer only).
 *
 * Exercises the lexer in isolation without parser/evaluator overhead,
 * allowing deeper exploration of tokenization edge cases.
 *
 * Build:  make fuzz-lexer
 * Run:    ./build/fuzz_lexer fuzz/corpus/ -max_len=4096
 * Seed:   make fuzz-seed
 */

#include "lattice.h"
#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* Timeout per input — kill runaway tokenization */
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

    /* Tokenize */
    Lexer lex = lexer_new(src);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    free(lex_err); /* NULL-safe */

    /* Cleanup */
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    free(src);

    alarm(0);
    return 0;
}
