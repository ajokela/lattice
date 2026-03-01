#include "lsp.h"
#include "lexer.h"
#include "parser.h"
#include "stackcompiler.h"
#include "chunk.h"
#include "ast.h"
#include "token.h"
#include "ds/vec.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Parse "line:col: message" format from lexer/parser errors.
 * line/col are 1-based in Lattice; convert to 0-based for LSP. */
static LspDiagnostic parse_error(const char *err_msg) {
    LspDiagnostic d = {0};
    d.severity = LSP_DIAG_ERROR;

    size_t line = 0, col = 0;
    const char *msg = err_msg;

    /* Try "N:N: message" */
    if (err_msg) {
        char *end1 = NULL;
        unsigned long v1 = strtoul(err_msg, &end1, 10);
        if (end1 && *end1 == ':') {
            line = v1;
            char *end2 = NULL;
            unsigned long v2 = strtoul(end1 + 1, &end2, 10);
            if (end2 && *end2 == ':') {
                col = v2;
                msg = end2 + 1;
                while (*msg == ' ') msg++;
            }
        }
    }

    d.line = line > 0 ? (int)line - 1 : 0;
    d.col = col > 0 ? (int)col - 1 : 0;
    d.message = strdup(msg ? msg : err_msg);
    return d;
}

/* Find the 0-based line and column of a keyword+name pattern in the text.
 * Searches for "<keyword> <name>" and returns the position of <name>.
 * search_from is 0-based line to start searching from (avoids earlier matches). */
static void find_decl_position(const char *text, const char *keyword, const char *name, int search_from, int *out_line,
                               int *out_col) {
    *out_line = 0;
    *out_col = 0;

    size_t kw_len = strlen(keyword);
    size_t name_len = strlen(name);
    const char *p = text;
    int line = 0;

    /* Advance to search_from line */
    while (line < search_from && *p) {
        if (*p == '\n') line++;
        p++;
    }

    while (*p) {
        const char *line_start = p;
        /* Look for keyword at current position (allowing leading whitespace) */
        const char *kw = strstr(p, keyword);
        if (!kw) break;

        /* Count lines to this point */
        while (p < kw) {
            if (*p == '\n') {
                line++;
                line_start = p + 1;
            }
            p++;
        }

        /* Check that keyword is followed by space then the name */
        const char *after_kw = kw + kw_len;
        if (*after_kw == ' ' || *after_kw == '\t') {
            after_kw++;
            while (*after_kw == ' ' || *after_kw == '\t') after_kw++;
            if (strncmp(after_kw, name, name_len) == 0) {
                char ch_after = after_kw[name_len];
                if (ch_after == '(' || ch_after == ' ' || ch_after == '\t' || ch_after == '{' || ch_after == '\n' ||
                    ch_after == '\r' || ch_after == '\0' || ch_after == ':') {
                    *out_line = line;
                    *out_col = (int)(after_kw - line_start);
                    return;
                }
            }
        }
        p = kw + 1;
    }
}

