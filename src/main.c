#include "lattice.h"
#include "lexer.h"
#include "parser.h"
#include "eval.h"
#include "phase_check.h"
#include "match_check.h"
#include "stackcompiler.h"
#include "stackvm.h"
#include "latc.h"
#include "regvm.h"
#include "runtime.h"
#include "package.h"
#include "formatter.h"
#include "debugger.h"
#include "completion.h"
#include "doc_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "win32_compat.h"
#else
#include <libgen.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
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
    if (!fgets(buf, 4096, stdin)) {
        free(buf);
        return NULL;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
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
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static bool gc_mode = false;
static bool gc_stress_mode = false;
static bool no_regions_mode = false;
static bool no_assertions_mode = false;
static bool tree_walk_mode = false;
static bool regvm_mode = false;
static bool debug_mode = false;
static int debug_break_line = -1; /* Initial breakpoint line (-1 = none) */
static int saved_argc = 0;
static char **saved_argv = NULL;

static int run_source(const char *source, bool show_stats, const char *script_dir, const char *source_path) {
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
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
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
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            return 1;
        }
        lat_vec_free(&errors);
    }

    /* Match exhaustiveness check (always runs, warnings only) */
    check_match_exhaustiveness(&prog);

    /* Evaluate */
    Evaluator *ev = evaluator_new();
    if (gc_stress_mode) evaluator_set_gc_stress(ev, true);
    if (no_regions_mode) evaluator_set_no_regions(ev, true);
    if (no_assertions_mode) evaluator_set_assertions(ev, false);
    if (script_dir) evaluator_set_script_dir(ev, script_dir);
    evaluator_set_argv(ev, saved_argc, saved_argv);

    /* Tree-walk interpreter (legacy path) */
    if (tree_walk_mode) {
        char *eval_err = evaluator_run(ev, &prog);
        if (eval_err) {
            fprintf(stderr, "error: %s\n", eval_err);
            free(eval_err);
            evaluator_free(ev);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            return 1;
        }

        if (show_stats) {
            fprintf(stderr, "\n");
            memory_stats_print(evaluator_stats(ev), stderr);
        }

        evaluator_free(ev);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 0;
    }

    /* Register StackVM (POC) */
    if (regvm_mode) {
        value_set_heap(NULL);
        value_set_arena(NULL);

        char *rcomp_err = NULL;
        RegChunk *rchunk = reg_compile(&prog, &rcomp_err);
        if (!rchunk) {
            fprintf(stderr, "regvm compile error: %s\n", rcomp_err);
            free(rcomp_err);
            evaluator_free(ev);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            return 1;
        }

        LatRuntime rrt;
        lat_runtime_init(&rrt);
        if (script_dir) rrt.script_dir = strdup(script_dir);
        rrt.prog_argc = saved_argc;
        rrt.prog_argv = saved_argv;

        RegVM rvm;
        regvm_init(&rvm, &rrt);

        LatValue rresult;
        RegVMResult rvm_res = regvm_run(&rvm, rchunk, &rresult);
        if (rvm_res != REGVM_OK) {
            fprintf(stderr, "regvm error: %s\n", rvm.error);
            regvm_free(&rvm);
            lat_runtime_free(&rrt);
            regchunk_free(rchunk);
            evaluator_free(ev);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            return 1;
        }
        value_free(&rresult);
        regvm_free(&rvm);
        lat_runtime_free(&rrt);
        regchunk_free(rchunk);
        evaluator_free(ev);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 0;
    }

    /* Bytecode StackVM (default) */
    /* Disconnect the fluid heap so the compiler and StackVM use plain
     * malloc/free. The DualHeap doesn't support realloc, which
     * the StackVM needs for growing arrays. */
    value_set_heap(NULL);
    value_set_arena(NULL);

    char *comp_err = NULL;
    Chunk *chunk = stack_compile(&prog, &comp_err);
    if (!chunk) {
        fprintf(stderr, "compile error: %s\n", comp_err);
        free(comp_err);
        evaluator_free(ev);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }

    LatRuntime rt;
    lat_runtime_init(&rt);
    if (script_dir) rt.script_dir = strdup(script_dir);
    rt.prog_argc = saved_argc;
    rt.prog_argv = saved_argv;

    StackVM vm;
    stackvm_init(&vm, &rt);

    /* Attach debugger if --debug mode */
    Debugger *dbg = NULL;
    if (debug_mode) {
        dbg = debugger_new();
        if (dbg) {
            if (source_path) debugger_load_source(dbg, source_path);
            if (debug_break_line > 0) {
                debugger_add_breakpoint(dbg, debug_break_line);
                /* When a breakpoint is set, start in 'continue' mode */
                dbg->step_mode = false;
                dbg->running = true;
            }
            vm.debugger = dbg;
            fprintf(stderr, "Lattice debugger attached.\n");
            if (dbg->bp_count > 0) {
                fprintf(stderr, "Breakpoints:");
                for (size_t i = 0; i < dbg->bp_count; i++) fprintf(stderr, " line %d", dbg->breakpoints_line[i]);
                fprintf(stderr, "\n");
            } else {
                fprintf(stderr, "Stepping from first instruction. Type 'help' for commands.\n");
            }
        }
    }

    if (gc_mode || gc_stress_mode) {
        vm.gc.enabled = true;
        vm.gc.stress = gc_stress_mode;
    }
    LatValue result;
    StackVMResult vm_res = stackvm_run(&vm, chunk, &result);
    if (vm_res != STACKVM_OK) {
        fprintf(stderr, "vm error: %s\n", vm.error);
        stackvm_print_stack_trace(&vm);
        debugger_free(dbg);
        stackvm_free(&vm);
        lat_runtime_free(&rt);
        chunk_free(chunk);
        evaluator_free(ev);
        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        return 1;
    }
    value_free(&result);
    debugger_free(dbg);
    stackvm_free(&vm);
    lat_runtime_free(&rt);
    chunk_free(chunk);
    evaluator_free(ev);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return 0;
}

