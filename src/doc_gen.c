#include "doc_gen.h"
#include "lattice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>

/* ── Internal helpers ───────────────────────────────────────────────────── */

/* Dynamic string buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->cap = 256;
    sb->data = malloc(sb->cap);
    sb->data[0] = '\0';
    sb->len = 0;
}

static void sb_ensure(StrBuf *sb, size_t extra) {
    while (sb->len + extra + 1 > sb->cap) {
        sb->cap *= 2;
        sb->data = realloc(sb->data, sb->cap);
    }
}

static void sb_append(StrBuf *sb, const char *s) {
    size_t slen = strlen(s);
    sb_ensure(sb, slen);
    memcpy(sb->data + sb->len, s, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
}

static void sb_append_char(StrBuf *sb, char c) {
    sb_ensure(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

__attribute__((format(printf, 2, 3))) static void sb_printf(StrBuf *sb, const char *fmt, ...) {
    char *tmp = NULL;
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&tmp, fmt, ap) < 0) tmp = NULL;
    va_end(ap);
    if (tmp) {
        sb_append(sb, tmp);
        free(tmp);
    }
}

static char *sb_finish(StrBuf *sb) { return sb->data; /* caller owns it */ }

/* ── Source scanner ─────────────────────────────────────────────────────── */

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    int line;
} DocScanner;

static void ds_init(DocScanner *ds, const char *source) {
    ds->src = source;
    ds->len = strlen(source);
    ds->pos = 0;
    ds->line = 1;
}

static bool ds_eof(const DocScanner *ds) { return ds->pos >= ds->len; }

static char ds_peek(const DocScanner *ds) {
    if (ds->pos >= ds->len) return '\0';
    return ds->src[ds->pos];
}

static char ds_advance(DocScanner *ds) {
    if (ds->pos >= ds->len) return '\0';
    char c = ds->src[ds->pos++];
    if (c == '\n') ds->line++;
    return c;
}

/* Skip to end of line, return pointer past newline */
static void ds_skip_line(DocScanner *ds) {
    while (!ds_eof(ds) && ds_peek(ds) != '\n') ds_advance(ds);
    if (!ds_eof(ds)) ds_advance(ds); /* consume \n */
}

/* Skip whitespace (not newlines) */
static void ds_skip_spaces(DocScanner *ds) {
    while (!ds_eof(ds) && (ds_peek(ds) == ' ' || ds_peek(ds) == '\t')) ds_advance(ds);
}

/* Skip all whitespace including newlines */
static void ds_skip_whitespace(DocScanner *ds) {
    while (!ds_eof(ds) && isspace((unsigned char)ds_peek(ds))) ds_advance(ds);
}

/* Check if current position starts with a triple-slash doc comment */
static bool ds_at_doc_comment(const DocScanner *ds) {
    if (ds->pos + 2 >= ds->len) return false;
    return ds->src[ds->pos] == '/' && ds->src[ds->pos + 1] == '/' && ds->src[ds->pos + 2] == '/';
}

/* Check if current position starts with a regular comment (// but not ///) */
static bool ds_at_comment(const DocScanner *ds) {
    if (ds->pos + 1 >= ds->len) return false;
    return ds->src[ds->pos] == '/' && ds->src[ds->pos + 1] == '/' &&
           (ds->pos + 2 >= ds->len || ds->src[ds->pos + 2] != '/');
}

/* Read a doc comment block: consecutive lines starting with /// (after optional whitespace).
 * Returns heap-allocated string with the comment text (/// prefix and leading space stripped).
 * Empty lines within the block produce blank lines in the output. */
static char *ds_read_doc_block(DocScanner *ds) {
    StrBuf sb;
    sb_init(&sb);
    bool first = true;

    while (!ds_eof(ds)) {
        /* Save position to backtrack if not a doc comment line */
        size_t saved_pos = ds->pos;
        int saved_line = ds->line;

        ds_skip_spaces(ds);

        if (!ds_at_doc_comment(ds)) {
            /* Not a doc comment — backtrack */
            ds->pos = saved_pos;
            ds->line = saved_line;
            break;
        }

        /* Skip the /// prefix */
        ds_advance(ds);
        ds_advance(ds);
        ds_advance(ds);

        /* Skip one optional space after /// */
        if (!ds_eof(ds) && ds_peek(ds) == ' ') ds_advance(ds);

        if (!first) sb_append_char(&sb, '\n');
        first = false;

        /* Read the rest of the line */
        while (!ds_eof(ds) && ds_peek(ds) != '\n') sb_append_char(&sb, ds_advance(ds));

        /* Consume the newline */
        if (!ds_eof(ds)) ds_advance(ds);
    }

    if (sb.len == 0) {
        free(sb.data);
        return NULL;
    }
    return sb_finish(&sb);
}

/* Read an identifier at current position */
static char *ds_read_ident(DocScanner *ds) {
    if (ds_eof(ds)) return NULL;
    if (!isalpha((unsigned char)ds_peek(ds)) && ds_peek(ds) != '_') return NULL;

    StrBuf sb;
    sb_init(&sb);
    while (!ds_eof(ds) && (isalnum((unsigned char)ds_peek(ds)) || ds_peek(ds) == '_'))
        sb_append_char(&sb, ds_advance(ds));

    return sb_finish(&sb);
}

/* Read until matching close delimiter, handling nesting.
 * open/close are e.g. '(' / ')' or '{' / '}'.
 * Returns heap-allocated string of contents between delimiters (not including them). */
static char *ds_read_balanced(DocScanner *ds, char open, char close) {
    if (ds_eof(ds) || ds_peek(ds) != open) return NULL;
    ds_advance(ds); /* consume open */

    StrBuf sb;
    sb_init(&sb);
    int depth = 1;

    while (!ds_eof(ds) && depth > 0) {
        char c = ds_peek(ds);
        if (c == open) depth++;
        else if (c == close) {
            depth--;
            if (depth == 0) {
                ds_advance(ds); /* consume closing delimiter */
                break;
            }
        }
        sb_append_char(&sb, ds_advance(ds));
    }

    return sb_finish(&sb);
}

/* Skip a brace-delimited block { ... } including nested braces */
static void ds_skip_braces(DocScanner *ds) {
    if (ds_eof(ds) || ds_peek(ds) != '{') return;
    ds_advance(ds);
    int depth = 1;
    while (!ds_eof(ds) && depth > 0) {
        char c = ds_advance(ds);
        if (c == '{') depth++;
        else if (c == '}') depth--;
    }
}

/* ── Parse individual doc comment lines for field/variant docs ──────── */

/* Read inline doc comments for struct fields or enum variants.
 * Expects to be inside a { } block. Reads doc comments and field declarations.
 * Returns arrays of names, types/params, and docs. */

static void parse_struct_fields(DocScanner *ds, DocField **out_fields, size_t *out_count) {
    size_t cap = 8, n = 0;
    DocField *fields = malloc(cap * sizeof(DocField));

    while (!ds_eof(ds) && ds_peek(ds) != '}') {
        ds_skip_whitespace(ds);
        if (ds_eof(ds) || ds_peek(ds) == '}') break;

        /* Check for doc comment on field */
        char *field_doc = NULL;
        if (ds_at_doc_comment(ds)) {
            field_doc = ds_read_doc_block(ds);
            ds_skip_whitespace(ds);
        }

        /* Skip regular comments */
        if (ds_at_comment(ds)) {
            ds_skip_line(ds);
            free(field_doc);
            continue;
        }

        if (ds_peek(ds) == '}') {
            free(field_doc);
            break;
        }

        /* Read field name */
        char *name = ds_read_ident(ds);
        if (!name) {
            free(field_doc);
            ds_skip_line(ds);
            continue;
        }

        ds_skip_spaces(ds);

        /* Expect colon */
        char *type_name = NULL;
        if (!ds_eof(ds) && ds_peek(ds) == ':') {
            ds_advance(ds);
            ds_skip_spaces(ds);

            /* Read type up to comma or } or newline */
            StrBuf tb;
            sb_init(&tb);
            while (!ds_eof(ds) && ds_peek(ds) != ',' && ds_peek(ds) != '}' && ds_peek(ds) != '\n') {
                sb_append_char(&tb, ds_advance(ds));
            }
            /* Trim trailing whitespace */
            while (tb.len > 0 && isspace((unsigned char)tb.data[tb.len - 1])) tb.data[--tb.len] = '\0';
            type_name = sb_finish(&tb);
        }

        /* Skip comma if present */
        ds_skip_spaces(ds);
        if (!ds_eof(ds) && ds_peek(ds) == ',') ds_advance(ds);

        if (n >= cap) {
            cap *= 2;
            fields = realloc(fields, cap * sizeof(DocField));
        }
        fields[n].name = name;
        fields[n].type_name = type_name;
        fields[n].doc = field_doc;
        n++;
    }

    *out_fields = fields;
    *out_count = n;
}

