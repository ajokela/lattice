#include "lsp.h"
#include "lexer.h"
#include "parser.h"
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
static void find_decl_position(const char *text, const char *keyword,
                               const char *name, int search_from,
                               int *out_line, int *out_col) {
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
            if (*p == '\n') { line++; line_start = p + 1; }
            p++;
        }

        /* Check that keyword is followed by space then the name */
        const char *after_kw = kw + kw_len;
        if (*after_kw == ' ' || *after_kw == '\t') {
            after_kw++;
            while (*after_kw == ' ' || *after_kw == '\t') after_kw++;
            if (strncmp(after_kw, name, name_len) == 0) {
                char ch_after = after_kw[name_len];
                if (ch_after == '(' || ch_after == ' ' || ch_after == '\t' ||
                    ch_after == '{' || ch_after == '\n' || ch_after == '\r' ||
                    ch_after == '\0' || ch_after == ':') {
                    *out_line = line;
                    *out_col = (int)(after_kw - line_start);
                    return;
                }
            }
        }
        p = kw + 1;
    }
}

/* Extract symbols from parsed AST */
static void extract_symbols(LspDocument *doc, const Program *prog) {
    int last_line = 0;  /* Track search position to handle duplicate names */

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
                for (size_t j = 0; j < fn->param_count; j++)
                    siglen += strlen(fn->params[j].name) + 16;
                sym.signature = malloc(siglen);
                char *p = sym.signature;
                p += sprintf(p, "fn %s(", fn->name);
                for (size_t j = 0; j < fn->param_count; j++) {
                    if (j > 0) p += sprintf(p, ", ");
                    p += sprintf(p, "%s", fn->params[j].name);
                    if (fn->params[j].ty.name)
                        p += sprintf(p, ": %s", fn->params[j].ty.name);
                }
                sprintf(p, ")");
                find_decl_position(doc->text, "fn", fn->name, last_line,
                                   &sym.line, &sym.col);
                found = true;
                break;
            }
            case ITEM_STRUCT: {
                sym.name = strdup(item->as.struct_decl.name);
                sym.kind = LSP_SYM_STRUCT;
                sym.signature = malloc(strlen(item->as.struct_decl.name) + 16);
                sprintf(sym.signature, "struct %s", item->as.struct_decl.name);
                find_decl_position(doc->text, "struct", item->as.struct_decl.name,
                                   last_line, &sym.line, &sym.col);
                found = true;
                break;
            }
            case ITEM_ENUM: {
                sym.name = strdup(item->as.enum_decl.name);
                sym.kind = LSP_SYM_ENUM;
                sym.signature = malloc(strlen(item->as.enum_decl.name) + 16);
                sprintf(sym.signature, "enum %s", item->as.enum_decl.name);
                find_decl_position(doc->text, "enum", item->as.enum_decl.name,
                                   last_line, &sym.line, &sym.col);
                found = true;
                break;
            }
            default:
                break;
        }

        if (found) {
            last_line = sym.line;
            doc->symbol_count++;
            doc->symbols = realloc(doc->symbols, doc->symbol_count * sizeof(LspSymbol));
            doc->symbols[doc->symbol_count - 1] = sym;
        }
    }
}

/* Free tokens helper */
static void free_tokens(LatVec *tokens) {
    for (size_t i = 0; i < tokens->len; i++)
        token_free(lat_vec_get(tokens, i));
    lat_vec_free(tokens);
}

/* Analyze a document: lex, parse, extract diagnostics and symbols */
void lsp_analyze_document(LspDocument *doc) {
    /* Free previous results */
    for (size_t i = 0; i < doc->diag_count; i++)
        free(doc->diagnostics[i].message);
    free(doc->diagnostics);
    doc->diagnostics = NULL;
    doc->diag_count = 0;

    for (size_t i = 0; i < doc->symbol_count; i++) {
        free(doc->symbols[i].name);
        free(doc->symbols[i].signature);
        free(doc->symbols[i].doc);
    }
    free(doc->symbols);
    doc->symbols = NULL;
    doc->symbol_count = 0;

    if (!doc->text) return;

    /* Lex */
    Lexer lex = lexer_new(doc->text);
    char *lex_err = NULL;
    LatVec tokens = lexer_tokenize(&lex, &lex_err);

    if (lex_err) {
        doc->diag_count = 1;
        doc->diagnostics = malloc(sizeof(LspDiagnostic));
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
        doc->diagnostics[0] = parse_error(parse_err);
        free(parse_err);
    } else {
        /* Extract symbols */
        extract_symbols(doc, &prog);
    }

    /* Clean up */
    for (size_t i = 0; i < prog.item_count; i++)
        item_free(&prog.items[i]);
    free(prog.items);
    free_tokens(&tokens);
}

void lsp_document_free(LspDocument *doc) {
    if (!doc) return;
    free(doc->uri);
    free(doc->text);
    for (size_t i = 0; i < doc->diag_count; i++)
        free(doc->diagnostics[i].message);
    free(doc->diagnostics);
    for (size_t i = 0; i < doc->symbol_count; i++) {
        free(doc->symbols[i].name);
        free(doc->symbols[i].signature);
        free(doc->symbols[i].doc);
    }
    free(doc->symbols);
    free(doc);
}