/* Extract symbols and struct/enum definitions from parsed AST */
static void extract_symbols(LspDocument *doc, const Program *prog) {
    int last_line = 0; /* Track search position to handle duplicate names */

    for (size_t i = 0; i < prog->item_count; i++) {
        const Item *item = &prog->items[i];
        LspSymbol sym = {0};
        bool found = false;

        switch (item->tag) {
            case ITEM_FUNCTION: {
                const FnDecl *fn = &item->as.fn_decl;
                sym.name = strdup(fn->name);
                sym.kind = LSP_SYM_FUNCTION;
                /* Build signature */
                size_t siglen = strlen(fn->name) + 32;
                for (size_t j = 0; j < fn->param_count; j++) siglen += strlen(fn->params[j].name) + 16;
                sym.signature = malloc(siglen);
                if (!sym.signature) return;
                char *p = sym.signature;
                char *end = sym.signature + siglen;
                p += snprintf(p, (size_t)(end - p), "fn %s(", fn->name);
                for (size_t j = 0; j < fn->param_count; j++) {
                    if (j > 0) p += snprintf(p, (size_t)(end - p), ", ");
                    p += snprintf(p, (size_t)(end - p), "%s", fn->params[j].name);
                    if (fn->params[j].ty.name) p += snprintf(p, (size_t)(end - p), ": %s", fn->params[j].ty.name);
                }
                snprintf(p, (size_t)(end - p), ")");
                find_decl_position(doc->text, "fn", fn->name, last_line, &sym.line, &sym.col);
                found = true;
                break;
            }
            case ITEM_STRUCT: {
                const StructDecl *sd = &item->as.struct_decl;
                sym.name = strdup(sd->name);
                sym.kind = LSP_SYM_STRUCT;
                sym.signature = malloc(strlen(sd->name) + 16);
                if (!sym.signature) return;
                snprintf(sym.signature, strlen(sd->name) + 16, "struct %s", sd->name);
                find_decl_position(doc->text, "struct", sd->name, last_line, &sym.line, &sym.col);

                /* Extract struct field info for completion */
                LspStructDef sdef;
                sdef.name = strdup(sd->name);
                sdef.line = sym.line;
                sdef.field_count = sd->field_count;
                sdef.fields = NULL;
                if (sd->field_count > 0) {
                    sdef.fields = malloc(sd->field_count * sizeof(LspFieldInfo));
                    if (!sdef.fields) return;
                    for (size_t j = 0; j < sd->field_count; j++) {
                        sdef.fields[j].name = strdup(sd->fields[j].name);
                        sdef.fields[j].type_name = sd->fields[j].ty.name ? strdup(sd->fields[j].ty.name) : NULL;
                    }
                }
                doc->struct_def_count++;
                doc->struct_defs = realloc(doc->struct_defs, doc->struct_def_count * sizeof(LspStructDef));
                doc->struct_defs[doc->struct_def_count - 1] = sdef;

                found = true;
                break;
            }
            case ITEM_ENUM: {
                const EnumDecl *ed = &item->as.enum_decl;
                sym.name = strdup(ed->name);
                sym.kind = LSP_SYM_ENUM;
                sym.signature = malloc(strlen(ed->name) + 16);
                if (!sym.signature) return;
                snprintf(sym.signature, strlen(ed->name) + 16, "enum %s", ed->name);
                find_decl_position(doc->text, "enum", ed->name, last_line, &sym.line, &sym.col);

                /* Extract enum variant info for completion */
                LspEnumDef edef;
                edef.name = strdup(ed->name);
                edef.line = sym.line;
                edef.variant_count = ed->variant_count;
                edef.variants = NULL;
                if (ed->variant_count > 0) {
                    edef.variants = malloc(ed->variant_count * sizeof(LspVariantInfo));
                    if (!edef.variants) return;
                    for (size_t j = 0; j < ed->variant_count; j++) {
                        edef.variants[j].name = strdup(ed->variants[j].name);
                        /* Build param string for tuple variants */
                        if (ed->variants[j].param_count > 0) {
                            size_t plen = 3; /* "()\0" */
                            for (size_t k = 0; k < ed->variants[j].param_count; k++) {
                                if (ed->variants[j].param_types[k].name)
                                    plen += strlen(ed->variants[j].param_types[k].name) + 2;
                                else plen += 5; /* "Any, " */
                            }
                            char *params = malloc(plen);
                            if (!params) return;
                            char *pp = params;
                            *pp++ = '(';
                            for (size_t k = 0; k < ed->variants[j].param_count; k++) {
                                if (k > 0) {
                                    *pp++ = ',';
                                    *pp++ = ' ';
                                }
                                const char *tn = ed->variants[j].param_types[k].name;
                                if (!tn) tn = "Any";
                                size_t tlen = strlen(tn);
                                memcpy(pp, tn, tlen);
                                pp += tlen;
                            }
                            *pp++ = ')';
                            *pp = '\0';
                            edef.variants[j].params = params;
                        } else {
                            edef.variants[j].params = NULL;
                        }
                    }
                }
                doc->enum_def_count++;
                doc->enum_defs = realloc(doc->enum_defs, doc->enum_def_count * sizeof(LspEnumDef));
                doc->enum_defs[doc->enum_def_count - 1] = edef;

                found = true;
                break;
            }
            case ITEM_STMT: {
                const Stmt *stmt = item->as.stmt;
                if (stmt && stmt->tag == STMT_BINDING && stmt->as.binding.name) {
                    sym.name = strdup(stmt->as.binding.name);
                    sym.kind = LSP_SYM_VARIABLE;

                    /* Build signature with phase and optional type */
                    const char *phase_kw = "let";
                    if (stmt->as.binding.phase == PHASE_FLUID) phase_kw = "flux";
                    else if (stmt->as.binding.phase == PHASE_CRYSTAL) phase_kw = "fix";

                    size_t siglen = strlen(phase_kw) + strlen(stmt->as.binding.name) + 32;
                    if (stmt->as.binding.ty && stmt->as.binding.ty->name) siglen += strlen(stmt->as.binding.ty->name);
                    sym.signature = malloc(siglen);
                    if (!sym.signature) return;

                    if (stmt->as.binding.ty && stmt->as.binding.ty->name)
                        snprintf(sym.signature, siglen, "%s %s: %s", phase_kw, stmt->as.binding.name,
                                 stmt->as.binding.ty->name);
                    else snprintf(sym.signature, siglen, "%s %s", phase_kw, stmt->as.binding.name);

                    /* Use the AST line number (1-based) converted to 0-based */
                    sym.line = stmt->line > 0 ? stmt->line - 1 : 0;
                    /* Find exact column for the name */
                    find_decl_position(doc->text, phase_kw, stmt->as.binding.name, sym.line, &sym.line, &sym.col);
                    found = true;
                }
                break;
            }
            case ITEM_TRAIT: {
                const TraitDecl *td = &item->as.trait_decl;
                sym.name = strdup(td->name);
                sym.kind = LSP_SYM_STRUCT; /* Use Struct kind — closest LSP match */
                size_t siglen = strlen(td->name) + 32;
                for (size_t j = 0; j < td->method_count; j++) siglen += strlen(td->methods[j].name) + 8;
                sym.signature = malloc(siglen);
                if (!sym.signature) return;
                char *p = sym.signature;
                char *end = sym.signature + siglen;
                p += snprintf(p, (size_t)(end - p), "trait %s {", td->name);
                for (size_t j = 0; j < td->method_count; j++) {
                    if (j > 0) p += snprintf(p, (size_t)(end - p), ",");
                    p += snprintf(p, (size_t)(end - p), " %s()", td->methods[j].name);
                }
                snprintf(p, (size_t)(end - p), " }");
                find_decl_position(doc->text, "trait", td->name, last_line, &sym.line, &sym.col);
                found = true;
                break;
            }
            case ITEM_IMPL: {
                const ImplBlock *ib = &item->as.impl_block;
                /* Build name like "impl Trait for Type" */
                size_t nlen = 16;
                if (ib->trait_name) nlen += strlen(ib->trait_name);
                if (ib->type_name) nlen += strlen(ib->type_name);
                sym.name = malloc(nlen);
                if (!sym.name) return;
                if (ib->trait_name && ib->type_name)
                    snprintf(sym.name, nlen, "%s for %s", ib->trait_name, ib->type_name);
                else if (ib->type_name) snprintf(sym.name, nlen, "%s", ib->type_name);
                else snprintf(sym.name, nlen, "impl");
                sym.kind = LSP_SYM_METHOD;
                size_t sig_size = strlen(sym.name) + 8;
                sym.signature = malloc(sig_size);
                if (!sym.signature) return;
                snprintf(sym.signature, sig_size, "impl %s", sym.name);
                find_decl_position(doc->text, "impl", ib->trait_name ? ib->trait_name : ib->type_name, last_line,
                                   &sym.line, &sym.col);

                /* Extract individual methods from the impl block */
                if (ib->type_name && ib->method_count > 0) {
                    for (size_t j = 0; j < ib->method_count; j++) {
                        const FnDecl *fn = &ib->methods[j];
                        LspImplMethod im;
                        im.type_name = strdup(ib->type_name);
                        im.method_name = strdup(fn->name);

                        /* Build signature like "fn distance(self: Point, other: Point)" */
                        size_t msiglen = strlen(fn->name) + 32;
                        for (size_t k = 0; k < fn->param_count; k++) msiglen += strlen(fn->params[k].name) + 16;
                        im.signature = malloc(msiglen);
                        if (im.signature) {
                            char *p = im.signature;
                            char *end = im.signature + msiglen;
                            p += snprintf(p, (size_t)(end - p), "fn %s(", fn->name);
                            for (size_t k = 0; k < fn->param_count; k++) {
                                if (k > 0) p += snprintf(p, (size_t)(end - p), ", ");
                                p += snprintf(p, (size_t)(end - p), "%s", fn->params[k].name);
                                if (fn->params[k].ty.name)
                                    p += snprintf(p, (size_t)(end - p), ": %s", fn->params[k].ty.name);
                            }
                            snprintf(p, (size_t)(end - p), ")");
                        }

                        /* Find method position in text */
                        find_decl_position(doc->text, "fn", fn->name, sym.line, &im.line, &(int){0});

                        doc->impl_method_count++;
                        doc->impl_methods = realloc(doc->impl_methods, doc->impl_method_count * sizeof(LspImplMethod));
                        doc->impl_methods[doc->impl_method_count - 1] = im;
                    }
                }

                found = true;
                break;
            }
            default: break;
        }

        if (found) {
            last_line = sym.line;
            sym.owner_type = NULL;
            doc->symbol_count++;
            doc->symbols = realloc(doc->symbols, doc->symbol_count * sizeof(LspSymbol));
            doc->symbols[doc->symbol_count - 1] = sym;
        }
    }
}