static void parse_enum_variants(DocScanner *ds, DocVariant **out_variants, size_t *out_count) {
    size_t cap = 8, n = 0;
    DocVariant *variants = malloc(cap * sizeof(DocVariant));

    while (!ds_eof(ds) && ds_peek(ds) != '}') {
        ds_skip_whitespace(ds);
        if (ds_eof(ds) || ds_peek(ds) == '}') break;

        /* Check for doc comment on variant */
        char *var_doc = NULL;
        if (ds_at_doc_comment(ds)) {
            var_doc = ds_read_doc_block(ds);
            ds_skip_whitespace(ds);
        }

        /* Skip regular comments */
        if (ds_at_comment(ds)) {
            ds_skip_line(ds);
            free(var_doc);
            continue;
        }

        if (ds_peek(ds) == '}') {
            free(var_doc);
            break;
        }

        /* Read variant name */
        char *name = ds_read_ident(ds);
        if (!name) {
            free(var_doc);
            ds_skip_line(ds);
            continue;
        }

        ds_skip_spaces(ds);

        /* Check for tuple params: Variant(Type1, Type2) */
        char *params = NULL;
        if (!ds_eof(ds) && ds_peek(ds) == '(') { params = ds_read_balanced(ds, '(', ')'); }

        /* Skip comma if present */
        ds_skip_spaces(ds);
        if (!ds_eof(ds) && ds_peek(ds) == ',') ds_advance(ds);

        if (n >= cap) {
            cap *= 2;
            variants = realloc(variants, cap * sizeof(DocVariant));
        }
        variants[n].name = name;
        variants[n].params = params;
        variants[n].doc = var_doc;
        n++;
    }

    *out_variants = variants;
    *out_count = n;
}

/* Parse function parameters from a parenthesized string */
static void parse_fn_params(DocScanner *ds, DocParam **out_params, size_t *out_count) {
    size_t cap = 8, n = 0;
    DocParam *params = malloc(cap * sizeof(DocParam));

    /* We should be at '(' */
    if (ds_eof(ds) || ds_peek(ds) != '(') {
        *out_params = params;
        *out_count = 0;
        return;
    }
    ds_advance(ds); /* consume '(' */

    while (!ds_eof(ds) && ds_peek(ds) != ')') {
        ds_skip_whitespace(ds);
        if (ds_peek(ds) == ')') break;

        /* Check for variadic ... prefix */
        bool is_variadic = false;
        if (ds->pos + 2 < ds->len && ds->src[ds->pos] == '.' && ds->src[ds->pos + 1] == '.' &&
            ds->src[ds->pos + 2] == '.') {
            is_variadic = true;
            ds_advance(ds);
            ds_advance(ds);
            ds_advance(ds);
        }

        char *name = ds_read_ident(ds);
        if (!name) {
            ds_skip_line(ds);
            break;
        }

        ds_skip_spaces(ds);

        char *type_name = NULL;
        bool has_default = false;

        if (!ds_eof(ds) && ds_peek(ds) == ':') {
            ds_advance(ds);
            ds_skip_spaces(ds);

            StrBuf tb;
            sb_init(&tb);
            /* Read type: stop at , or ) or = */
            int paren_depth = 0;
            while (!ds_eof(ds)) {
                char c = ds_peek(ds);
                if (c == '(') paren_depth++;
                else if (c == ')') {
                    if (paren_depth == 0) break;
                    paren_depth--;
                }
                if (c == ',' && paren_depth == 0) break;
                if (c == '=' && paren_depth == 0) break;
                sb_append_char(&tb, ds_advance(ds));
            }
            while (tb.len > 0 && isspace((unsigned char)tb.data[tb.len - 1])) tb.data[--tb.len] = '\0';
            type_name = sb_finish(&tb);
        }

        /* Check for default value */
        ds_skip_spaces(ds);
        if (!ds_eof(ds) && ds_peek(ds) == '=') {
            has_default = true;
            ds_advance(ds);
            /* Skip the default value expression (up to , or ) at matching depth) */
            int depth = 0;
            while (!ds_eof(ds)) {
                char c = ds_peek(ds);
                if (c == '(') depth++;
                else if (c == ')') {
                    if (depth == 0) break;
                    depth--;
                }
                if (c == ',' && depth == 0) break;
                ds_advance(ds);
            }
        }

        /* Skip comma */
        ds_skip_spaces(ds);
        if (!ds_eof(ds) && ds_peek(ds) == ',') ds_advance(ds);

        if (n >= cap) {
            cap *= 2;
            params = realloc(params, cap * sizeof(DocParam));
        }
        params[n].name = name;
        params[n].type_name = type_name;
        params[n].is_variadic = is_variadic;
        params[n].has_default = has_default;
        n++;
    }

    /* Consume ')' */
    if (!ds_eof(ds) && ds_peek(ds) == ')') ds_advance(ds);

    *out_params = params;
    *out_count = n;
}

/* Read a return type annotation: -> TypeName */
static char *ds_read_return_type(DocScanner *ds) {
    ds_skip_spaces(ds);
    if (ds_eof(ds)) return NULL;
    if (ds->pos + 1 >= ds->len) return NULL;
    if (ds->src[ds->pos] != '-' || ds->src[ds->pos + 1] != '>') return NULL;

    ds_advance(ds);
    ds_advance(ds); /* skip -> */
    ds_skip_spaces(ds);

    StrBuf tb;
    sb_init(&tb);
    /* Read type until we hit a keyword, brace, or newline */
    int bracket_depth = 0;
    while (!ds_eof(ds)) {
        char c = ds_peek(ds);
        if (c == '[') bracket_depth++;
        else if (c == ']') bracket_depth--;
        if (bracket_depth <= 0 && (c == '{' || c == '\n')) break;
        /* Also stop at 'require'/'ensure' contextual keywords */
        if (bracket_depth <= 0 && isalpha((unsigned char)c)) {
            /* Peek ahead to check for require/ensure */
            size_t remaining = ds->len - ds->pos;
            if ((remaining >= 7 && strncmp(ds->src + ds->pos, "require", 7) == 0 &&
                 (ds->pos + 7 >= ds->len || !isalnum((unsigned char)ds->src[ds->pos + 7]))) ||
                (remaining >= 6 && strncmp(ds->src + ds->pos, "ensure", 6) == 0 &&
                 (ds->pos + 6 >= ds->len || !isalnum((unsigned char)ds->src[ds->pos + 6])))) {
                break;
            }
        }
        sb_append_char(&tb, ds_advance(ds));
    }
    while (tb.len > 0 && isspace((unsigned char)tb.data[tb.len - 1])) tb.data[--tb.len] = '\0';

    if (tb.len == 0) {
        free(tb.data);
        return NULL;
    }
    return sb_finish(&tb);
}

/* Parse trait methods (signatures only, no body) */
static void parse_trait_methods(DocScanner *ds, DocTraitMethod **out_methods, size_t *out_count) {
    size_t cap = 8, n = 0;
    DocTraitMethod *methods = malloc(cap * sizeof(DocTraitMethod));

    while (!ds_eof(ds) && ds_peek(ds) != '}') {
        ds_skip_whitespace(ds);
        if (ds_eof(ds) || ds_peek(ds) == '}') break;

        char *method_doc = NULL;
        if (ds_at_doc_comment(ds)) {
            method_doc = ds_read_doc_block(ds);
            ds_skip_whitespace(ds);
        }

        /* Skip regular comments */
        if (ds_at_comment(ds)) {
            ds_skip_line(ds);
            free(method_doc);
            continue;
        }

        if (ds_peek(ds) == '}') {
            free(method_doc);
            break;
        }

        /* Expect 'fn' keyword */
        size_t saved = ds->pos;
        int saved_line = ds->line;
        char *kw = ds_read_ident(ds);
        if (!kw || strcmp(kw, "fn") != 0) {
            free(kw);
            free(method_doc);
            ds->pos = saved;
            ds->line = saved_line;
            ds_skip_line(ds);
            continue;
        }
        free(kw);

        ds_skip_spaces(ds);
        char *name = ds_read_ident(ds);
        if (!name) {
            free(method_doc);
            ds_skip_line(ds);
            continue;
        }

        ds_skip_spaces(ds);

        DocParam *params = NULL;
        size_t param_count = 0;
        if (!ds_eof(ds) && ds_peek(ds) == '(') { parse_fn_params(ds, &params, &param_count); }

        char *ret_type = ds_read_return_type(ds);

        /* Skip optional semicolon */
        ds_skip_spaces(ds);
        if (!ds_eof(ds) && ds_peek(ds) == ';') ds_advance(ds);

        if (n >= cap) {
            cap *= 2;
            methods = realloc(methods, cap * sizeof(DocTraitMethod));
        }
        methods[n].name = name;
        methods[n].params = params;
        methods[n].param_count = param_count;
        methods[n].return_type = ret_type;
        methods[n].doc = method_doc;
        n++;
    }

    *out_methods = methods;
    *out_count = n;
}