static bool has_suffix(const char *str, const char *suffix) {
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    return slen >= xlen && strcmp(str + slen - xlen, suffix) == 0;
}

static int run_latc_file(const char *path) {
    /* Read the raw file to inspect the magic number */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 1;
    }
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4) {
        fprintf(stderr, "error: '%s' is too small for a bytecode header\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* Auto-detect format: RegVM ("RLAT") vs StackVM ("LATC") */
    if (memcmp(magic, RLATC_MAGIC, 4) == 0) {
        char *err = NULL;
        RegChunk *rchunk = regchunk_load(path, &err);
        if (!rchunk) {
            fprintf(stderr, "error: %s\n", err);
            free(err);
            return 1;
        }

        value_set_heap(NULL);
        value_set_arena(NULL);

        LatRuntime rt;
        lat_runtime_init(&rt);
        rt.prog_argc = saved_argc;
        rt.prog_argv = saved_argv;

        RegVM rvm;
        regvm_init(&rvm, &rt);

        LatValue result;
        RegVMResult rvm_res = regvm_run(&rvm, rchunk, &result);
        if (rvm_res != REGVM_OK) {
            fprintf(stderr, "regvm error: %s\n", rvm.error);
            regvm_free(&rvm);
            lat_runtime_free(&rt);
            regchunk_free(rchunk);
            return 1;
        }
        value_free(&result);
        regvm_free(&rvm);
        lat_runtime_free(&rt);
        regchunk_free(rchunk);
        return 0;
    }

    /* Default: StackVM format */
    char *err = NULL;
    Chunk *chunk = chunk_load(path, &err);
    if (!chunk) {
        fprintf(stderr, "error: %s\n", err);
        free(err);
        return 1;
    }

    value_set_heap(NULL);
    value_set_arena(NULL);

    LatRuntime rt;
    lat_runtime_init(&rt);
    rt.prog_argc = saved_argc;
    rt.prog_argv = saved_argv;

    StackVM vm;
    stackvm_init(&vm, &rt);

    LatValue result;
    StackVMResult vm_res = stackvm_run(&vm, chunk, &result);
    if (vm_res != STACKVM_OK) {
        fprintf(stderr, "vm error: %s\n", vm.error);
        stackvm_print_stack_trace(&vm);
        stackvm_free(&vm);
        lat_runtime_free(&rt);
        chunk_free(chunk);
        return 1;
    }
    value_free(&result);
    stackvm_free(&vm);
    lat_runtime_free(&rt);
    chunk_free(chunk);
    return 0;
}

static int run_file(const char *path, bool show_stats) {
    /* Auto-detect .latc/.rlat pre-compiled bytecode */
    if (has_suffix(path, ".latc") || has_suffix(path, ".rlat")) return run_latc_file(path);

    char *source = read_file(path);
    if (!source) {
        fprintf(stderr, "error: cannot read '%s'\n", path);
        return 1;
    }
    /* Extract directory of the script for require() resolution */
    char *path_copy = strdup(path);
    char *dir = dirname(path_copy);
    int result = run_source(source, show_stats, dir, path);
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
            case TOK_LBRACE:
            case TOK_LPAREN:
            case TOK_LBRACKET: depth++; break;
            case TOK_RBRACE:
            case TOK_RPAREN:
            case TOK_RBRACKET: depth--; break;
            default: break;
        }
    }
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    return depth <= 0;
}