/* Free tokens helper */
static void free_tokens(LatVec *tokens) {
    for (size_t i = 0; i < tokens->len; i++) token_free(lat_vec_get(tokens, i));
    lat_vec_free(tokens);
}

/* Free struct def info */
static void free_struct_defs(LspStructDef *defs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(defs[i].name);
        for (size_t j = 0; j < defs[i].field_count; j++) {
            free(defs[i].fields[j].name);
            free(defs[i].fields[j].type_name);
        }
        free(defs[i].fields);
    }
    free(defs);
}

/* Free impl method info */
static void free_impl_methods(LspImplMethod *methods, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(methods[i].type_name);
        free(methods[i].method_name);
        free(methods[i].signature);
    }
    free(methods);
}

/* Free enum def info */
static void free_enum_defs(LspEnumDef *defs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(defs[i].name);
        for (size_t j = 0; j < defs[i].variant_count; j++) {
            free(defs[i].variants[j].name);
            free(defs[i].variants[j].params);
        }
        free(defs[i].variants);
    }
    free(defs);
}

/* ── Folding Ranges ── */

/* Extract folding ranges by scanning for brace blocks and block comments.
 * Handles string literals (including triple-quoted and interpolated) to
 * avoid counting braces inside strings. */
static void extract_folding_ranges(LspDocument *doc) {
    if (!doc->text) return;

    /* Brace stack: track the line number where each '{' was found */
    int brace_stack[256];
    int brace_depth = 0;

    const char *p = doc->text;
    int line = 0;

    while (*p) {
        /* Skip string literals */
        if (*p == '"') {
            /* Check for triple-quoted string """ */
            if (p[1] == '"' && p[2] == '"') {
                p += 3;
                while (*p) {
                    if (*p == '"' && p[1] == '"' && p[2] == '"') {
                        p += 3;
                        break;
                    }
                    if (*p == '\n') line++;
                    p++;
                }
                continue;
            }
            /* Regular string — handle ${...} interpolation */
            p++;
            while (*p && *p != '"' && *p != '\n') {
                if (*p == '\\') {
                    p++;
                    if (*p) p++;
                    continue;
                }
                if (*p == '$' && p[1] == '{') {
                    /* Skip interpolation — track nested braces */
                    p += 2;
                    int depth = 1;
                    while (*p && depth > 0) {
                        if (*p == '{') depth++;
                        else if (*p == '}') depth--;
                        if (*p == '\n') line++;
                        if (depth > 0) p++;
                    }
                    if (*p == '}') p++;
                    continue;
                }
                p++;
            }
            if (*p == '"') p++;
            continue;
        }

        /* Skip line comments */
        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Block comments — potential folding range */
        if (*p == '/' && p[1] == '*') {
            int start_line = line;
            p += 2;
            while (*p) {
                if (*p == '*' && p[1] == '/') {
                    p += 2;
                    break;
                }
                if (*p == '\n') line++;
                p++;
            }
            /* Emit folding range if multi-line */
            if (line > start_line) {
                doc->folding_range_count++;
                doc->folding_ranges = realloc(doc->folding_ranges, doc->folding_range_count * sizeof(LspFoldingRange));
                LspFoldingRange *fr = &doc->folding_ranges[doc->folding_range_count - 1];
                fr->start_line = start_line;
                fr->end_line = line;
                fr->kind = "comment";
            }
            continue;
        }

        if (*p == '{') {
            if (brace_depth < 256) {
                brace_stack[brace_depth] = line;
                brace_depth++;
            }
        } else if (*p == '}') {
            if (brace_depth > 0) {
                brace_depth--;
                int start_line = brace_stack[brace_depth];
                if (line > start_line) {
                    doc->folding_range_count++;
                    doc->folding_ranges =
                        realloc(doc->folding_ranges, doc->folding_range_count * sizeof(LspFoldingRange));
                    LspFoldingRange *fr = &doc->folding_ranges[doc->folding_range_count - 1];
                    fr->start_line = start_line;
                    fr->end_line = line;
                    fr->kind = "region";
                }
            }
        }

        if (*p == '\n') line++;
        p++;
    }
}

