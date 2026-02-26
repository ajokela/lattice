/* Fuzz harness for the Lattice bytecode VM and register VM backends.
 *
 * Build:  make fuzz-vm
 * Run:    ./build/fuzz_vm fuzz/corpus/ -max_len=4096
 * Seed:   make fuzz-seed
 */

#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "stackcompiler.h"
#include "stackvm.h"
#include "regvm.h"
#include "value.h"
#include "runtime.h"
#include "chunk.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* Timeout per input — kill runaway programs (infinite loops etc.) */
static void alarm_handler(int sig) {
    (void)sig;
    _exit(0);  /* clean exit, not a bug */
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
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
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        free(src);
        alarm(0);
        return 0;
    }

    /* ── Stack VM backend ── */
    {
        value_set_heap(NULL);
        value_set_arena(NULL);

        char *comp_err = NULL;
        Chunk *chunk = stack_compile(&prog, &comp_err);
        if (!chunk) {
            free(comp_err);
        } else {
            LatRuntime rt;
            lat_runtime_init(&rt);
            StackVM vm;
            stackvm_init(&vm, &rt);

            LatValue vm_result;
            StackVMResult vm_res = stackvm_run(&vm, chunk, &vm_result);
            if (vm_res == STACKVM_OK) {
                value_free(&vm_result);
            }

            stackvm_free(&vm);
            lat_runtime_free(&rt);
            chunk_free(chunk);
        }
    }

    /* ── Register VM backend ── */
    {
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
            if (rvm_res == REGVM_OK) {
                value_free(&rresult);
            }

            regvm_free(&rvm);
            lat_runtime_free(&rrt);
            regchunk_free(rchunk);
        }
    }

    /* Cleanup */
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    free(src);

    alarm(0);
    return 0;
}