/* Parse impl block methods (full fn decls with bodies) */
static void parse_impl_methods(DocScanner *ds, DocTraitMethod **out_methods, size_t *out_count) {
    size_t cap = 8, n = 0;
    DocTraitMethod *methods = malloc(cap * sizeof(DocTraitMethod));

    while (!ds_eof(ds) && ds_peek(ds) != '}') {
        ds_skip_whitespace(ds);
        if (ds_eof(ds) || ds_peek(ds) == '}') break;

        char *method_doc = NULL;
        if (ds_at_doc_comment(ds)) {
            method_doc = ds_read_doc_block(ds);
            ds_skip_whitespace(ds);
        }

        /* Skip regular comments */
        if (ds_at_comment(ds)) {
            ds_skip_line(ds);
            free(method_doc);
            continue;
        }

        if (ds_peek(ds) == '}') {
            free(method_doc);
            break;
        }

        /* Expect 'fn' keyword */
        size_t saved = ds->pos;
        int saved_line = ds->line;
        char *kw = ds_read_ident(ds);
        if (!kw || strcmp(kw, "fn") != 0) {
            free(kw);
            free(method_doc);
            ds->pos = saved;
            ds->line = saved_line;
            ds_skip_line(ds);
            continue;
        }
        free(kw);

        ds_skip_spaces(ds);
        char *name = ds_read_ident(ds);
        if (!name) {
            free(method_doc);
            ds_skip_line(ds);
            continue;
        }

        ds_skip_spaces(ds);

        DocParam *params = NULL;
        size_t param_count = 0;
        if (!ds_eof(ds) && ds_peek(ds) == '(') { parse_fn_params(ds, &params, &param_count); }

        char *ret_type = ds_read_return_type(ds);

        /* Skip the function body (skip any contract keywords, then brace block) */
        ds_skip_whitespace(ds);
        /* Skip require/ensure clauses */
        while (!ds_eof(ds)) {
            size_t saved2 = ds->pos;
            int saved_line2 = ds->line;
            ds_skip_spaces(ds);
            char *maybe_kw = ds_read_ident(ds);
            if (maybe_kw && (strcmp(maybe_kw, "require") == 0 || strcmp(maybe_kw, "ensure") == 0)) {
                free(maybe_kw);
                /* Skip expression — find the next 'require', 'ensure', or '{' */
                int depth = 0;
                while (!ds_eof(ds)) {
                    char c = ds_peek(ds);
                    if (c == '(') depth++;
                    else if (c == ')') depth--;
                    else if (depth == 0 && c == '{') break;
                    /* Check for next require/ensure */
                    if (depth == 0 && isalpha((unsigned char)c)) {
                        size_t rem = ds->len - ds->pos;
                        if ((rem >= 7 && strncmp(ds->src + ds->pos, "require", 7) == 0 &&
                             !isalnum((unsigned char)ds->src[ds->pos + 7])) ||
                            (rem >= 6 && strncmp(ds->src + ds->pos, "ensure", 6) == 0 &&
                             !isalnum((unsigned char)ds->src[ds->pos + 6])))
                            break;
                    }
                    ds_advance(ds);
                }
            } else {
                free(maybe_kw);
                ds->pos = saved2;
                ds->line = saved_line2;
                break;
            }
        }

        ds_skip_whitespace(ds);
        if (!ds_eof(ds) && ds_peek(ds) == '{') ds_skip_braces(ds);

        if (n >= cap) {
            cap *= 2;
            methods = realloc(methods, cap * sizeof(DocTraitMethod));
        }
        methods[n].name = name;
        methods[n].params = params;
        methods[n].param_count = param_count;
        methods[n].return_type = ret_type;
        methods[n].doc = method_doc;
        n++;
    }

    *out_methods = methods;
    *out_count = n;
}

/* ── Main extraction ────────────────────────────────────────────────────── */