/* ── Semantic Tokens ── */

/* Check if a token type is a keyword */
static bool is_keyword_token(TokenType type) {
    switch (type) {
        case TOK_FLUX:
        case TOK_FIX:
        case TOK_LET:
        case TOK_FREEZE:
        case TOK_THAW:
        case TOK_FORGE:
        case TOK_FN:
        case TOK_STRUCT:
        case TOK_IF:
        case TOK_ELSE:
        case TOK_FOR:
        case TOK_IN:
        case TOK_WHILE:
        case TOK_LOOP:
        case TOK_RETURN:
        case TOK_BREAK:
        case TOK_CONTINUE:
        case TOK_SPAWN:
        case TOK_TRUE:
        case TOK_FALSE:
        case TOK_NIL:
        case TOK_CLONE:
        case TOK_ANNEAL:
        case TOK_PRINT:
        case TOK_TRY:
        case TOK_CATCH:
        case TOK_SCOPE:
        case TOK_TEST:
        case TOK_MATCH:
        case TOK_ENUM:
        case TOK_IMPORT:
        case TOK_FROM:
        case TOK_AS:
        case TOK_CRYSTALLIZE:
        case TOK_BORROW:
        case TOK_SUBLIMATE:
        case TOK_DEFER:
        case TOK_SELECT:
        case TOK_TRAIT:
        case TOK_IMPL:
        case TOK_EXPORT: return true;
        default: return false;
    }
}

