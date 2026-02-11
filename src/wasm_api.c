#ifdef __EMSCRIPTEN__

#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "eval.h"
#include "phase_check.h"
#include <emscripten.h>

static Evaluator *g_ev = NULL;

/* Keep parsed programs alive so struct/fn decls referenced by the evaluator
 * remain valid. We store them in a simple growable array. */
static Program *g_programs = NULL;
static LatVec  *g_token_vecs = NULL;
static size_t   g_prog_count = 0;
static size_t   g_prog_cap = 0;

static void store_program(Program prog, LatVec tokens) {
    if (g_prog_count == g_prog_cap) {
        g_prog_cap = g_prog_cap ? g_prog_cap * 2 : 16;
        g_programs = realloc(g_programs, g_prog_cap * sizeof(Program));
        g_token_vecs = realloc(g_token_vecs, g_prog_cap * sizeof(LatVec));
    }
    g_programs[g_prog_count] = prog;
    g_token_vecs[g_prog_count] = tokens;
    g_prog_count++;
}

static void free_stored_programs(void) {
    for (size_t i = 0; i < g_prog_count; i++) {
        program_free(&g_programs[i]);
        for (size_t j = 0; j < g_token_vecs[i].len; j++)
            token_free(lat_vec_get(&g_token_vecs[i], j));
        lat_vec_free(&g_token_vecs[i]);
    }
    free(g_programs);
    free(g_token_vecs);
    g_programs = NULL;
    g_token_vecs = NULL;
    g_prog_count = 0;
    g_prog_cap = 0;
}

EMSCRIPTEN_KEEPALIVE
void lat_init(void) {
    if (g_ev) {
        evaluator_free(g_ev);
        free_stored_programs();
    }
    g_ev = evaluator_new();
}

EMSCRIPTEN_KEEPALIVE
const char *lat_run_line(const char *source) {
    if (!g_ev) return "error: evaluator not initialized";

    /* Lex */
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        fprintf(stderr, "error: %s\n", lex_err);
        free(lex_err);
        lat_vec_free(&tokens);
        return NULL;
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
        return NULL;
    }

    /* Evaluate (REPL mode — no auto-main) */
    char *eval_err = evaluator_run_repl(g_ev, &prog);
    if (eval_err) {
        fprintf(stderr, "error: %s\n", eval_err);
        free(eval_err);
        /* Still store so any partial defs stay valid */
        store_program(prog, tokens);
        return NULL;
    }

    /* Keep program alive (struct/fn decls are referenced by pointer) */
    store_program(prog, tokens);
    return NULL;
}

EMSCRIPTEN_KEEPALIVE
int lat_is_complete(const char *source) {
    Lexer lex = lexer_new(source);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        lat_vec_free(&tokens);
        return 0;
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
    return depth <= 0 ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
void lat_destroy(void) {
    if (g_ev) {
        evaluator_free(g_ev);
        g_ev = NULL;
    }
    free_stored_programs();
}

#endif /* __EMSCRIPTEN__ */
