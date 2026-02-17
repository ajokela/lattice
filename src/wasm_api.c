#ifdef __EMSCRIPTEN__

#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "vm.h"
#include <emscripten.h>

static VM *g_vm = NULL;

/* Keep parsed programs alive so struct/fn/enum decl pointers referenced by
 * compiled chunks remain valid. */
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
    if (g_vm) {
        vm_free(g_vm);
        free(g_vm);
        compiler_free_known_enums();
        free_stored_programs();
    }
    /* Disconnect the fluid heap so the compiler and VM use plain malloc/free */
    value_set_heap(NULL);
    value_set_arena(NULL);

    g_vm = malloc(sizeof(VM));
    vm_init(g_vm);
}

EMSCRIPTEN_KEEPALIVE
const char *lat_run_line(const char *source) {
    if (!g_vm) return "error: VM not initialized";

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

    /* Compile for REPL (keeps last expression value on stack) */
    char *comp_err = NULL;
    Chunk *chunk = compile_repl(&prog, &comp_err);
    if (!chunk) {
        fprintf(stderr, "compile error: %s\n", comp_err);
        free(comp_err);
        store_program(prog, tokens);
        return NULL;
    }

    /* Run on VM */
    LatValue result;
    VMResult vm_res = vm_run(g_vm, chunk, &result);
    if (vm_res != VM_OK) {
        fprintf(stderr, "error: %s\n", g_vm->error);
        free(g_vm->error);
        g_vm->error = NULL;
        /* Reset VM state for next iteration */
        for (LatValue *slot = g_vm->stack; slot < g_vm->stack_top; slot++)
            value_free(slot);
        g_vm->stack_top = g_vm->stack;
        g_vm->frame_count = 0;
        g_vm->handler_count = 0;
        g_vm->defer_count = 0;
        while (g_vm->open_upvalues) {
            ObjUpvalue *uv = g_vm->open_upvalues;
            g_vm->open_upvalues = uv->next;
            uv->closed = *uv->location;
            uv->location = &uv->closed;
        }
    } else if (result.type != VAL_UNIT && result.type != VAL_NIL) {
        char *repr = value_repr(&result);
        printf("=> %s\n", repr);
        free(repr);
        value_free(&result);
    } else {
        value_free(&result);
    }

    chunk_free(chunk);

    /* Keep program alive (struct/fn/enum decls are referenced by pointer) */
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
    if (g_vm) {
        vm_free(g_vm);
        free(g_vm);
        g_vm = NULL;
    }
    compiler_free_known_enums();
    free_stored_programs();
}

#endif /* __EMSCRIPTEN__ */