static void repl(void) {
    printf("Lattice v%s — crystallization-based programming language\n", LATTICE_VERSION);
    printf("Copyright (c) 2026 Alex Jokela. BSD 3-Clause License.\n");
    printf("Type expressions to evaluate. Ctrl-D to exit.\n\n");

    lattice_completion_init();

    /* Disconnect the fluid heap so the compiler and StackVM use plain malloc/free */
    value_set_heap(NULL);
    value_set_arena(NULL);

    LatRuntime rt;
    lat_runtime_init(&rt);
    rt.prog_argc = saved_argc;
    rt.prog_argv = saved_argv;

    StackVM vm;
    stackvm_init(&vm, &rt);

    /* Keep programs/tokens alive — scope/select store Expr* pointers in chunks */
    size_t prog_cap = 16, prog_count = 0;
    Program *kept_progs = malloc(prog_cap * sizeof(Program));
    if (!kept_progs) return;
    size_t tok_cap = 16, tok_count = 0;
    LatVec *kept_tokens = malloc(tok_cap * sizeof(LatVec));
    if (!kept_tokens) return;

    char accumulated[65536];
    accumulated[0] = '\0';

    for (;;) {
        const char *prompt = (accumulated[0] == '\0') ? "lattice> " : "    ...> ";
        char *line = readline(prompt);
        if (!line) {
            printf("\n");
            break;
        }

        if (accumulated[0] != '\0') strcat(accumulated, "\n");
        strcat(accumulated, line);

        if (line[0] != '\0') add_history(line);
        free(line);

        if (!input_is_complete(accumulated)) continue;

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
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            accumulated[0] = '\0';
            continue;
        }

        /* Compile for REPL (keeps last expression value on stack) */
        char *comp_err = NULL;
        Chunk *chunk = stack_compile_repl(&prog, &comp_err);
        if (!chunk) {
            fprintf(stderr, "compile error: %s\n", comp_err);
            free(comp_err);
            /* Keep program/tokens alive even on compile error */
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
            continue;
        }

        /* Run */
        LatValue result;
        StackVMResult vm_res = stackvm_run(&vm, chunk, &result);
        if (vm_res != STACKVM_OK) {
            fprintf(stderr, "error: %s\n", vm.error);
            stackvm_print_stack_trace(&vm);
            free(vm.error);
            vm.error = NULL;
            /* Reset StackVM state for next iteration */
            for (LatValue *slot = vm.stack; slot < vm.stack_top; slot++) value_free(slot);
            vm.stack_top = vm.stack;
            vm.frame_count = 0;
            vm.handler_count = 0;
            vm.defer_count = 0;
            /* Close any open upvalues */
            while (vm.open_upvalues) {
                ObjUpvalue *uv = vm.open_upvalues;
                vm.open_upvalues = uv->next;
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

        stackvm_track_chunk(&vm, chunk);

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

    stackvm_free(&vm);
    lat_runtime_free(&rt);
    stack_compiler_free_known_enums();
    for (size_t i = 0; i < prog_count; i++) program_free(&kept_progs[i]);
    free(kept_progs);
    for (size_t i = 0; i < tok_count; i++) {
        for (size_t j = 0; j < kept_tokens[i].len; j++) token_free(lat_vec_get(&kept_tokens[i], j));
        lat_vec_free(&kept_tokens[i]);
    }
    free(kept_tokens);
}

static void repl_regvm(void) {
    printf("Lattice v%s — crystallization-based programming language (regvm)\n", LATTICE_VERSION);
    printf("Copyright (c) 2026 Alex Jokela. BSD 3-Clause License.\n");
    printf("Type expressions to evaluate. Ctrl-D to exit.\n\n");

    lattice_completion_init();

    value_set_heap(NULL);
    value_set_arena(NULL);

    LatRuntime rt;
    lat_runtime_init(&rt);
    rt.prog_argc = saved_argc;
    rt.prog_argv = saved_argv;

    RegVM rvm;
    regvm_init(&rvm, &rt);

    /* Keep programs/tokens alive for struct/fn/enum lifetime */
    size_t prog_cap = 16, prog_count = 0;
    Program *kept_progs = malloc(prog_cap * sizeof(Program));
    if (!kept_progs) return;
    size_t tok_cap = 16, tok_count = 0;
    LatVec *kept_tokens = malloc(tok_cap * sizeof(LatVec));
    if (!kept_tokens) return;

    char accumulated[65536];
    accumulated[0] = '\0';

    for (;;) {
        const char *prompt = (accumulated[0] == '\0') ? "lattice> " : "    ...> ";
        char *line = readline(prompt);
        if (!line) {
            printf("\n");
            break;
        }

        if (accumulated[0] != '\0') strcat(accumulated, "\n");
        strcat(accumulated, line);

        if (line[0] != '\0') add_history(line);
        free(line);

        if (!input_is_complete(accumulated)) continue;

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
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            accumulated[0] = '\0';
            continue;
        }

        /* Compile for REPL */
        char *comp_err = NULL;
        RegChunk *chunk = reg_compile_repl(&prog, &comp_err);
        if (!chunk) {
            fprintf(stderr, "compile error: %s\n", comp_err);
            free(comp_err);
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
            continue;
        }

        /* Run */
        LatValue result;
        RegVMResult rvm_res = regvm_run_repl(&rvm, chunk, &result);
        if (rvm_res != REGVM_OK) {
            fprintf(stderr, "error: %s\n", rvm.error);
            free(rvm.error);
            rvm.error = NULL;
            /* Reset StackVM state for next iteration */
            for (size_t i = 0; i < rvm.reg_stack_top; i++) value_free_inline(&rvm.reg_stack[i]);
            rvm.reg_stack_top = 0;
            rvm.frame_count = 0;
            rvm.handler_count = 0;
            rvm.defer_count = 0;
        } else if (result.type != VAL_UNIT && result.type != VAL_NIL) {
            char *repr = value_repr(&result);
            printf("=> %s\n", repr);
            free(repr);
            value_free(&result);
        } else {
            value_free(&result);
        }

        /* Track chunk with StackVM (don't free — closures may reference sub-chunks) */
        regvm_track_chunk(&rvm, chunk);

        /* Keep program and tokens alive */
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

    regvm_free(&rvm);
    lat_runtime_free(&rt);
    reg_compiler_free_known_enums();
    for (size_t i = 0; i < prog_count; i++) program_free(&kept_progs[i]);
    free(kept_progs);
    for (size_t i = 0; i < tok_count; i++) {
        for (size_t j = 0; j < kept_tokens[i].len; j++) token_free(lat_vec_get(&kept_tokens[i], j));
        lat_vec_free(&kept_tokens[i]);
    }
    free(kept_tokens);
}

static void repl_tree_walk(void) {
    printf("Lattice v%s — crystallization-based programming language\n", LATTICE_VERSION);
    printf("Copyright (c) 2026 Alex Jokela. BSD 3-Clause License.\n");
    printf("Type expressions to evaluate. Ctrl-D to exit.\n\n");

    lattice_completion_init();

    Evaluator *ev = evaluator_new();
    if (gc_stress_mode) evaluator_set_gc_stress(ev, true);
    if (no_regions_mode) evaluator_set_no_regions(ev, true);
    if (no_assertions_mode) evaluator_set_assertions(ev, false);
    evaluator_set_argv(ev, saved_argc, saved_argv);

    /* Keep programs alive so struct/fn/enum decl pointers stay valid */
    size_t prog_cap = 16, prog_count = 0;
    Program *kept_progs = malloc(prog_cap * sizeof(Program));
    if (!kept_progs) return;
    size_t tok_cap = 16, tok_count = 0;
    LatVec *kept_tokens = malloc(tok_cap * sizeof(LatVec));
    if (!kept_tokens) return;

    char accumulated[65536];
    accumulated[0] = '\0';

    for (;;) {
        const char *prompt = (accumulated[0] == '\0') ? "lattice> " : "    ...> ";
        char *line = readline(prompt);
        if (!line) {
            printf("\n");
            break;
        }

        if (accumulated[0] != '\0') strcat(accumulated, "\n");
        strcat(accumulated, line);

        if (line[0] != '\0') add_history(line);
        free(line);

        if (!input_is_complete(accumulated)) continue;

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
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
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
    for (size_t i = 0; i < prog_count; i++) program_free(&kept_progs[i]);
    free(kept_progs);
    for (size_t i = 0; i < tok_count; i++) {
        for (size_t j = 0; j < kept_tokens[i].len; j++) token_free(lat_vec_get(&kept_tokens[i], j));
        lat_vec_free(&kept_tokens[i]);
    }
    free(kept_tokens);
}

static int run_test_file_ex(const char *path, const char *filter, bool verbose, bool print_file_header) {
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
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        free(path_copy);
        free(source);
        return 1;
    }

    /* Match exhaustiveness check */
    check_match_exhaustiveness(&prog);

    if (print_file_header) printf("Running tests in %s...\n", path);

    Evaluator *ev = evaluator_new();
    if (gc_stress_mode) evaluator_set_gc_stress(ev, true);
    if (no_regions_mode) evaluator_set_no_regions(ev, true);
    if (no_assertions_mode) evaluator_set_assertions(ev, false);
    evaluator_set_script_dir(ev, dir);
    evaluator_set_argv(ev, saved_argc, saved_argv);
    if (filter) evaluator_set_test_filter(ev, filter);
    if (verbose) evaluator_set_test_verbose(ev, true);

    int result = evaluator_run_tests(ev, &prog);

    evaluator_free(ev);
    program_free(&prog);
    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);
    free(path_copy);
    free(source);
    return result;
}

/* Check if a filename matches test file patterns: *_test.lat or test_*.lat */
static bool is_test_file(const char *name) {
    size_t len = strlen(name);
    if (len < 5) return false;
    /* Must end in .lat */
    if (strcmp(name + len - 4, ".lat") != 0) return false;
    /* Check for test_*.lat prefix */
    if (strncmp(name, "test_", 5) == 0) return true;
    /* Check for *_test.lat suffix */
    if (len >= 9 && strcmp(name + len - 9, "_test.lat") == 0) return true;
    return false;
}

/* Recursively discover test files in a directory */
static void discover_test_files(const char *dir_path, char ***files, size_t *count, size_t *cap) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* skip hidden files */
        char full_path[4096];
        size_t dlen = strlen(dir_path);
        const char *sep = (dlen > 0 && dir_path[dlen - 1] == '/') ? "" : "/";
        snprintf(full_path, sizeof(full_path), "%s%s%s", dir_path, sep, ent->d_name);
        /* Check if directory */
        struct stat st;
        if (stat(full_path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            discover_test_files(full_path, files, count, cap);
        } else if (S_ISREG(st.st_mode) && is_test_file(ent->d_name)) {
            if (*count >= *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *files = realloc(*files, *cap * sizeof(char *));
            }
            (*files)[*count] = strdup(full_path);
            (*count)++;
        }
    }
    closedir(dir);
}

