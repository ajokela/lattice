/* Fuzz harness for the Lattice register VM (RegVM) backend.
 *
 * Build:  make fuzz-regvm
 * Run:    ./build/fuzz_regvm fuzz/corpus/ -max_len=4096
 * Seed:   make fuzz-seed
 */

#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "regvm.h"
#include "value.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* Timeout per input — kill runaway programs (infinite loops etc.) */
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

    /* Set a 1-second alarm to bail out of infinite loops */
    signal(SIGALRM, alarm_handler);
    alarm(1);

    /* Lex */
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

    /* Parse */
    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        free(src);
        alarm(0);
        return 0;
    }

    /* RegVM compile + execute */
    value_set_heap(NULL);
    value_set_arena(NULL);

    char *rcomp_err = NULL;
    RegChunk *rchunk = reg_compile(&prog, &rcomp_err);
    if (!rchunk) {
        free(rcomp_err);
    } else {
        LatRuntime rrt;
        lat_runtime_init(&rrt);
        RegVM rvm;
        regvm_init(&rvm, &rrt);

        LatValue rresult;
        RegVMResult rvm_res = regvm_run(&rvm, rchunk, &rresult);
        if (rvm_res == REGVM_OK) { value_free(&rresult); }

        regvm_free(&rvm);
        lat_runtime_free(&rrt);
        regchunk_free(rchunk);
    }

    /* Cleanup */
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    free(src);

    alarm(0);
    return 0;
}