DocFile doc_extract(const char *source, const char *filename) {
    DocFile df;
    memset(&df, 0, sizeof(df));
    df.filename = strdup(filename ? filename : "<stdin>");

    DocScanner ds;
    ds_init(&ds, source);

    size_t cap = 16;
    df.items = malloc(cap * sizeof(DocItem));

    /* First, check for module-level doc comment (/// at very start of file) */
    ds_skip_whitespace(&ds);
    if (ds_at_doc_comment(&ds)) {
        /* Peek ahead: is this followed by a declaration keyword? */
        size_t saved_pos = ds.pos;
        int saved_line = ds.line;
        char *doc = ds_read_doc_block(&ds);
        ds_skip_whitespace(&ds);

        /* Check what follows */
        size_t check_pos = ds.pos;
        int check_line = ds.line;
        char *next_word = ds_read_ident(&ds);
        ds.pos = check_pos;
        ds.line = check_line;

        bool is_decl_doc =
            next_word &&
            (strcmp(next_word, "fn") == 0 || strcmp(next_word, "struct") == 0 || strcmp(next_word, "enum") == 0 ||
             strcmp(next_word, "trait") == 0 || strcmp(next_word, "impl") == 0 || strcmp(next_word, "flux") == 0 ||
             strcmp(next_word, "fix") == 0 || strcmp(next_word, "let") == 0 || strcmp(next_word, "export") == 0);

        if (is_decl_doc) {
            /* This doc comment belongs to the first declaration — backtrack */
            ds.pos = saved_pos;
            ds.line = saved_line;
            free(doc);
        } else {
            /* Module-level doc comment */
            df.module_doc = doc;
        }
        free(next_word);
    }

    while (!ds_eof(&ds)) {
        ds_skip_whitespace(&ds);
        if (ds_eof(&ds)) break;

        /* Collect any doc comment */
        char *doc = NULL;
        if (ds_at_doc_comment(&ds)) {
            doc = ds_read_doc_block(&ds);
            ds_skip_whitespace(&ds);
        }

        /* Skip regular comments */
        if (ds_at_comment(&ds)) {
            ds_skip_line(&ds);
            free(doc);
            continue;
        }

        /* Skip block comments */
        if (!ds_eof(&ds) && ds_peek(&ds) == '/' && ds.pos + 1 < ds.len && ds.src[ds.pos + 1] == '*') {
            ds_advance(&ds);
            ds_advance(&ds);
            int depth = 1;
            while (!ds_eof(&ds) && depth > 0) {
                char c = ds_advance(&ds);
                if (c == '/' && ds_peek(&ds) == '*') {
                    ds_advance(&ds);
                    depth++;
                } else if (c == '*' && ds_peek(&ds) == '/') {
                    ds_advance(&ds);
                    depth--;
                }
            }
            free(doc);
            continue;
        }

        if (ds_eof(&ds)) {
            free(doc);
            break;
        }

        /* Try to read an identifier / keyword */
        int decl_line = ds.line;

        /* Check for 'export' keyword prefix */
        char *word = ds_read_ident(&ds);
        if (word && strcmp(word, "export") == 0) {
            free(word);
            ds_skip_whitespace(&ds);
            word = ds_read_ident(&ds);
            decl_line = ds.line;
        }

        if (!word) {
            /* Not an identifier — skip character */
            free(doc);
            ds_advance(&ds);
            continue;
        }

        /* Ensure capacity */
        if (df.item_count >= cap) {
            cap *= 2;
            df.items = realloc(df.items, cap * sizeof(DocItem));
        }

        if (strcmp(word, "fn") == 0) {
            /* Function declaration */
            free(word);
            ds_skip_spaces(&ds);
            char *name = ds_read_ident(&ds);
            if (!name) {
                free(doc);
                ds_skip_line(&ds);
                continue;
            }

            ds_skip_spaces(&ds);

            DocParam *params = NULL;
            size_t param_count = 0;
            if (!ds_eof(&ds) && ds_peek(&ds) == '(') parse_fn_params(&ds, &params, &param_count);

            char *ret_type = ds_read_return_type(&ds);

            /* Skip require/ensure and body */
            ds_skip_whitespace(&ds);
            while (!ds_eof(&ds)) {
                size_t s2 = ds.pos;
                int sl2 = ds.line;
                ds_skip_spaces(&ds);
                char *kw = ds_read_ident(&ds);
                if (kw && (strcmp(kw, "require") == 0 || strcmp(kw, "ensure") == 0)) {
                    free(kw);
                    int depth = 0;
                    while (!ds_eof(&ds)) {
                        char c = ds_peek(&ds);
                        if (c == '(') depth++;
                        else if (c == ')') depth--;
                        else if (depth == 0 && c == '{') break;
                        if (depth == 0 && isalpha((unsigned char)c)) {
                            size_t rem = ds.len - ds.pos;
                            if ((rem >= 7 && strncmp(ds.src + ds.pos, "require", 7) == 0 &&
                                 (ds.pos + 7 >= ds.len || !isalnum((unsigned char)ds.src[ds.pos + 7]))) ||
                                (rem >= 6 && strncmp(ds.src + ds.pos, "ensure", 6) == 0 &&
                                 (ds.pos + 6 >= ds.len || !isalnum((unsigned char)ds.src[ds.pos + 6]))))
                                break;
                        }
                        ds_advance(&ds);
                    }
                } else {
                    free(kw);
                    ds.pos = s2;
                    ds.line = sl2;
                    break;
                }
            }
            ds_skip_whitespace(&ds);
            if (!ds_eof(&ds) && ds_peek(&ds) == '{') ds_skip_braces(&ds);

            DocItem *item = &df.items[df.item_count++];
            memset(item, 0, sizeof(*item));
            item->kind = DOC_FUNCTION;
            item->name = name;
            item->doc = doc;
            item->line = decl_line;
            item->as.fn.params = params;
            item->as.fn.param_count = param_count;
            item->as.fn.return_type = ret_type;

        } else if (strcmp(word, "struct") == 0) {
            /* Struct declaration */
            free(word);
            ds_skip_spaces(&ds);
            char *name = ds_read_ident(&ds);
            if (!name) {
                free(doc);
                ds_skip_line(&ds);
                continue;
            }

            ds_skip_whitespace(&ds);
            DocField *fields = NULL;
            size_t field_count = 0;
            if (!ds_eof(&ds) && ds_peek(&ds) == '{') {
                ds_advance(&ds); /* consume '{' */
                parse_struct_fields(&ds, &fields, &field_count);
                if (!ds_eof(&ds) && ds_peek(&ds) == '}') ds_advance(&ds);
            }

            DocItem *item = &df.items[df.item_count++];
            memset(item, 0, sizeof(*item));
            item->kind = DOC_STRUCT;
            item->name = name;
            item->doc = doc;
            item->line = decl_line;
            item->as.strct.fields = fields;
            item->as.strct.field_count = field_count;

        } else if (strcmp(word, "enum") == 0) {
            /* Enum declaration */
            free(word);
            ds_skip_spaces(&ds);
            char *name = ds_read_ident(&ds);
            if (!name) {
                free(doc);
                ds_skip_line(&ds);
                continue;
            }

            ds_skip_whitespace(&ds);
            DocVariant *variants = NULL;
            size_t variant_count = 0;
            if (!ds_eof(&ds) && ds_peek(&ds) == '{') {
                ds_advance(&ds); /* consume '{' */
                parse_enum_variants(&ds, &variants, &variant_count);
                if (!ds_eof(&ds) && ds_peek(&ds) == '}') ds_advance(&ds);
            }

            DocItem *item = &df.items[df.item_count++];
            memset(item, 0, sizeof(*item));
            item->kind = DOC_ENUM;
            item->name = name;
            item->doc = doc;
            item->line = decl_line;
            item->as.enm.variants = variants;
            item->as.enm.variant_count = variant_count;

        } else if (strcmp(word, "trait") == 0) {
            /* Trait declaration */
            free(word);
            ds_skip_spaces(&ds);
            char *name = ds_read_ident(&ds);
            if (!name) {
                free(doc);
                ds_skip_line(&ds);
                continue;
            }

            ds_skip_whitespace(&ds);
            DocTraitMethod *methods = NULL;
            size_t method_count = 0;
            if (!ds_eof(&ds) && ds_peek(&ds) == '{') {
                ds_advance(&ds); /* consume '{' */
                parse_trait_methods(&ds, &methods, &method_count);
                if (!ds_eof(&ds) && ds_peek(&ds) == '}') ds_advance(&ds);
            }

            DocItem *item = &df.items[df.item_count++];
            memset(item, 0, sizeof(*item));
            item->kind = DOC_TRAIT;
            item->name = name;
            item->doc = doc;
            item->line = decl_line;
            item->as.trait.methods = methods;
            item->as.trait.method_count = method_count;

        } else if (strcmp(word, "impl") == 0) {
            /* Impl block */
            free(word);
            ds_skip_spaces(&ds);
            char *trait_name = ds_read_ident(&ds);
            if (!trait_name) {
                free(doc);
                ds_skip_line(&ds);
                continue;
            }

            ds_skip_spaces(&ds);
            /* Expect 'for' keyword */
            char *for_kw = ds_read_ident(&ds);
            if (!for_kw || strcmp(for_kw, "for") != 0) {
                free(for_kw);
                free(trait_name);
                free(doc);
                ds_skip_line(&ds);
                continue;
            }
            free(for_kw);

            ds_skip_spaces(&ds);
            char *type_name = ds_read_ident(&ds);
            if (!type_name) {
                free(trait_name);
                free(doc);
                ds_skip_line(&ds);
                continue;
            }

            ds_skip_whitespace(&ds);
            DocTraitMethod *methods = NULL;
            size_t method_count = 0;
            if (!ds_eof(&ds) && ds_peek(&ds) == '{') {
                ds_advance(&ds); /* consume '{' */
                parse_impl_methods(&ds, &methods, &method_count);
                if (!ds_eof(&ds) && ds_peek(&ds) == '}') ds_advance(&ds);
            }

            DocItem *item = &df.items[df.item_count++];
            memset(item, 0, sizeof(*item));
            item->kind = DOC_IMPL;
            item->name = NULL;
            item->doc = doc;
            item->line = decl_line;
            item->as.impl.trait_name = trait_name;
            item->as.impl.type_name = type_name;
            item->as.impl.methods = methods;
            item->as.impl.method_count = method_count;

        } else if (strcmp(word, "flux") == 0 || strcmp(word, "fix") == 0 || strcmp(word, "let") == 0) {
            /* Variable declaration */
            char *phase = word;
            ds_skip_spaces(&ds);

            /* Check for destructuring: let [a, b] = ... or let {x, y} = ... */
            if (!ds_eof(&ds) && (ds_peek(&ds) == '[' || ds_peek(&ds) == '{')) {
                free(doc);
                free(phase);
                ds_skip_line(&ds);
                continue;
            }

            char *name = ds_read_ident(&ds);
            if (!name) {
                free(doc);
                free(phase);
                ds_skip_line(&ds);
                continue;
            }

            ds_skip_spaces(&ds);

            /* Optional type annotation */
            char *type_name = NULL;
            if (!ds_eof(&ds) && ds_peek(&ds) == ':') {
                ds_advance(&ds);
                ds_skip_spaces(&ds);
                StrBuf tb;
                sb_init(&tb);
                while (!ds_eof(&ds) && ds_peek(&ds) != '=' && ds_peek(&ds) != '\n') {
                    sb_append_char(&tb, ds_advance(&ds));
                }
                while (tb.len > 0 && isspace((unsigned char)tb.data[tb.len - 1])) tb.data[--tb.len] = '\0';
                if (tb.len > 0) type_name = sb_finish(&tb);
                else free(tb.data);
            }

            /* Skip the rest of the line (= value) */
            ds_skip_line(&ds);

            /* Only document variables with doc comments */
            if (doc) {
                DocItem *item = &df.items[df.item_count++];
                memset(item, 0, sizeof(*item));
                item->kind = DOC_VARIABLE;
                item->name = name;
                item->doc = doc;
                item->line = decl_line;
                item->as.var.phase = phase;
                item->as.var.type_name = type_name;
            } else {
                free(name);
                free(phase);
                free(type_name);
            }

        } else if (strcmp(word, "test") == 0) {
            /* Skip test blocks */
            free(word);
            free(doc);
            ds_skip_whitespace(&ds);
            /* Skip test name string */
            if (!ds_eof(&ds) && ds_peek(&ds) == '"') {
                ds_advance(&ds);
                while (!ds_eof(&ds) && ds_peek(&ds) != '"') {
                    if (ds_peek(&ds) == '\\') ds_advance(&ds);
                    ds_advance(&ds);
                }
                if (!ds_eof(&ds)) ds_advance(&ds);
            }
            ds_skip_whitespace(&ds);
            if (!ds_eof(&ds) && ds_peek(&ds) == '{') ds_skip_braces(&ds);

        } else if (strcmp(word, "import") == 0) {
            /* Skip imports */
            free(word);
            free(doc);
            ds_skip_line(&ds);

        } else {
            /* Unknown — skip to next line or statement */
            free(word);
            free(doc);
            /* Skip to next newline, handling braces we might encounter */
            while (!ds_eof(&ds) && ds_peek(&ds) != '\n') {
                if (ds_peek(&ds) == '{') {
                    ds_skip_braces(&ds);
                    break;
                }
                ds_advance(&ds);
            }
            if (!ds_eof(&ds) && ds_peek(&ds) == '\n') ds_advance(&ds);
        }
    }

    return df;
}

/* ── Free ───────────────────────────────────────────────────────────────── */

static void doc_param_free(DocParam *p) {
    free(p->name);
    free(p->type_name);
}

static void doc_trait_method_free(DocTraitMethod *m) {
    free(m->name);
    for (size_t i = 0; i < m->param_count; i++) doc_param_free(&m->params[i]);
    free(m->params);
    free(m->return_type);
    free(m->doc);
}

void doc_file_free(DocFile *df) {
    free(df->filename);
    free(df->module_doc);
    for (size_t i = 0; i < df->item_count; i++) {
        DocItem *it = &df->items[i];
        free(it->name);
        free(it->doc);
        switch (it->kind) {
            case DOC_FUNCTION:
                for (size_t j = 0; j < it->as.fn.param_count; j++) doc_param_free(&it->as.fn.params[j]);
                free(it->as.fn.params);
                free(it->as.fn.return_type);
                break;
            case DOC_STRUCT:
                for (size_t j = 0; j < it->as.strct.field_count; j++) {
                    free(it->as.strct.fields[j].name);
                    free(it->as.strct.fields[j].type_name);
                    free(it->as.strct.fields[j].doc);
                }
                free(it->as.strct.fields);
                break;
            case DOC_ENUM:
                for (size_t j = 0; j < it->as.enm.variant_count; j++) {
                    free(it->as.enm.variants[j].name);
                    free(it->as.enm.variants[j].params);
                    free(it->as.enm.variants[j].doc);
                }
                free(it->as.enm.variants);
                break;
            case DOC_TRAIT:
                for (size_t j = 0; j < it->as.trait.method_count; j++) doc_trait_method_free(&it->as.trait.methods[j]);
                free(it->as.trait.methods);
                break;
            case DOC_IMPL:
                free(it->as.impl.trait_name);
                free(it->as.impl.type_name);
                for (size_t j = 0; j < it->as.impl.method_count; j++) doc_trait_method_free(&it->as.impl.methods[j]);
                free(it->as.impl.methods);
                break;
            case DOC_VARIABLE:
                free(it->as.var.phase);
                free(it->as.var.type_name);
                break;
            case DOC_MODULE: break;
        }
    }
    free(df->items);
}

