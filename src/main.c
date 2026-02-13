#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "eval.h"
#include "phase_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#ifndef __EMSCRIPTEN__
  #if defined(LATTICE_HAS_EDITLINE)
    #include <editline/readline.h>
  #elif defined(LATTICE_HAS_READLINE)
    #include <readline/readline.h>
    #include <readline/history.h>
  #else
    /* Minimal fallback: no line editing, no history */
    static char *readline(const char *prompt) {
        if (prompt) fputs(prompt, stdout);
        fflush(stdout);
        char *buf = malloc(4096);
        if (!buf) return NULL;
        if (!fgets(buf, 4096, stdin)) { free(buf); return NULL; }
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
        return buf;
    }
    static void add_history(const char *line) { (void)line; }
  #endif
#endif

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
static bool no_regions_mode = false;
static int  saved_argc = 0;
static char **saved_argv = NULL;

static int run_source(const char *source, bool show_stats, const char *script_dir) {
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
    if (no_regions_mode)
        evaluator_set_no_regions(ev, true);
    if (script_dir)
        evaluator_set_script_dir(ev, script_dir);
    evaluator_set_argv(ev, saved_argc, saved_argv);
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
    /* Extract directory of the script for require() resolution */
    char *path_copy = strdup(path);
    char *dir = dirname(path_copy);
    int result = run_source(source, show_stats, dir);
    free(path_copy);
    free(source);
    return result;
}

/* Check if input has balanced brackets/parens/braces.
 * Returns true if the input appears complete (balanced or has errors
 * that more input won't fix). Returns false if more input is needed. */
static bool input_is_complete(const char *source) {
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        /* Lexer error (e.g. unclosed string) means incomplete */
        free(lex_err);
        lat_vec_free(&tokens);
        return false;
    }
    int depth = 0;
    for (size_t i = 0; i < tokens.len; i++) {
        Token *t = lat_vec_get(&tokens, i);
        switch (t->type) {
            case TOK_LBRACE: case TOK_LPAREN: case TOK_LBRACKET:
                depth++;
                break;
            case TOK_RBRACE: case TOK_RPAREN: case TOK_RBRACKET:
                depth--;
                break;
            default:
                break;
        }
    }
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return depth <= 0;
}

static void repl(void) {
    printf("Lattice v%s — crystallization-based programming language\n", LATTICE_VERSION);
    printf("Copyright (c) 2026 Alex Jokela. BSD 3-Clause License.\n");
    printf("Type expressions to evaluate. Ctrl-D to exit.\n\n");

    Evaluator *ev = evaluator_new();
    if (gc_stress_mode)
        evaluator_set_gc_stress(ev, true);
    if (no_regions_mode)
        evaluator_set_no_regions(ev, true);
    evaluator_set_argv(ev, saved_argc, saved_argv);

    /* Keep programs alive so struct/fn/enum decl pointers stay valid */
    size_t prog_cap = 16, prog_count = 0;
    Program *kept_progs = malloc(prog_cap * sizeof(Program));
    size_t tok_cap = 16, tok_count = 0;
    LatVec *kept_tokens = malloc(tok_cap * sizeof(LatVec));

    char accumulated[65536];
    accumulated[0] = '\0';

    for (;;) {
        const char *prompt = (accumulated[0] == '\0') ? "lattice> " : "    ...> ";
        char *line = readline(prompt);
        if (!line) {
            printf("\n");
            break;
        }

        if (accumulated[0] != '\0')
            strcat(accumulated, "\n");
        strcat(accumulated, line);

        if (line[0] != '\0')
            add_history(line);
        free(line);

        if (!input_is_complete(accumulated))
            continue;

        /* Lex */
        Lexer lex = lexer_new(accumulated);
        char *lex_err = NULL;
        LatVec tokens = lexer_tokenize(&lex, &lex_err);
        if (lex_err) {
            fprintf(stderr, "error: %s\n", lex_err);
            free(lex_err);
            accumulated[0] = '\0';
            continue;
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
            accumulated[0] = '\0';
            continue;
        }

        /* Evaluate and capture result */
        EvalResult r = evaluator_run_repl_result(ev, &prog);
        if (!r.ok) {
            fprintf(stderr, "error: %s\n", r.error);
            free(r.error);
        } else if (r.value.type != VAL_UNIT && r.value.type != VAL_NIL) {
            char *repr = eval_repr(ev, &r.value);
            printf("=> %s\n", repr);
            free(repr);
            value_free(&r.value);
        } else {
            value_free(&r.value);
        }

        /* Keep program and tokens alive for struct/fn/enum declarations */
        if (prog_count >= prog_cap) {
            prog_cap *= 2;
            kept_progs = realloc(kept_progs, prog_cap * sizeof(Program));
        }
        kept_progs[prog_count++] = prog;
        if (tok_count >= tok_cap) {
            tok_cap *= 2;
            kept_tokens = realloc(kept_tokens, tok_cap * sizeof(LatVec));
        }
        kept_tokens[tok_count++] = tokens;

        accumulated[0] = '\0';
    }

    evaluator_free(ev);
    for (size_t i = 0; i < prog_count; i++)
        program_free(&kept_progs[i]);
    free(kept_progs);
    for (size_t i = 0; i < tok_count; i++) {
        for (size_t j = 0; j < kept_tokens[i].len; j++)
            token_free(lat_vec_get(&kept_tokens[i], j));
        lat_vec_free(&kept_tokens[i]);
    }
    free(kept_tokens);
}

static int run_test_file(const char *path) {
    char *source = read_file(path);
    if (!source) {
        fprintf(stderr, "error: cannot read '%s'\n", path);
        return 1;
    }
    char *path_copy = strdup(path);
    char *dir = dirname(path_copy);

    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        fprintf(stderr, "error: %s\n", lex_err);
        free(lex_err);
        lat_vec_free(&tokens);
        free(path_copy);
        free(source);
        return 1;
    }

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
        free(path_copy);
        free(source);
        return 1;
    }

    Evaluator *ev = evaluator_new();
    if (gc_stress_mode)
        evaluator_set_gc_stress(ev, true);
    if (no_regions_mode)
        evaluator_set_no_regions(ev, true);
    evaluator_set_script_dir(ev, dir);
    evaluator_set_argv(ev, saved_argc, saved_argv);

    int result = evaluator_run_tests(ev, &prog);

    evaluator_free(ev);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++)
        token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    free(path_copy);
    free(source);
    return result;
}

int main(int argc, char **argv) {
    saved_argc = argc;
    saved_argv = argv;
    bool show_stats = false;
    const char *file = NULL;

    /* Check for 'test' subcommand */
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        const char *test_path = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--gc-stress") == 0)
                gc_stress_mode = true;
            else if (strcmp(argv[i], "--no-regions") == 0)
                no_regions_mode = true;
            else if (!test_path)
                test_path = argv[i];
            else {
                fprintf(stderr, "usage: clat test [file.lat]\n");
                return 1;
            }
        }
        if (!test_path) {
            fprintf(stderr, "usage: clat test <file.lat>\n");
            return 1;
        }
        return run_test_file(test_path);
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stats") == 0)
            show_stats = true;
        else if (strcmp(argv[i], "--gc-stress") == 0)
            gc_stress_mode = true;
        else if (strcmp(argv[i], "--no-regions") == 0)
            no_regions_mode = true;
        else if (!file)
            file = argv[i];
        else {
            fprintf(stderr, "usage: clat [--stats] [--gc-stress] [--no-regions] [file.lat]\n");
            return 1;
        }
    }

    if (file)
        return run_file(file, show_stats);
    else
        repl();

    return 0;
}