/* Sort helper for qsort on string pointers */
static int cmp_strings(const void *a, const void *b) { return strcmp(*(const char **)a, *(const char **)b); }

int main(int argc, char **argv) {
    saved_argc = argc;
    saved_argv = argv;
    bool show_stats = false;
    const char *file = NULL;

    /* Check for 'compile' subcommand */
    if (argc >= 2 && strcmp(argv[1], "compile") == 0) {
        const char *input_path = NULL;
        const char *output_path = NULL;
        bool compile_regvm = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "error: -o requires an argument\n");
                    return 1;
                }
                output_path = argv[++i];
            } else if (strcmp(argv[i], "--regvm") == 0) {
                compile_regvm = true;
            } else if (!input_path) {
                input_path = argv[i];
            } else {
                fprintf(stderr, "usage: clat compile [--regvm] <file.lat> [-o output.latc]\n");
                return 1;
            }
        }
        if (!input_path) {
            fprintf(stderr, "usage: clat compile [--regvm] <file.lat> [-o output.latc]\n");
            return 1;
        }

        /* Build default output path:
         * --regvm: replace .lat with .rlat (or append .rlat)
         * stack VM: replace .lat with .latc (or append .latc) */
        const char *ext = compile_regvm ? ".rlat" : ".latc";
        char *default_output = NULL;
        if (!output_path) {
            size_t ilen = strlen(input_path);
            if (has_suffix(input_path, ".lat")) {
                /* Replace trailing ".lat" with the appropriate extension */
                size_t base_len = ilen - 4; /* strip ".lat" */
                size_t ext_len = strlen(ext);
                default_output = malloc(base_len + ext_len + 1);
                if (!default_output) return 1;
                memcpy(default_output, input_path, base_len);
                memcpy(default_output + base_len, ext, ext_len + 1);
            } else {
                size_t ext_len = strlen(ext);
                default_output = malloc(ilen + ext_len + 1);
                if (!default_output) return 1;
                memcpy(default_output, input_path, ilen);
                memcpy(default_output + ilen, ext, ext_len + 1);
            }
            output_path = default_output;
        }

        /* Lex → Parse → Compile */
        char *source = read_file(input_path);
        if (!source) {
            fprintf(stderr, "error: cannot read '%s'\n", input_path);
            free(default_output);
            return 1;
        }

        Lexer lex = lexer_new(source);
        char *lex_err = NULL;
        LatVec tokens = lexer_tokenize(&lex, &lex_err);
        if (lex_err) {
            fprintf(stderr, "error: %s\n", lex_err);
            free(lex_err);
            free(source);
            free(default_output);
            return 1;
        }

        Parser parser = parser_new(&tokens);
        char *parse_err = NULL;
        Program prog = parser_parse(&parser, &parse_err);
        if (parse_err) {
            fprintf(stderr, "error: %s\n", parse_err);
            free(parse_err);
            program_free(&prog);
            for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
            lat_vec_free(&tokens);
            free(source);
            free(default_output);
            return 1;
        }

        /* Match exhaustiveness check */
        check_match_exhaustiveness(&prog);

        value_set_heap(NULL);
        value_set_arena(NULL);

        if (compile_regvm) {
            /* RegVM compilation path */
            char *comp_err = NULL;
            RegChunk *rchunk = reg_compile(&prog, &comp_err);
            if (!rchunk) {
                fprintf(stderr, "regvm compile error: %s\n", comp_err);
                free(comp_err);
                program_free(&prog);
                for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
                lat_vec_free(&tokens);
                free(source);
                free(default_output);
                return 1;
            }

            /* Save RegVM bytecode */
            if (regchunk_save(rchunk, output_path) != 0) {
                fprintf(stderr, "error: cannot write '%s'\n", output_path);
                regchunk_free(rchunk);
                program_free(&prog);
                for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
                lat_vec_free(&tokens);
                free(source);
                free(default_output);
                return 1;
            }

            regchunk_free(rchunk);
        } else {
            /* StackVM compilation path (default) */
            char *comp_err = NULL;
            Chunk *chunk = stack_compile(&prog, &comp_err);
            if (!chunk) {
                fprintf(stderr, "compile error: %s\n", comp_err);
                free(comp_err);
                program_free(&prog);
                for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
                lat_vec_free(&tokens);
                free(source);
                free(default_output);
                return 1;
            }

            /* Save StackVM bytecode */
            if (chunk_save(chunk, output_path) != 0) {
                fprintf(stderr, "error: cannot write '%s'\n", output_path);
                chunk_free(chunk);
                program_free(&prog);
                for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
                lat_vec_free(&tokens);
                free(source);
                free(default_output);
                return 1;
            }

            chunk_free(chunk);
        }

        program_free(&prog);
        for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
        lat_vec_free(&tokens);
        free(source);
        free(default_output);
        return 0;
    }

    /* Check for 'test' subcommand */
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        const char *test_path = NULL;
        const char *filter_pattern = NULL;
        bool verbose_mode = false;
        bool summary_mode = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--gc-stress") == 0) gc_stress_mode = true;
            else if (strcmp(argv[i], "--no-regions") == 0) no_regions_mode = true;
            else if (strcmp(argv[i], "--no-assertions") == 0) no_assertions_mode = true;
            else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) verbose_mode = true;
            else if (strcmp(argv[i], "--summary") == 0) summary_mode = true;
            else if ((strcmp(argv[i], "--filter") == 0 || strcmp(argv[i], "-f") == 0) && i + 1 < argc)
                filter_pattern = argv[++i];
            else if (!test_path) test_path = argv[i];
            else {
                fprintf(stderr, "usage: clat test <file.lat|dir/> [--filter pattern] [--verbose] [--summary]\n");
                return 1;
            }
        }
        if (!test_path) {
            fprintf(stderr, "usage: clat test <file.lat|dir/> [--filter pattern] [--verbose] [--summary]\n");
            return 1;
        }

        /* Check if test_path is a directory */
        struct stat path_stat;
        if (stat(test_path, &path_stat) != 0) {
            fprintf(stderr, "error: cannot access '%s'\n", test_path);
            return 1;
        }

        if (S_ISDIR(path_stat.st_mode)) {
            /* Directory mode: discover and run test files */
            char **test_files = NULL;
            size_t file_count = 0, file_cap = 0;
            discover_test_files(test_path, &test_files, &file_count, &file_cap);
            if (file_count == 0) {
                printf("No test files found in '%s'.\n", test_path);
                printf("(Looking for *_test.lat or test_*.lat files)\n");
                free(test_files);
                return 0;
            }
            qsort(test_files, file_count, sizeof(char *), cmp_strings);

            printf("Discovered %zu test file%s in %s\n\n", file_count, file_count == 1 ? "" : "s", test_path);

            int total_exit = 0;
            size_t files_passed = 0, files_failed = 0;
            for (size_t i = 0; i < file_count; i++) {
                int result = run_test_file_ex(test_files[i], filter_pattern, verbose_mode, true);
                if (result != 0) {
                    files_failed++;
                    total_exit = 1;
                } else {
                    files_passed++;
                }
                if (i < file_count - 1) printf("\n");
            }

            if (summary_mode || file_count > 1) {
                printf("\n\033[1mOverall: %zu file%s passed, %zu failed (%zu total)\033[0m\n", files_passed,
                       files_passed == 1 ? "" : "s", files_failed, file_count);
            }

            for (size_t i = 0; i < file_count; i++) free(test_files[i]);
            free(test_files);
            return total_exit;
        }

        /* Single file mode */
        return run_test_file_ex(test_path, filter_pattern, verbose_mode, false);
    }

    /* Check for 'init' subcommand */
    if (argc >= 2 && strcmp(argv[1], "init") == 0) { return pkg_cmd_init(); }

    /* Check for 'install' subcommand */
    if (argc >= 2 && strcmp(argv[1], "install") == 0) { return pkg_cmd_install(); }

    /* Check for 'add' subcommand */
    if (argc >= 2 && strcmp(argv[1], "add") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: clat add <package> [version]\n");
            return 1;
        }
        const char *pkg_version = (argc >= 4) ? argv[3] : NULL;
        return pkg_cmd_add(argv[2], pkg_version);
    }

    /* Check for 'remove' subcommand */
    if (argc >= 2 && strcmp(argv[1], "remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: clat remove <package>\n");
            return 1;
        }
        return pkg_cmd_remove(argv[2]);
    }

    /* Check for 'fmt' subcommand */
    if (argc >= 2 && strcmp(argv[1], "fmt") == 0) {
        bool check_only = false;
        bool from_stdin = false;
        const char *fmt_path = NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--check") == 0) check_only = true;
            else if (strcmp(argv[i], "--stdin") == 0) from_stdin = true;
            else if (!fmt_path) fmt_path = argv[i];
            else {
                fprintf(stderr, "usage: clat fmt [--check] [--stdin] [file.lat]\n");
                return 1;
            }
        }

        if (from_stdin) {
            char *fmt_err = NULL;
            char *formatted = lat_format_stdin(&fmt_err);
            if (!formatted) {
                fprintf(stderr, "error: %s\n", fmt_err ? fmt_err : "format failed");
                free(fmt_err);
                return 1;
            }
            fputs(formatted, stdout);
            free(formatted);
            return 0;
        }

        if (!fmt_path) {
            fprintf(stderr, "usage: clat fmt [--check] [--stdin] <file.lat>\n");
            return 1;
        }

        char *source = read_file(fmt_path);
        if (!source) {
            fprintf(stderr, "error: cannot read '%s'\n", fmt_path);
            return 1;
        }

        if (check_only) {
            char *fmt_err = NULL;
            bool ok = lat_format_check(source, &fmt_err);
            free(source);
            if (fmt_err) {
                fprintf(stderr, "error: %s\n", fmt_err);
                free(fmt_err);
                return 1;
            }
            if (!ok) {
                fprintf(stderr, "%s: not formatted\n", fmt_path);
                return 1;
            }
            return 0;
        }

        char *fmt_err = NULL;
        char *formatted = lat_format(source, &fmt_err);
        free(source);
        if (!formatted) {
            fprintf(stderr, "error: %s\n", fmt_err ? fmt_err : "format failed");
            free(fmt_err);
            return 1;
        }

        /* Write back in-place */
        FILE *out = fopen(fmt_path, "w");
        if (!out) {
            fprintf(stderr, "error: cannot write '%s'\n", fmt_path);
            free(formatted);
            return 1;
        }
        fputs(formatted, out);
        fclose(out);
        free(formatted);
        return 0;
    }

    /* Check for 'doc' subcommand */
    if (argc >= 2 && strcmp(argv[1], "doc") == 0) { return doc_cmd(argc - 2, argv + 2); }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stats") == 0) show_stats = true;
        else if (strcmp(argv[i], "--gc") == 0) gc_mode = true;
        else if (strcmp(argv[i], "--gc-stress") == 0) {
            gc_mode = true;
            gc_stress_mode = true;
        } else if (strcmp(argv[i], "--no-gc") == 0) gc_mode = false;
        else if (strcmp(argv[i], "--no-regions") == 0) no_regions_mode = true;
        else if (strcmp(argv[i], "--no-assertions") == 0) no_assertions_mode = true;
        else if (strcmp(argv[i], "--tree-walk") == 0) tree_walk_mode = true;
        else if (strcmp(argv[i], "--regvm") == 0) regvm_mode = true;
        else if (strcmp(argv[i], "--debug") == 0) debug_mode = true;
        else if (strcmp(argv[i], "--break") == 0) {
            if (i + 1 < argc) {
                debug_break_line = atoi(argv[++i]);
                debug_mode = true;
            } else {
                fprintf(stderr, "error: --break requires a line number\n");
                return 1;
            }
        } else if (!file) {
            file = argv[i];
            /* Remaining args after filename are passed to the script via args() */
            saved_argc = argc - i;
            saved_argv = argv + i;
            break;
        }
    }

    if (file) return run_file(file, show_stats);
    else if (tree_walk_mode) repl_tree_walk();
    else if (regvm_mode) repl_regvm();
    else repl();

    return 0;
}