/* ── Rendering helpers ──────────────────────────────────────────────────── */

static void render_params_md(StrBuf *sb, const DocParam *params, size_t count) {
    sb_append(sb, "(");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) sb_append(sb, ", ");
        if (params[i].is_variadic) sb_append(sb, "...");
        sb_append(sb, params[i].name);
        if (params[i].type_name) {
            sb_append(sb, ": ");
            sb_append(sb, params[i].type_name);
        }
        if (params[i].has_default) sb_append(sb, " = ...");
    }
    sb_append(sb, ")");
}

static void render_params_json(StrBuf *sb, const DocParam *params, size_t count) {
    sb_append(sb, "[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) sb_append(sb, ", ");
        sb_append(sb, "{");
        sb_printf(sb, "\"name\": \"%s\"", params[i].name);
        if (params[i].type_name) sb_printf(sb, ", \"type\": \"%s\"", params[i].type_name);
        if (params[i].is_variadic) sb_append(sb, ", \"variadic\": true");
        if (params[i].has_default) sb_append(sb, ", \"has_default\": true");
        sb_append(sb, "}");
    }
    sb_append(sb, "]");
}

/* Escape a string for JSON output */
static void sb_append_json_str(StrBuf *sb, const char *s) {
    sb_append(sb, "\"");
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"': sb_append(sb, "\\\""); break;
            case '\\': sb_append(sb, "\\\\"); break;
            case '\n': sb_append(sb, "\\n"); break;
            case '\r': sb_append(sb, "\\r"); break;
            case '\t': sb_append(sb, "\\t"); break;
            default: sb_append_char(sb, *p); break;
        }
    }
    sb_append(sb, "\"");
}

/* Escape a string for HTML output */
static void sb_append_html_esc(StrBuf *sb, const char *s) {
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '&': sb_append(sb, "&amp;"); break;
            case '<': sb_append(sb, "&lt;"); break;
            case '>': sb_append(sb, "&gt;"); break;
            case '"': sb_append(sb, "&quot;"); break;
            default: sb_append_char(sb, *p); break;
        }
    }
}

/* ── Markdown renderer ──────────────────────────────────────────────────── */

static void render_markdown(StrBuf *sb, const DocFile *files, size_t file_count) {
    for (size_t fi = 0; fi < file_count; fi++) {
        const DocFile *df = &files[fi];

        if (file_count > 1) { sb_printf(sb, "# %s\n\n", df->filename); }

        if (df->module_doc) {
            sb_append(sb, df->module_doc);
            sb_append(sb, "\n\n");
        }

        bool has_functions = false, has_structs = false, has_enums = false;
        bool has_traits = false, has_impls = false, has_vars = false;

        for (size_t i = 0; i < df->item_count; i++) {
            switch (df->items[i].kind) {
                case DOC_FUNCTION: has_functions = true; break;
                case DOC_STRUCT: has_structs = true; break;
                case DOC_ENUM: has_enums = true; break;
                case DOC_TRAIT: has_traits = true; break;
                case DOC_IMPL: has_impls = true; break;
                case DOC_VARIABLE: has_vars = true; break;
                case DOC_MODULE: break;
            }
        }

        /* Functions */
        if (has_functions) {
            sb_append(sb, "## Functions\n\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_FUNCTION) continue;

                sb_printf(sb, "### `%s", it->name);
                render_params_md(sb, it->as.fn.params, it->as.fn.param_count);
                if (it->as.fn.return_type) sb_printf(sb, " -> %s", it->as.fn.return_type);
                sb_append(sb, "`\n\n");

                if (it->doc) {
                    sb_append(sb, it->doc);
                    sb_append(sb, "\n\n");
                }
            }
        }

        /* Structs */
        if (has_structs) {
            sb_append(sb, "## Structs\n\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_STRUCT) continue;

                sb_printf(sb, "### `struct %s`\n\n", it->name);
                if (it->doc) {
                    sb_append(sb, it->doc);
                    sb_append(sb, "\n\n");
                }

                if (it->as.strct.field_count > 0) {
                    sb_append(sb, "| Field | Type | Description |\n");
                    sb_append(sb, "|-------|------|-------------|\n");
                    for (size_t j = 0; j < it->as.strct.field_count; j++) {
                        const DocField *f = &it->as.strct.fields[j];
                        sb_printf(sb, "| `%s` | `%s` | %s |\n", f->name, f->type_name ? f->type_name : "",
                                  f->doc ? f->doc : "");
                    }
                    sb_append(sb, "\n");
                }
            }
        }

        /* Enums */
        if (has_enums) {
            sb_append(sb, "## Enums\n\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_ENUM) continue;

                sb_printf(sb, "### `enum %s`\n\n", it->name);
                if (it->doc) {
                    sb_append(sb, it->doc);
                    sb_append(sb, "\n\n");
                }

                if (it->as.enm.variant_count > 0) {
                    sb_append(sb, "**Variants:**\n\n");
                    for (size_t j = 0; j < it->as.enm.variant_count; j++) {
                        const DocVariant *v = &it->as.enm.variants[j];
                        sb_printf(sb, "- `%s", v->name);
                        if (v->params) sb_printf(sb, "(%s)", v->params);
                        sb_append(sb, "`");
                        if (v->doc) sb_printf(sb, " — %s", v->doc);
                        sb_append(sb, "\n");
                    }
                    sb_append(sb, "\n");
                }
            }
        }

        /* Traits */
        if (has_traits) {
            sb_append(sb, "## Traits\n\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_TRAIT) continue;

                sb_printf(sb, "### `trait %s`\n\n", it->name);
                if (it->doc) {
                    sb_append(sb, it->doc);
                    sb_append(sb, "\n\n");
                }

                if (it->as.trait.method_count > 0) {
                    sb_append(sb, "**Methods:**\n\n");
                    for (size_t j = 0; j < it->as.trait.method_count; j++) {
                        const DocTraitMethod *m = &it->as.trait.methods[j];
                        sb_printf(sb, "- `fn %s", m->name);
                        render_params_md(sb, m->params, m->param_count);
                        if (m->return_type) sb_printf(sb, " -> %s", m->return_type);
                        sb_append(sb, "`");
                        if (m->doc) sb_printf(sb, " — %s", m->doc);
                        sb_append(sb, "\n");
                    }
                    sb_append(sb, "\n");
                }
            }
        }

        /* Impl blocks */
        if (has_impls) {
            sb_append(sb, "## Implementations\n\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_IMPL) continue;

                sb_printf(sb, "### `impl %s for %s`\n\n", it->as.impl.trait_name, it->as.impl.type_name);
                if (it->doc) {
                    sb_append(sb, it->doc);
                    sb_append(sb, "\n\n");
                }

                if (it->as.impl.method_count > 0) {
                    sb_append(sb, "**Methods:**\n\n");
                    for (size_t j = 0; j < it->as.impl.method_count; j++) {
                        const DocTraitMethod *m = &it->as.impl.methods[j];
                        sb_printf(sb, "- `fn %s", m->name);
                        render_params_md(sb, m->params, m->param_count);
                        if (m->return_type) sb_printf(sb, " -> %s", m->return_type);
                        sb_append(sb, "`");
                        if (m->doc) sb_printf(sb, " — %s", m->doc);
                        sb_append(sb, "\n");
                    }
                    sb_append(sb, "\n");
                }
            }
        }

        /* Variables */
        if (has_vars) {
            sb_append(sb, "## Variables\n\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_VARIABLE) continue;

                sb_printf(sb, "### `%s %s", it->as.var.phase, it->name);
                if (it->as.var.type_name) sb_printf(sb, ": %s", it->as.var.type_name);
                sb_append(sb, "`\n\n");
                if (it->doc) {
                    sb_append(sb, it->doc);
                    sb_append(sb, "\n\n");
                }
            }
        }

        if (fi + 1 < file_count) sb_append(sb, "---\n\n");
    }
}

/* ── JSON renderer ──────────────────────────────────────────────────────── */

