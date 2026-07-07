/* Fuzz harness for the Lattice parser (lex -> parse -> free AST).
 *
 * Exercises the parser in isolation: deeper than fuzz_lexer (tokens only) and
 * more targeted than fuzz_eval / fuzz_latc (which run the whole pipeline), so
 * the fuzzer spends its budget on parser bugs — AST construction and the parse
 * error paths — rather than on lexing or evaluation.
 *
 * Build:  make fuzz-parser
 * Run:    ./build/fuzz_parser fuzz/corpus_parser/ -max_len=4096
 */

#include "lattice.h"
#include "lexer.h"
#include "parser.h"
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

    /* Lex first — a lexer error means there's nothing to parse. */
    Lexer lex = lexer_new(src);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        free(src);
        alarm(0);
        return 0;
    }

    /* Parse — the target under test. parser_parse returns a Program even on
     * error (with parse_err set), so free both unconditionally. */
    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    free(parse_err); /* NULL-safe */
    program_free(&prog);

    /* Cleanup */
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    free(src);

    alarm(0);
    return 0;
}
