#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "eval.h"
#include "phase_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static bool gc_stress_mode = false;

static int run_source(const char *source, bool show_stats) {
    /* Lex */
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        fprintf(stderr, "error: %s\n", lex_err);
        free(lex_err);
        return 1;
    }

    /* Parse */
    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);
    if (parse_err) {
        fprintf(stderr, "error: %s\n", parse_err);
        free(parse_err);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    /* Phase check (strict mode only) */
    if (prog.mode == MODE_STRICT) {
        LatVec errors = phase_check(&prog);
        if (errors.len > 0) {
            for (size_t i = 0; i < errors.len; i++) {
                char **msg = lat_vec_get(&errors, i);
                fprintf(stderr, "phase error: %s\n", *msg);
                free(*msg);
            }
            lat_vec_free(&errors);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++)
                token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            return 1;
        }
        lat_vec_free(&errors);
    }

    /* Evaluate */
    Evaluator *ev = evaluator_new();
    if (gc_stress_mode)
        evaluator_set_gc_stress(ev, true);
    char *eval_err = evaluator_run(ev, &prog);
    if (eval_err) {
        fprintf(stderr, "error: %s\n", eval_err);
        free(eval_err);
        evaluator_free(ev);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++)
            token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    if (show_stats) {
        fprintf(stderr, "\n");
        memory_stats_print(evaluator_stats(ev), stderr);
    }

    evaluator_free(ev);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return 0;
}

static int run_file(const char *path, bool show_stats) {
    char *source = read_file(path);
    if (!source) {
        fprintf(stderr, "error: cannot read '%s'\n", path);
        return 1;
    }
    int result = run_source(source, show_stats);
    free(source);
    return result;
}

static void repl(void) {
    printf("Lattice v0.1.0 (C) — crystallization-based programming language\n");
    printf("Type expressions to evaluate. Ctrl-D to exit.\n\n");

    char line[4096];
    char accumulated[65536];
    accumulated[0] = '\0';

    for (;;) {
        if (accumulated[0] == '\0')
            printf("lattice> ");
        else
            printf("    ...> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        strcat(accumulated, line);

        int result = run_source(accumulated, false);
        if (result == 0) {
            accumulated[0] = '\0';
        } else {
            /* Simple heuristic: if it's an EOF-type error, keep reading */
            if (strstr(line, "\n") && line[0] != '\n') {
                /* Try again next line */
            } else {
                accumulated[0] = '\0';
            }
        }
    }
}

int main(int argc, char **argv) {
    bool show_stats = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stats") == 0)
            show_stats = true;
        else if (strcmp(argv[i], "--gc-stress") == 0)
            gc_stress_mode = true;
        else if (!file)
            file = argv[i];
        else {
            fprintf(stderr, "usage: clat [--stats] [--gc-stress] [file.lat]\n");
            return 1;
        }
    }

    if (file)
        return run_file(file, show_stats);
    else
        repl();

    return 0;
}