static void render_json(StrBuf *sb, const DocFile *files, size_t file_count) {
    sb_append(sb, "[\n");
    for (size_t fi = 0; fi < file_count; fi++) {
        const DocFile *df = &files[fi];
        if (fi > 0) sb_append(sb, ",\n");

        sb_append(sb, "  {\n");
        sb_append(sb, "    \"file\": ");
        sb_append_json_str(sb, df->filename);
        sb_append(sb, ",\n");

        if (df->module_doc) {
            sb_append(sb, "    \"module_doc\": ");
            sb_append_json_str(sb, df->module_doc);
            sb_append(sb, ",\n");
        }

        sb_append(sb, "    \"items\": [\n");
        for (size_t i = 0; i < df->item_count; i++) {
            const DocItem *it = &df->items[i];
            if (i > 0) sb_append(sb, ",\n");

            sb_append(sb, "      {\n");

            const char *kind_str = "unknown";
            switch (it->kind) {
                case DOC_FUNCTION: kind_str = "function"; break;
                case DOC_STRUCT: kind_str = "struct"; break;
                case DOC_ENUM: kind_str = "enum"; break;
                case DOC_TRAIT: kind_str = "trait"; break;
                case DOC_IMPL: kind_str = "impl"; break;
                case DOC_VARIABLE: kind_str = "variable"; break;
                case DOC_MODULE: kind_str = "module"; break;
            }
            sb_printf(sb, "        \"kind\": \"%s\"", kind_str);

            if (it->name) {
                sb_append(sb, ",\n        \"name\": ");
                sb_append_json_str(sb, it->name);
            }

            sb_printf(sb, ",\n        \"line\": %d", it->line);

            if (it->doc) {
                sb_append(sb, ",\n        \"doc\": ");
                sb_append_json_str(sb, it->doc);
            }

            switch (it->kind) {
                case DOC_FUNCTION:
                    sb_append(sb, ",\n        \"params\": ");
                    render_params_json(sb, it->as.fn.params, it->as.fn.param_count);
                    if (it->as.fn.return_type) {
                        sb_append(sb, ",\n        \"return_type\": ");
                        sb_append_json_str(sb, it->as.fn.return_type);
                    }
                    break;

                case DOC_STRUCT:
                    sb_append(sb, ",\n        \"fields\": [");
                    for (size_t j = 0; j < it->as.strct.field_count; j++) {
                        if (j > 0) sb_append(sb, ", ");
                        const DocField *f = &it->as.strct.fields[j];
                        sb_append(sb, "{");
                        sb_printf(sb, "\"name\": \"%s\"", f->name);
                        if (f->type_name) sb_printf(sb, ", \"type\": \"%s\"", f->type_name);
                        if (f->doc) {
                            sb_append(sb, ", \"doc\": ");
                            sb_append_json_str(sb, f->doc);
                        }
                        sb_append(sb, "}");
                    }
                    sb_append(sb, "]");
                    break;

                case DOC_ENUM:
                    sb_append(sb, ",\n        \"variants\": [");
                    for (size_t j = 0; j < it->as.enm.variant_count; j++) {
                        if (j > 0) sb_append(sb, ", ");
                        const DocVariant *v = &it->as.enm.variants[j];
                        sb_append(sb, "{");
                        sb_printf(sb, "\"name\": \"%s\"", v->name);
                        if (v->params) sb_printf(sb, ", \"params\": \"%s\"", v->params);
                        if (v->doc) {
                            sb_append(sb, ", \"doc\": ");
                            sb_append_json_str(sb, v->doc);
                        }
                        sb_append(sb, "}");
                    }
                    sb_append(sb, "]");
                    break;

                case DOC_TRAIT:
                    sb_append(sb, ",\n        \"methods\": [");
                    for (size_t j = 0; j < it->as.trait.method_count; j++) {
                        if (j > 0) sb_append(sb, ", ");
                        const DocTraitMethod *m = &it->as.trait.methods[j];
                        sb_append(sb, "{");
                        sb_printf(sb, "\"name\": \"%s\"", m->name);
                        sb_append(sb, ", \"params\": ");
                        render_params_json(sb, m->params, m->param_count);
                        if (m->return_type) sb_printf(sb, ", \"return_type\": \"%s\"", m->return_type);
                        if (m->doc) {
                            sb_append(sb, ", \"doc\": ");
                            sb_append_json_str(sb, m->doc);
                        }
                        sb_append(sb, "}");
                    }
                    sb_append(sb, "]");
                    break;

                case DOC_IMPL:
                    sb_printf(sb, ",\n        \"trait_name\": \"%s\"", it->as.impl.trait_name);
                    sb_printf(sb, ",\n        \"type_name\": \"%s\"", it->as.impl.type_name);
                    sb_append(sb, ",\n        \"methods\": [");
                    for (size_t j = 0; j < it->as.impl.method_count; j++) {
                        if (j > 0) sb_append(sb, ", ");
                        const DocTraitMethod *m = &it->as.impl.methods[j];
                        sb_append(sb, "{");
                        sb_printf(sb, "\"name\": \"%s\"", m->name);
                        sb_append(sb, ", \"params\": ");
                        render_params_json(sb, m->params, m->param_count);
                        if (m->return_type) sb_printf(sb, ", \"return_type\": \"%s\"", m->return_type);
                        if (m->doc) {
                            sb_append(sb, ", \"doc\": ");
                            sb_append_json_str(sb, m->doc);
                        }
                        sb_append(sb, "}");
                    }
                    sb_append(sb, "]");
                    break;

                case DOC_VARIABLE:
                    sb_printf(sb, ",\n        \"phase\": \"%s\"", it->as.var.phase);
                    if (it->as.var.type_name) sb_printf(sb, ",\n        \"type\": \"%s\"", it->as.var.type_name);
                    break;

                case DOC_MODULE: break;
            }

            sb_append(sb, "\n      }");
        }
        sb_append(sb, "\n    ]\n  }");
    }
    sb_append(sb, "\n]\n");
}

/* ── HTML renderer ──────────────────────────────────────────────────────── */