/* Get the text length of a keyword token */
static int keyword_token_length(TokenType type) {
    switch (type) {
        case TOK_FN: return 2;
        case TOK_IF:
        case TOK_IN:
        case TOK_AS: return 2;
        case TOK_FIX:
        case TOK_LET:
        case TOK_FOR:
        case TOK_NIL:
        case TOK_TRY: return 3;
        case TOK_FLUX:
        case TOK_THAW:
        case TOK_ELSE:
        case TOK_LOOP:
        case TOK_FROM:
        case TOK_IMPL:
        case TOK_ENUM:
        case TOK_TRUE:
        case TOK_TEST: return 4;
        case TOK_FORGE:
        case TOK_WHILE:
        case TOK_BREAK:
        case TOK_CLONE:
        case TOK_CATCH:
        case TOK_SCOPE:
        case TOK_MATCH:
        case TOK_PRINT:
        case TOK_SPAWN:
        case TOK_DEFER:
        case TOK_TRAIT:
        case TOK_FALSE: return 5;
        case TOK_STRUCT:
        case TOK_RETURN:
        case TOK_FREEZE:
        case TOK_IMPORT:
        case TOK_SELECT:
        case TOK_ANNEAL:
        case TOK_BORROW:
        case TOK_EXPORT: return 6;
        case TOK_CONTINUE: return 8;
        case TOK_SUBLIMATE: return 10;
        case TOK_CRYSTALLIZE: return 12;
        default: return 0;
    }
}

/* Check if a token type is an operator */
static bool is_operator_token(TokenType type) {
    switch (type) {
        case TOK_PLUS:
        case TOK_MINUS:
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT:
        case TOK_EQ:
        case TOK_EQEQ:
        case TOK_BANGEQ:
        case TOK_LT:
        case TOK_GT:
        case TOK_LTEQ:
        case TOK_GTEQ:
        case TOK_AND:
        case TOK_OR:
        case TOK_BANG:
        case TOK_DOTDOT:
        case TOK_ARROW:
        case TOK_FATARROW:
        case TOK_PIPE:
        case TOK_AMPERSAND:
        case TOK_CARET:
        case TOK_LSHIFT:
        case TOK_RSHIFT:
        case TOK_PLUS_EQ:
        case TOK_MINUS_EQ:
        case TOK_STAR_EQ:
        case TOK_SLASH_EQ:
        case TOK_PERCENT_EQ:
        case TOK_AMP_EQ:
        case TOK_PIPE_EQ:
        case TOK_CARET_EQ:
        case TOK_LSHIFT_EQ:
        case TOK_RSHIFT_EQ:
        case TOK_QUESTION:
        case TOK_QUESTION_QUESTION: return true;
        default: return false;
    }
}

/* Operator token text length */
static int operator_token_length(TokenType type) {
    switch (type) {
        case TOK_PLUS:
        case TOK_MINUS:
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT:
        case TOK_EQ:
        case TOK_LT:
        case TOK_GT:
        case TOK_BANG:
        case TOK_PIPE:
        case TOK_AMPERSAND:
        case TOK_CARET:
        case TOK_QUESTION: return 1;
        case TOK_EQEQ:
        case TOK_BANGEQ:
        case TOK_LTEQ:
        case TOK_GTEQ:
        case TOK_AND:
        case TOK_OR:
        case TOK_DOTDOT:
        case TOK_ARROW:
        case TOK_FATARROW:
        case TOK_LSHIFT:
        case TOK_RSHIFT:
        case TOK_PLUS_EQ:
        case TOK_MINUS_EQ:
        case TOK_STAR_EQ:
        case TOK_SLASH_EQ:
        case TOK_PERCENT_EQ:
        case TOK_AMP_EQ:
        case TOK_PIPE_EQ:
        case TOK_CARET_EQ:
        case TOK_QUESTION_QUESTION: return 2;
        case TOK_LSHIFT_EQ:
        case TOK_RSHIFT_EQ: return 3;
        default: return 1;
    }
}

/* A semantic token entry before delta encoding */
typedef struct {
    int line; /* 0-based */
    int col;  /* 0-based */
    int length;
    int type; /* LSP_SEMTOK_* */
} SemTokEntry;

/* Compare semantic token entries by position */
static int semtok_cmp(const void *a, const void *b) {
    const SemTokEntry *sa = a, *sb = b;
    if (sa->line != sb->line) return sa->line - sb->line;
    return sa->col - sb->col;
}

