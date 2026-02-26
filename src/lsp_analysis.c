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

    /* Clean up */
    for (size_t i = 0; i < prog.item_count; i++) item_free(&prog.items[i]);
    free(prog.items);
    free_tokens(&tokens);
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
    free(doc);
}