static void render_html(StrBuf *sb, const DocFile *files, size_t file_count) {
    sb_append(sb, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    sb_append(sb, "  <meta charset=\"utf-8\">\n");
    sb_append(sb, "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    sb_append(sb, "  <title>Lattice Documentation</title>\n");
    sb_append(sb, "  <style>\n");
    sb_append(sb, "    :root {\n");
    sb_append(sb, "      --bg: #08080d; --bg-card: #0e0e18; --border: #1a1a2e;\n");
    sb_append(sb, "      --text: #c8c8d4; --text-dim: #6a6a80; --heading: #e8e8f0;\n");
    sb_append(sb, "      --accent: #4fc3f7; --keyword: #c792ea; --string: #c3e88d;\n");
    sb_append(sb, "      --type: #ffcb6b; --fn: #82aaff;\n");
    sb_append(sb, "      --mono: 'SF Mono', 'Cascadia Code', 'JetBrains Mono', monospace;\n");
    sb_append(sb, "      --sans: 'Inter', -apple-system, system-ui, sans-serif;\n");
    sb_append(sb, "    }\n");
    sb_append(sb, "    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }\n");
    sb_append(sb, "    body { font-family: var(--sans); background: var(--bg); color: var(--text); line-height: 1.7; "
                  "padding: 40px; max-width: 900px; margin: 0 auto; }\n");
    sb_append(sb, "    h1 { color: var(--heading); font-size: 2rem; margin-bottom: 8px; }\n");
    sb_append(sb, "    h2 { color: var(--accent); font-size: 1.4rem; margin: 32px 0 16px; border-bottom: 1px solid "
                  "var(--border); padding-bottom: 8px; }\n");
    sb_append(sb, "    h3 { color: var(--heading); font-size: 1rem; margin: 20px 0 8px; }\n");
    sb_append(sb, "    .doc-card { background: var(--bg-card); border: 1px solid var(--border); border-radius: 8px; "
                  "padding: 16px 20px; margin-bottom: 12px; }\n");
    sb_append(sb, "    .doc-card:hover { border-color: var(--accent); }\n");
    sb_append(sb, "    .sig { font-family: var(--mono); font-size: 0.9rem; margin-bottom: 6px; }\n");
    sb_append(sb, "    .sig .kw { color: var(--keyword); } .sig .fn { color: var(--fn); font-weight: 600; }\n");
    sb_append(sb, "    .sig .ty { color: var(--type); } .sig .dim { color: var(--text-dim); }\n");
    sb_append(
        sb,
        "    .doc-desc { color: var(--text-dim); font-size: 0.875rem; line-height: 1.6; white-space: pre-line; }\n");
    sb_append(sb, "    .fields { margin-top: 10px; } .fields table { width: 100%; border-collapse: collapse; "
                  "font-size: 0.85rem; }\n");
    sb_append(sb, "    .fields th { text-align: left; color: var(--text-dim); font-weight: 500; padding: 4px 12px 4px "
                  "0; border-bottom: 1px solid var(--border); }\n");
    sb_append(sb, "    .fields td { padding: 4px 12px 4px 0; border-bottom: 1px solid rgba(26,26,46,0.5); }\n");
    sb_append(sb, "    .fields code { font-family: var(--mono); font-size: 0.82rem; }\n");
    sb_append(
        sb,
        "    .module-doc { color: var(--text); font-size: 0.95rem; margin-bottom: 24px; white-space: pre-line; }\n");
    sb_append(sb, "    .variant-list { list-style: none; margin: 8px 0; }\n");
    sb_append(sb, "    .variant-list li { padding: 2px 0; font-family: var(--mono); font-size: 0.85rem; }\n");
    sb_append(sb,
              "    .variant-list .vdoc { font-family: var(--sans); color: var(--text-dim); font-size: 0.82rem; }\n");
    sb_append(sb, "    hr { border: none; border-top: 1px solid var(--border); margin: 32px 0; }\n");
    sb_append(sb, "  </style>\n</head>\n<body>\n");

    for (size_t fi = 0; fi < file_count; fi++) {
        const DocFile *df = &files[fi];

        sb_append(sb, "<h1>");
        sb_append_html_esc(sb, df->filename);
        sb_append(sb, "</h1>\n");

        if (df->module_doc) {
            sb_append(sb, "<div class=\"module-doc\">");
            sb_append_html_esc(sb, df->module_doc);
            sb_append(sb, "</div>\n");
        }

        /* Functions */
        bool has_fn = false;
        for (size_t i = 0; i < df->item_count; i++)
            if (df->items[i].kind == DOC_FUNCTION) {
                has_fn = true;
                break;
            }
        if (has_fn) {
            sb_append(sb, "<h2>Functions</h2>\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_FUNCTION) continue;

                sb_append(sb, "<div class=\"doc-card\">\n<div class=\"sig\">");
                sb_append(sb, "<span class=\"kw\">fn</span> <span class=\"fn\">");
                sb_append_html_esc(sb, it->name);
                sb_append(sb, "</span><span class=\"dim\">(</span>");
                for (size_t j = 0; j < it->as.fn.param_count; j++) {
                    if (j > 0) sb_append(sb, "<span class=\"dim\">, </span>");
                    const DocParam *p = &it->as.fn.params[j];
                    if (p->is_variadic) sb_append(sb, "<span class=\"dim\">...</span>");
                    sb_append_html_esc(sb, p->name);
                    if (p->type_name) {
                        sb_append(sb, "<span class=\"dim\">: </span><span class=\"ty\">");
                        sb_append_html_esc(sb, p->type_name);
                        sb_append(sb, "</span>");
                    }
                }
                sb_append(sb, "<span class=\"dim\">)</span>");
                if (it->as.fn.return_type) {
                    sb_append(sb, " <span class=\"dim\">-&gt;</span> <span class=\"ty\">");
                    sb_append_html_esc(sb, it->as.fn.return_type);
                    sb_append(sb, "</span>");
                }
                sb_append(sb, "</div>\n");
                if (it->doc) {
                    sb_append(sb, "<div class=\"doc-desc\">");
                    sb_append_html_esc(sb, it->doc);
                    sb_append(sb, "</div>\n");
                }
                sb_append(sb, "</div>\n");
            }
        }

        /* Structs */
        bool has_st = false;
        for (size_t i = 0; i < df->item_count; i++)
            if (df->items[i].kind == DOC_STRUCT) {
                has_st = true;
                break;
            }
        if (has_st) {
            sb_append(sb, "<h2>Structs</h2>\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_STRUCT) continue;

                sb_append(sb, "<div class=\"doc-card\">\n<div class=\"sig\">");
                sb_append(sb, "<span class=\"kw\">struct</span> <span class=\"ty\">");
                sb_append_html_esc(sb, it->name);
                sb_append(sb, "</span></div>\n");
                if (it->doc) {
                    sb_append(sb, "<div class=\"doc-desc\">");
                    sb_append_html_esc(sb, it->doc);
                    sb_append(sb, "</div>\n");
                }
                if (it->as.strct.field_count > 0) {
                    sb_append(
                        sb,
                        "<div class=\"fields\"><table>\n<tr><th>Field</th><th>Type</th><th>Description</th></tr>\n");
                    for (size_t j = 0; j < it->as.strct.field_count; j++) {
                        const DocField *f = &it->as.strct.fields[j];
                        sb_append(sb, "<tr><td><code>");
                        sb_append_html_esc(sb, f->name);
                        sb_append(sb, "</code></td><td><code>");
                        if (f->type_name) sb_append_html_esc(sb, f->type_name);
                        sb_append(sb, "</code></td><td>");
                        if (f->doc) sb_append_html_esc(sb, f->doc);
                        sb_append(sb, "</td></tr>\n");
                    }
                    sb_append(sb, "</table></div>\n");
                }
                sb_append(sb, "</div>\n");
            }
        }

        /* Enums */
        bool has_en = false;
        for (size_t i = 0; i < df->item_count; i++)
            if (df->items[i].kind == DOC_ENUM) {
                has_en = true;
                break;
            }
        if (has_en) {
            sb_append(sb, "<h2>Enums</h2>\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_ENUM) continue;

                sb_append(sb, "<div class=\"doc-card\">\n<div class=\"sig\">");
                sb_append(sb, "<span class=\"kw\">enum</span> <span class=\"ty\">");
                sb_append_html_esc(sb, it->name);
                sb_append(sb, "</span></div>\n");
                if (it->doc) {
                    sb_append(sb, "<div class=\"doc-desc\">");
                    sb_append_html_esc(sb, it->doc);
                    sb_append(sb, "</div>\n");
                }
                if (it->as.enm.variant_count > 0) {
                    sb_append(sb, "<ul class=\"variant-list\">\n");
                    for (size_t j = 0; j < it->as.enm.variant_count; j++) {
                        const DocVariant *v = &it->as.enm.variants[j];
                        sb_append(sb, "<li>");
                        sb_append_html_esc(sb, v->name);
                        if (v->params) {
                            sb_append(sb, "(");
                            sb_append_html_esc(sb, v->params);
                            sb_append(sb, ")");
                        }
                        if (v->doc) {
                            sb_append(sb, " <span class=\"vdoc\">");
                            sb_append_html_esc(sb, v->doc);
                            sb_append(sb, "</span>");
                        }
                        sb_append(sb, "</li>\n");
                    }
                    sb_append(sb, "</ul>\n");
                }
                sb_append(sb, "</div>\n");
            }
        }

        /* Traits */
        bool has_tr = false;
        for (size_t i = 0; i < df->item_count; i++)
            if (df->items[i].kind == DOC_TRAIT) {
                has_tr = true;
                break;
            }
        if (has_tr) {
            sb_append(sb, "<h2>Traits</h2>\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_TRAIT) continue;

                sb_append(sb, "<div class=\"doc-card\">\n<div class=\"sig\">");
                sb_append(sb, "<span class=\"kw\">trait</span> <span class=\"ty\">");
                sb_append_html_esc(sb, it->name);
                sb_append(sb, "</span></div>\n");
                if (it->doc) {
                    sb_append(sb, "<div class=\"doc-desc\">");
                    sb_append_html_esc(sb, it->doc);
                    sb_append(sb, "</div>\n");
                }
                for (size_t j = 0; j < it->as.trait.method_count; j++) {
                    const DocTraitMethod *m = &it->as.trait.methods[j];
                    sb_append(sb, "<div class=\"sig\" style=\"margin-left:16px; margin-top:8px;\">");
                    sb_append(sb, "<span class=\"kw\">fn</span> <span class=\"fn\">");
                    sb_append_html_esc(sb, m->name);
                    sb_append(sb, "</span><span class=\"dim\">(</span>");
                    for (size_t k = 0; k < m->param_count; k++) {
                        if (k > 0) sb_append(sb, "<span class=\"dim\">, </span>");
                        sb_append_html_esc(sb, m->params[k].name);
                        if (m->params[k].type_name) {
                            sb_append(sb, "<span class=\"dim\">: </span><span class=\"ty\">");
                            sb_append_html_esc(sb, m->params[k].type_name);
                            sb_append(sb, "</span>");
                        }
                    }
                    sb_append(sb, "<span class=\"dim\">)</span>");
                    if (m->return_type) {
                        sb_append(sb, " <span class=\"dim\">-&gt;</span> <span class=\"ty\">");
                        sb_append_html_esc(sb, m->return_type);
                        sb_append(sb, "</span>");
                    }
                    sb_append(sb, "</div>\n");
                    if (m->doc) {
                        sb_append(sb, "<div class=\"doc-desc\" style=\"margin-left:16px;\">");
                        sb_append_html_esc(sb, m->doc);
                        sb_append(sb, "</div>\n");
                    }
                }
                sb_append(sb, "</div>\n");
            }
        }

        /* Impl blocks */
        bool has_im = false;
        for (size_t i = 0; i < df->item_count; i++)
            if (df->items[i].kind == DOC_IMPL) {
                has_im = true;
                break;
            }
        if (has_im) {
            sb_append(sb, "<h2>Implementations</h2>\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_IMPL) continue;

                sb_append(sb, "<div class=\"doc-card\">\n<div class=\"sig\">");
                sb_append(sb, "<span class=\"kw\">impl</span> <span class=\"ty\">");
                sb_append_html_esc(sb, it->as.impl.trait_name);
                sb_append(sb, "</span> <span class=\"kw\">for</span> <span class=\"ty\">");
                sb_append_html_esc(sb, it->as.impl.type_name);
                sb_append(sb, "</span></div>\n");
                if (it->doc) {
                    sb_append(sb, "<div class=\"doc-desc\">");
                    sb_append_html_esc(sb, it->doc);
                    sb_append(sb, "</div>\n");
                }
                for (size_t j = 0; j < it->as.impl.method_count; j++) {
                    const DocTraitMethod *m = &it->as.impl.methods[j];
                    sb_append(sb, "<div class=\"sig\" style=\"margin-left:16px; margin-top:8px;\">");
                    sb_append(sb, "<span class=\"kw\">fn</span> <span class=\"fn\">");
                    sb_append_html_esc(sb, m->name);
                    sb_append(sb, "</span><span class=\"dim\">(</span>");
                    for (size_t k = 0; k < m->param_count; k++) {
                        if (k > 0) sb_append(sb, "<span class=\"dim\">, </span>");
                        sb_append_html_esc(sb, m->params[k].name);
                        if (m->params[k].type_name) {
                            sb_append(sb, "<span class=\"dim\">: </span><span class=\"ty\">");
                            sb_append_html_esc(sb, m->params[k].type_name);
                            sb_append(sb, "</span>");
                        }
                    }
                    sb_append(sb, "<span class=\"dim\">)</span>");
                    if (m->return_type) {
                        sb_append(sb, " <span class=\"dim\">-&gt;</span> <span class=\"ty\">");
                        sb_append_html_esc(sb, m->return_type);
                        sb_append(sb, "</span>");
                    }
                    sb_append(sb, "</div>\n");
                    if (m->doc) {
                        sb_append(sb, "<div class=\"doc-desc\" style=\"margin-left:16px;\">");
                        sb_append_html_esc(sb, m->doc);
                        sb_append(sb, "</div>\n");
                    }
                }
                sb_append(sb, "</div>\n");
            }
        }

        /* Variables */
        bool has_va = false;
        for (size_t i = 0; i < df->item_count; i++)
            if (df->items[i].kind == DOC_VARIABLE) {
                has_va = true;
                break;
            }
        if (has_va) {
            sb_append(sb, "<h2>Variables</h2>\n");
            for (size_t i = 0; i < df->item_count; i++) {
                const DocItem *it = &df->items[i];
                if (it->kind != DOC_VARIABLE) continue;

                sb_append(sb, "<div class=\"doc-card\">\n<div class=\"sig\">");
                sb_append(sb, "<span class=\"kw\">");
                sb_append_html_esc(sb, it->as.var.phase);
                sb_append(sb, "</span> ");
                sb_append_html_esc(sb, it->name);
                if (it->as.var.type_name) {
                    sb_append(sb, "<span class=\"dim\">: </span><span class=\"ty\">");
                    sb_append_html_esc(sb, it->as.var.type_name);
                    sb_append(sb, "</span>");
                }
                sb_append(sb, "</div>\n");
                if (it->doc) {
                    sb_append(sb, "<div class=\"doc-desc\">");
                    sb_append_html_esc(sb, it->doc);
                    sb_append(sb, "</div>\n");
                }
                sb_append(sb, "</div>\n");
            }
        }

        if (fi + 1 < file_count) sb_append(sb, "<hr>\n");
    }

    sb_append(sb, "</body>\n</html>\n");
}

/* ── Public render ──────────────────────────────────────────────────────── */

char *doc_render(const DocFile *files, size_t file_count, DocFormat fmt) {
    StrBuf sb;
    sb_init(&sb);

    switch (fmt) {
        case DOC_FMT_MARKDOWN: render_markdown(&sb, files, file_count); break;
        case DOC_FMT_JSON: render_json(&sb, files, file_count); break;
        case DOC_FMT_HTML: render_html(&sb, files, file_count); break;
    }

    return sb_finish(&sb);
}

/* ── File I/O helpers ───────────────────────────────────────────────────── */

static char *read_file_content(const char *path) {
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

static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool has_lat_suffix(const char *name) {
    size_t len = strlen(name);
    return len >= 4 && strcmp(name + len - 4, ".lat") == 0;
}

static void mkdirs(const char *path) {
    char *tmp = strdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    free(tmp);
}

/* ── CLI entry point ────────────────────────────────────────────────────── */

int doc_cmd(int argc, char **argv) {
    DocFormat fmt = DOC_FMT_MARKDOWN;
    const char *output_dir = NULL;
    const char **inputs = NULL;
    size_t input_count = 0;
    size_t input_cap = 8;

    inputs = malloc(input_cap * sizeof(char *));

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            fmt = DOC_FMT_JSON;
        } else if (strcmp(argv[i], "--html") == 0) {
            fmt = DOC_FMT_HTML;
        } else if (strcmp(argv[i], "--markdown") == 0 || strcmp(argv[i], "--md") == 0) {
            fmt = DOC_FMT_MARKDOWN;
        } else if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) output_dir = argv[++i];
            else {
                fprintf(stderr, "error: %s requires an argument\n", argv[i]);
                free(inputs);
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: clat doc [options] <file.lat|dir>...\n\n");
            printf("Generate documentation from Lattice source files.\n\n");
            printf("Options:\n");
            printf("  --md, --markdown   Markdown output (default)\n");
            printf("  --json             JSON output\n");
            printf("  --html             HTML output\n");
            printf("  -o, --output DIR   Write output files to DIR\n");
            printf("  -h, --help         Show this help\n\n");
            printf("Examples:\n");
            printf("  clat doc file.lat            Markdown to stdout\n");
            printf("  clat doc --json file.lat     JSON to stdout\n");
            printf("  clat doc --html dir/         HTML to stdout\n");
            printf("  clat doc -o docs/ src/       Write docs to docs/\n");
            free(inputs);
            return 0;
        } else {
            if (input_count >= input_cap) {
                input_cap *= 2;
                inputs = realloc(inputs, input_cap * sizeof(char *));
            }
            inputs[input_count++] = argv[i];
        }
    }

    if (input_count == 0) {
        fprintf(stderr, "error: no input files specified\n");
        fprintf(stderr, "usage: clat doc [--json|--html] [--output dir] <file.lat|dir>...\n");
        free(inputs);
        return 1;
    }

    /* Collect all .lat files */
    size_t file_cap = 16, file_count = 0;
    char **file_paths = malloc(file_cap * sizeof(char *));

    for (size_t i = 0; i < input_count; i++) {
        if (is_directory(inputs[i])) {
            DIR *d = opendir(inputs[i]);
            if (!d) {
                fprintf(stderr, "error: cannot open directory '%s'\n", inputs[i]);
                continue;
            }
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (!has_lat_suffix(ent->d_name)) continue;
                if (file_count >= file_cap) {
                    file_cap *= 2;
                    file_paths = realloc(file_paths, file_cap * sizeof(char *));
                }
                char *full_path = NULL;
                lat_asprintf(&full_path, "%s/%s", inputs[i], ent->d_name);
                file_paths[file_count++] = full_path;
            }
            closedir(d);
        } else {
            if (file_count >= file_cap) {
                file_cap *= 2;
                file_paths = realloc(file_paths, file_cap * sizeof(char *));
            }
            file_paths[file_count++] = strdup(inputs[i]);
        }
    }

    if (file_count == 0) {
        fprintf(stderr, "error: no .lat files found\n");
        free(inputs);
        free(file_paths);
        return 1;
    }

    /* Extract docs from each file */
    DocFile *doc_files = malloc(file_count * sizeof(DocFile));
    size_t doc_count = 0;

    for (size_t i = 0; i < file_count; i++) {
        char *source = read_file_content(file_paths[i]);
        if (!source) {
            fprintf(stderr, "warning: cannot read '%s'\n", file_paths[i]);
            continue;
        }

        /* Use basename for display */
        char *path_copy = strdup(file_paths[i]);
        char *base = basename(path_copy);
        doc_files[doc_count] = doc_extract(source, base);
        free(path_copy);
        free(source);
        doc_count++;
    }

    if (output_dir) {
        /* Write one file per input */
        mkdirs(output_dir);
        mkdir(output_dir, 0755);

        const char *ext = ".md";
        if (fmt == DOC_FMT_JSON) ext = ".json";
        else if (fmt == DOC_FMT_HTML) ext = ".html";

        for (size_t i = 0; i < doc_count; i++) {
            char *rendered = doc_render(&doc_files[i], 1, fmt);

            /* Build output filename: output_dir / basename.ext */
            char *path_copy = strdup(doc_files[i].filename);
            char *base = basename(path_copy);
            /* Strip .lat extension if present */
            size_t blen = strlen(base);
            if (blen >= 4 && strcmp(base + blen - 4, ".lat") == 0) base[blen - 4] = '\0';

            char *out_path = NULL;
            lat_asprintf(&out_path, "%s/%s%s", output_dir, base, ext);
            free(path_copy);

            FILE *f = fopen(out_path, "w");
            if (f) {
                fputs(rendered, f);
                fclose(f);
                fprintf(stderr, "wrote %s\n", out_path);
            } else {
                fprintf(stderr, "error: cannot write '%s'\n", out_path);
            }
            free(out_path);
            free(rendered);
        }
    } else {
        /* Write all to stdout */
        char *rendered = doc_render(doc_files, doc_count, fmt);
        fputs(rendered, stdout);
        free(rendered);
    }

    /* Cleanup */
    for (size_t i = 0; i < doc_count; i++) doc_file_free(&doc_files[i]);
    free(doc_files);

    for (size_t i = 0; i < file_count; i++) free(file_paths[i]);
    free(file_paths);
    free(inputs);

    return 0;
}