/* Extract semantic tokens by re-lexing and classifying tokens, plus scanning for comments. */
static void extract_semantic_tokens(LspDocument *doc) {
    if (!doc->text || !*doc->text) return;

    /* Phase 1: Scan for comments and record them as entries */
    size_t entry_cap = 256;
    size_t entry_count = 0;
    SemTokEntry *entries = malloc(entry_cap * sizeof(SemTokEntry));
    if (!entries) return;

    const char *p = doc->text;
    int line = 0, col = 0;

    while (*p) {
        if (*p == '"') {
            /* Skip strings */
            if (p[1] == '"' && p[2] == '"') {
                p += 3;
                col += 3;
                while (*p) {
                    if (*p == '"' && p[1] == '"' && p[2] == '"') {
                        p += 3;
                        col += 3;
                        break;
                    }
                    if (*p == '\n') {
                        line++;
                        col = 0;
                    } else col++;
                    p++;
                }
                continue;
            }
            p++;
            col++;
            while (*p && *p != '"' && *p != '\n') {
                if (*p == '\\') {
                    p++;
                    col++;
                    if (*p) {
                        p++;
                        col++;
                    }
                    continue;
                }
                if (*p == '$' && p[1] == '{') {
                    p += 2;
                    col += 2;
                    int depth = 1;
                    while (*p && depth > 0) {
                        if (*p == '{') depth++;
                        else if (*p == '}') depth--;
                        if (*p == '\n') {
                            line++;
                            col = 0;
                        } else col++;
                        if (depth > 0) p++;
                    }
                    if (*p == '}') {
                        p++;
                        col++;
                    }
                    continue;
                }
                p++;
                col++;
            }
            if (*p == '"') {
                p++;
                col++;
            }
            continue;
        }

        if (*p == '/' && p[1] == '/') {
            int start_col = col;
            int len = 0;
            while (*p && *p != '\n') {
                p++;
                col++;
                len++;
            }
            if (entry_count >= entry_cap) {
                entry_cap *= 2;
                entries = realloc(entries, entry_cap * sizeof(SemTokEntry));
            }
            entries[entry_count++] = (SemTokEntry){line, start_col, len, LSP_SEMTOK_COMMENT};
            continue;
        }

        if (*p == '/' && p[1] == '*') {
            int start_line = line, start_col = col;
            p += 2;
            col += 2;
            while (*p) {
                if (*p == '*' && p[1] == '/') {
                    p += 2;
                    col += 2;
                    break;
                }
                if (*p == '\n') {
                    line++;
                    col = 0;
                } else col++;
                p++;
            }
            /* For multi-line comments, emit one token per line */
            if (line > start_line) {
                /* First line: from start_col to end of line */
                const char *scan = doc->text;
                int sl = 0;
                while (sl < start_line && *scan) {
                    if (*scan == '\n') sl++;
                    scan++;
                }
                int first_len = 0;
                const char *ls = scan;
                while (*ls && *ls != '\n') {
                    ls++;
                    first_len++;
                }
                first_len -= start_col;
                if (first_len > 0) {
                    if (entry_count >= entry_cap) {
                        entry_cap *= 2;
                        entries = realloc(entries, entry_cap * sizeof(SemTokEntry));
                    }
                    entries[entry_count++] = (SemTokEntry){start_line, start_col, first_len, LSP_SEMTOK_COMMENT};
                }
                /* Middle + last lines */
                for (int cl = start_line + 1; cl <= line; cl++) {
                    if (*scan == '\n') scan++;
                    const char *le = scan;
                    int ll = 0;
                    while (*le && *le != '\n') {
                        le++;
                        ll++;
                    }
                    if (ll > 0) {
                        if (entry_count >= entry_cap) {
                            entry_cap *= 2;
                            entries = realloc(entries, entry_cap * sizeof(SemTokEntry));
                        }
                        entries[entry_count++] = (SemTokEntry){cl, 0, ll, LSP_SEMTOK_COMMENT};
                    }
                    scan = le;
                }
            } else {
                /* Single-line block comment */
                int len = col - start_col;
                if (len > 0) {
                    if (entry_count >= entry_cap) {
                        entry_cap *= 2;
                        entries = realloc(entries, entry_cap * sizeof(SemTokEntry));
                    }
                    entries[entry_count++] = (SemTokEntry){start_line, start_col, len, LSP_SEMTOK_COMMENT};
                }
            }
            continue;
        }

        if (*p == '\n') {
            line++;
            col = 0;
        } else col++;
        p++;
    }

    /* Phase 2: Lex tokens and classify */
    Lexer lex = lexer_new(doc->text);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);
    if (lex_err) {
        free(lex_err);
        /* Still process what we can */
    }

    for (size_t i = 0; i < tokens.len; i++) {
        Token *tok = lat_vec_get(&tokens, i);
        if (tok->type == TOK_EOF) break;

        int tok_line = (int)tok->line - 1; /* Convert 1-based to 0-based */
        int tok_col = (int)tok->col - 1;
        int tok_len = 0;
        int tok_type = -1;

        if (is_keyword_token(tok->type)) {
            tok_type = LSP_SEMTOK_KEYWORD;
            tok_len = keyword_token_length(tok->type);
        } else if (tok->type == TOK_INT_LIT || tok->type == TOK_FLOAT_LIT) {
            tok_type = LSP_SEMTOK_NUMBER;
            /* Scan source text from position to find length */
            const char *s = doc->text;
            int sl = 0;
            while (sl < (int)tok->line - 1 && *s) {
                if (*s == '\n') sl++;
                s++;
            }
            s += tok->col - 1;
            const char *start = s;
            if (*s == '-') s++;
            if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) {
                s += 2;
                while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F') || *s == '_')
                    s++;
            } else if (*s == '0' && (s[1] == 'b' || s[1] == 'B')) {
                s += 2;
                while (*s == '0' || *s == '1' || *s == '_') s++;
            } else {
                while ((*s >= '0' && *s <= '9') || *s == '_') s++;
                if (*s == '.') {
                    s++;
                    while ((*s >= '0' && *s <= '9') || *s == '_') s++;
                }
                if (*s == 'e' || *s == 'E') {
                    s++;
                    if (*s == '+' || *s == '-') s++;
                    while (*s >= '0' && *s <= '9') s++;
                }
            }
            tok_len = (int)(s - start);
            if (tok_len <= 0) tok_len = 1;
        } else if (tok->type == TOK_STRING_LIT || tok->type == TOK_INTERP_START || tok->type == TOK_INTERP_MID ||
                   tok->type == TOK_INTERP_END) {
            tok_type = LSP_SEMTOK_STRING;
            /* String token length: scan source text for the quoted string */
            const char *s = doc->text;
            int sl = 0;
            while (sl < (int)tok->line - 1 && *s) {
                if (*s == '\n') sl++;
                s++;
            }
            s += tok->col - 1;
            if (*s == '"') {
                const char *start = s;
                s++;
                while (*s && *s != '"' && *s != '\n') {
                    if (*s == '\\') {
                        s++;
                        if (*s) s++;
                        continue;
                    }
                    if (*s == '$' && s[1] == '{') break; /* interpolation start */
                    s++;
                }
                if (*s == '"') s++;
                tok_len = (int)(s - start);
            } else {
                tok_len = tok->as.str_val ? (int)strlen(tok->as.str_val) : 1;
            }
        } else if (tok->type == TOK_IDENT) {
            /* Look up in doc symbols */
            tok_type = LSP_SEMTOK_VARIABLE;
            if (tok->as.str_val) {
                tok_len = (int)strlen(tok->as.str_val);
                for (size_t s = 0; s < doc->symbol_count; s++) {
                    if (strcmp(doc->symbols[s].name, tok->as.str_val) == 0) {
                        if (doc->symbols[s].kind == LSP_SYM_FUNCTION) tok_type = LSP_SEMTOK_FUNCTION;
                        else if (doc->symbols[s].kind == LSP_SYM_STRUCT || doc->symbols[s].kind == LSP_SYM_ENUM)
                            tok_type = LSP_SEMTOK_TYPE;
                        break;
                    }
                }
            }
        } else if (is_operator_token(tok->type)) {
            tok_type = LSP_SEMTOK_OPERATOR;
            tok_len = operator_token_length(tok->type);
        }

        if (tok_type >= 0 && tok_len > 0 && tok_line >= 0 && tok_col >= 0) {
            if (entry_count >= entry_cap) {
                entry_cap *= 2;
                entries = realloc(entries, entry_cap * sizeof(SemTokEntry));
            }
            entries[entry_count++] = (SemTokEntry){tok_line, tok_col, tok_len, tok_type};
        }
    }

    for (size_t i = 0; i < tokens.len; i++) token_free(lat_vec_get(&tokens, i));
    lat_vec_free(&tokens);

    if (entry_count == 0) {
        free(entries);
        return;
    }

    /* Sort by position */
    qsort(entries, entry_count, sizeof(SemTokEntry), semtok_cmp);

    /* Remove duplicates at same position (keep first, which is comment if present) */
    size_t dedup_count = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (i > 0 && entries[i].line == entries[i - 1].line && entries[i].col == entries[i - 1].col) continue;
        entries[dedup_count++] = entries[i];
    }
    entry_count = dedup_count;

    /* Delta-encode */
    doc->semantic_tokens.count = entry_count * 5;
    doc->semantic_tokens.data = malloc(doc->semantic_tokens.count * sizeof(int));
    if (!doc->semantic_tokens.data) {
        doc->semantic_tokens.count = 0;
        free(entries);
        return;
    }

    int prev_line = 0, prev_col = 0;
    for (size_t i = 0; i < entry_count; i++) {
        int dl = entries[i].line - prev_line;
        int dc = (dl == 0) ? entries[i].col - prev_col : entries[i].col;
        doc->semantic_tokens.data[i * 5 + 0] = dl;
        doc->semantic_tokens.data[i * 5 + 1] = dc;
        doc->semantic_tokens.data[i * 5 + 2] = entries[i].length;
        doc->semantic_tokens.data[i * 5 + 3] = entries[i].type;
        doc->semantic_tokens.data[i * 5 + 4] = 0; /* modifiers */
        prev_line = entries[i].line;
        prev_col = entries[i].col;
    }

    free(entries);
}

/* Analyze a document: lex, parse, extract diagnostics and symbols */
void lsp_analyze_document(LspDocument *doc) {
    /* Free previous results */
    for (size_t i = 0; i < doc->diag_count; i++) free(doc->diagnostics[i].message);
    free(doc->diagnostics);
    doc->diagnostics = NULL;
    doc->diag_count = 0;

    for (size_t i = 0; i < doc->symbol_count; i++) {
        free(doc->symbols[i].name);
        free(doc->symbols[i].signature);
        free(doc->symbols[i].doc);
        free(doc->symbols[i].owner_type);
    }
    free(doc->symbols);
    doc->symbols = NULL;
    doc->symbol_count = 0;

    free_struct_defs(doc->struct_defs, doc->struct_def_count);
    doc->struct_defs = NULL;
    doc->struct_def_count = 0;

    free_enum_defs(doc->enum_defs, doc->enum_def_count);
    doc->enum_defs = NULL;
    doc->enum_def_count = 0;

    free_impl_methods(doc->impl_methods, doc->impl_method_count);
    doc->impl_methods = NULL;
    doc->impl_method_count = 0;

    free(doc->folding_ranges);
    doc->folding_ranges = NULL;
    doc->folding_range_count = 0;

    free(doc->semantic_tokens.data);
    doc->semantic_tokens.data = NULL;
    doc->semantic_tokens.count = 0;

    if (!doc->text) return;

    /* Lex */
    Lexer lex = lexer_new(doc->text);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);

    if (lex_err) {
        doc->diag_count = 1;
        doc->diagnostics = malloc(sizeof(LspDiagnostic));
        if (!doc->diagnostics) return;
        doc->diagnostics[0] = parse_error(lex_err);
        free(lex_err);
        free_tokens(&tokens);
        return;
    }

    /* Parse */
    Parser parser = parser_new(&tokens);
    char *parse_err = NULL;
    Program prog = parser_parse(&parser, &parse_err);

    if (parse_err) {
        doc->diag_count = 1;
        doc->diagnostics = malloc(sizeof(LspDiagnostic));
        if (!doc->diagnostics) return;
        doc->diagnostics[0] = parse_error(parse_err);
        free(parse_err);

        /* Extract symbols from partial AST — gives completions for
         * successfully parsed items even when there's a syntax error */
        if (prog.item_count > 0) extract_symbols(doc, &prog);
    } else {
        /* Extract symbols */
        extract_symbols(doc, &prog);

        /* Compile — catch compiler errors as diagnostics */
        char *comp_err = NULL;
        Chunk *chunk = stack_compile(&prog, &comp_err);
        if (comp_err) {
            doc->diag_count = 1;
            doc->diagnostics = malloc(sizeof(LspDiagnostic));
            if (doc->diagnostics) doc->diagnostics[0] = parse_error(comp_err);
            free(comp_err);
        }
        if (chunk) chunk_free(chunk);
    }

    /* Clean up AST and tokens */
    for (size_t i = 0; i < prog.item_count; i++) item_free(&prog.items[i]);
    free(prog.items);
    free_tokens(&tokens);

    /* Extract folding ranges and semantic tokens (text-based, run after symbols) */
    extract_folding_ranges(doc);
    extract_semantic_tokens(doc);
}

void lsp_document_free(LspDocument *doc) {
    if (!doc) return;
    free(doc->uri);
    free(doc->text);
    for (size_t i = 0; i < doc->diag_count; i++) free(doc->diagnostics[i].message);
    free(doc->diagnostics);
    for (size_t i = 0; i < doc->symbol_count; i++) {
        free(doc->symbols[i].name);
        free(doc->symbols[i].signature);
        free(doc->symbols[i].doc);
        free(doc->symbols[i].owner_type);
    }
    free(doc->symbols);
    free_struct_defs(doc->struct_defs, doc->struct_def_count);
    free_enum_defs(doc->enum_defs, doc->enum_def_count);
    free_impl_methods(doc->impl_methods, doc->impl_method_count);
    free(doc->folding_ranges);
    free(doc->semantic_tokens.data);
    free(doc);
}
