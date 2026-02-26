/*
 * formatter.c — Source code formatter for Lattice (.lat) files.
 *
 * Strategy: scan the source character by character, recognizing language
 * constructs (comments, strings, keywords, operators, braces) and re-emit
 * them with normalized whitespace and indentation.
 *
 * This avoids the standard lexer (which strips comments) so that comments
 * are preserved in the formatted output.
 */

#include "formatter.h"
#include "lattice.h"
#include "ds/str.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#define INDENT_WIDTH   4
#define MAX_LINE_WIDTH 100

/* ── Scanner state ── */
typedef struct {
    const char *src;
    size_t len;
    size_t pos;

    LatStr out; /* output buffer */
    int indent; /* current indentation level */
    bool at_line_start;
    bool need_space; /* emit a space before next token */
    int line_len;    /* chars on current output line */
    bool last_was_newline;
    int blank_lines;    /* consecutive blank lines emitted */
    bool in_struct_lit; /* track if we're inside a struct literal */
    int paren_depth;
    int bracket_depth;
    int brace_depth; /* tracks overall brace depth */

    /* Track what was last emitted for spacing decisions */
    enum {
        LAST_NONE,
        LAST_IDENT,
        LAST_NUMBER,
        LAST_STRING,
        LAST_KEYWORD,
        LAST_OPERATOR,
        LAST_OPEN_PAREN,
        LAST_CLOSE_PAREN,
        LAST_OPEN_BRACE,
        LAST_CLOSE_BRACE,
        LAST_OPEN_BRACKET,
        LAST_CLOSE_BRACKET,
        LAST_COMMA,
        LAST_SEMICOLON,
        LAST_COLON,
        LAST_DOT,
        LAST_ARROW,
        LAST_FAT_ARROW,
        LAST_PIPE,
        LAST_COMMENT,
        LAST_MODE_DIRECTIVE,
        LAST_AT,
        LAST_BANG,
        LAST_QUESTION,
        LAST_TILDE,
        LAST_STAR,
        LAST_COLONCOLON,
        LAST_DOTDOT,
        LAST_DOTDOTDOT,
    } last;

    /* The actual string of the last keyword/ident for context */
    char last_word[64];

    /* Tracking top-level items for blank-line separation */
    bool had_toplevel_item;

    /* Track if we're inside closure parameter delimiters |...| */
    bool in_closure_params;
} Fmt;

/* ── Helpers ── */

static char fmt_peek(const Fmt *f) {
    if (f->pos >= f->len) return '\0';
    return f->src[f->pos];
}

static char fmt_peek_at(const Fmt *f, size_t offset) {
    size_t idx = f->pos + offset;
    if (idx >= f->len) return '\0';
    return f->src[idx];
}

static char fmt_advance(Fmt *f) {
    if (f->pos >= f->len) return '\0';
    return f->src[f->pos++];
}

static void emit_char(Fmt *f, char c) {
    lat_str_push(&f->out, c);
    if (c == '\n') {
        f->line_len = 0;
        f->at_line_start = true;
        f->last_was_newline = true;
    } else {
        f->line_len++;
        f->at_line_start = false;
        f->last_was_newline = false;
    }
}

static void emit_indent(Fmt *f) {
    for (int i = 0; i < f->indent * INDENT_WIDTH; i++) emit_char(f, ' ');
}

static void emit_newline(Fmt *f) {
    /* Don't emit trailing whitespace on current line */
    /* Trim trailing spaces from the output buffer */
    while (f->out.len > 0 && f->out.data[f->out.len - 1] == ' ') {
        f->out.len--;
        f->out.data[f->out.len] = '\0';
    }
    emit_char(f, '\n');
    f->blank_lines = 0;
}

static void emit_blank_line(Fmt *f) {
    /* Trim trailing spaces */
    while (f->out.len > 0 && f->out.data[f->out.len - 1] == ' ') {
        f->out.len--;
        f->out.data[f->out.len] = '\0';
    }
    /* Only emit blank line if we haven't already emitted one */
    if (f->blank_lines < 1) {
        emit_char(f, '\n');
        f->blank_lines++;
    }
}

static void ensure_on_new_line(Fmt *f) {
    if (!f->at_line_start && f->out.len > 0) emit_newline(f);
}

/* ── Source scanning helpers ── */

static bool is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool is_keyword(const char *word) {
    static const char *keywords[] = {"flux",      "fix",    "let",      "freeze", "thaw",  "forge",       "fn",
                                     "struct",    "if",     "else",     "for",    "in",    "while",       "loop",
                                     "return",    "break",  "continue", "spawn",  "true",  "false",       "nil",
                                     "clone",     "anneal", "print",    "try",    "catch", "scope",       "test",
                                     "match",     "enum",   "import",   "from",   "as",    "crystallize", "borrow",
                                     "sublimate", "defer",  "select",   "trait",  "impl",  "export",      NULL};
    for (const char **kw = keywords; *kw; kw++) {
        if (strcmp(word, *kw) == 0) return true;
    }
    return false;
}

/* Keywords that are followed by '(' and should have a space before it */
static bool is_flow_keyword(const char *word) {
    return strcmp(word, "if") == 0 || strcmp(word, "while") == 0 || strcmp(word, "for") == 0 ||
           strcmp(word, "match") == 0 || strcmp(word, "select") == 0 || strcmp(word, "catch") == 0;
}

/* Read an identifier or keyword from source (does not advance f->pos) */
static size_t read_word(const Fmt *f, char *buf, size_t buf_size) {
    size_t i = 0;
    size_t p = f->pos;
    while (p < f->len && is_ident_char(f->src[p]) && i < buf_size - 1) { buf[i++] = f->src[p++]; }
    buf[i] = '\0';
    return i;
}

/* ── Comment handling ── */

static void emit_line_comment(Fmt *f) {
    /* // already peeked — emit it */
    emit_char(f, fmt_advance(f)); /* / */
    emit_char(f, fmt_advance(f)); /* / */
    /* Ensure a space after // if there isn't one and next char isn't newline */
    if (f->pos < f->len && f->src[f->pos] != ' ' && f->src[f->pos] != '\n' && f->src[f->pos] != '\r') {
        emit_char(f, ' ');
    }
    while (f->pos < f->len && f->src[f->pos] != '\n') { emit_char(f, fmt_advance(f)); }
    f->last = LAST_COMMENT;
}

static void emit_block_comment(Fmt *f) {
    /* Emit block comment preserving contents */
    emit_char(f, fmt_advance(f)); /* / */
    emit_char(f, fmt_advance(f)); /* * */
    int depth = 1;
    while (depth > 0 && f->pos < f->len) {
        char c = f->src[f->pos];
        if (c == '/' && fmt_peek_at(f, 1) == '*') {
            emit_char(f, fmt_advance(f));
            emit_char(f, fmt_advance(f));
            depth++;
        } else if (c == '*' && fmt_peek_at(f, 1) == '/') {
            emit_char(f, fmt_advance(f));
            emit_char(f, fmt_advance(f));
            depth--;
        } else {
            emit_char(f, fmt_advance(f));
        }
    }
    f->last = LAST_COMMENT;
}

/* ── String handling (preserves original content) ── */

static void emit_string_literal(Fmt *f) {
    char quote = fmt_advance(f);
    emit_char(f, quote);

    /* Check for triple-quote */
    if (quote == '"' && fmt_peek(f) == '"' && fmt_peek_at(f, 1) == '"') {
        emit_char(f, fmt_advance(f)); /* second " */
        emit_char(f, fmt_advance(f)); /* third " */
        /* Emit until closing """ */
        while (f->pos < f->len) {
            if (f->src[f->pos] == '"' && fmt_peek_at(f, 1) == '"' && fmt_peek_at(f, 2) == '"') {
                emit_char(f, fmt_advance(f));
                emit_char(f, fmt_advance(f));
                emit_char(f, fmt_advance(f));
                f->last = LAST_STRING;
                return;
            }
            if (f->src[f->pos] == '\\') {
                emit_char(f, fmt_advance(f));                      /* backslash */
                if (f->pos < f->len) emit_char(f, fmt_advance(f)); /* escaped char */
            } else {
                emit_char(f, fmt_advance(f));
            }
        }
        f->last = LAST_STRING;
        return;
    }

    /* Regular string */
    char close_quote = quote;
    while (f->pos < f->len) {
        char c = f->src[f->pos];
        if (c == '\\') {
            emit_char(f, fmt_advance(f));                      /* backslash */
            if (f->pos < f->len) emit_char(f, fmt_advance(f)); /* escaped char */
        } else if (c == close_quote) {
            emit_char(f, fmt_advance(f));
            f->last = LAST_STRING;
            return;
        } else if (c == '$' && close_quote == '"' && fmt_peek_at(f, 1) == '{') {
            /* String interpolation: ${ ... } — preserve literally */
            emit_char(f, fmt_advance(f)); /* $ */
            emit_char(f, fmt_advance(f)); /* { */
            int depth = 1;
            while (depth > 0 && f->pos < f->len) {
                c = f->src[f->pos];
                if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) {
                        emit_char(f, fmt_advance(f));
                        break;
                    }
                }
                if (c == '"' || c == '\'') {
                    char q = fmt_advance(f);
                    emit_char(f, q);
                    while (f->pos < f->len && f->src[f->pos] != q) {
                        if (f->src[f->pos] == '\\') {
                            emit_char(f, fmt_advance(f));
                            if (f->pos < f->len) emit_char(f, fmt_advance(f));
                        } else emit_char(f, fmt_advance(f));
                    }
                    if (f->pos < f->len) emit_char(f, fmt_advance(f)); /* closing quote */
                } else if (depth > 0) {
                    emit_char(f, fmt_advance(f));
                }
            }
        } else {
            emit_char(f, fmt_advance(f));
        }
    }
    f->last = LAST_STRING;
}

/* ── Number literal ── */

static void emit_number(Fmt *f) {
    /* Preserve the number as-is from source */
    while (f->pos < f->len && (isdigit((unsigned char)f->src[f->pos]) || f->src[f->pos] == '.' ||
                               f->src[f->pos] == 'x' || f->src[f->pos] == 'X' || f->src[f->pos] == '_' ||
                               (isxdigit((unsigned char)f->src[f->pos]) && f->pos >= 2 &&
                                (f->src[f->pos - 1] == 'x' || f->src[f->pos - 1] == 'X' ||
                                 isxdigit((unsigned char)f->src[f->pos - 1]))))) {
        emit_char(f, fmt_advance(f));
    }
    f->last = LAST_NUMBER;
}

/* ── Operator handling ── */

/* Determines if a '-' is unary (prefix) rather than binary */
static bool is_unary_minus(const Fmt *f) {
    (void)f;
    switch (f->last) {
        case LAST_IDENT:
        case LAST_NUMBER:
        case LAST_STRING:
        case LAST_CLOSE_PAREN:
        case LAST_CLOSE_BRACKET:
        case LAST_CLOSE_BRACE: return false;
        default: return true;
    }
}

/* Emit a space if needed before a token */
static void space_before(Fmt *f) {
    if (f->at_line_start) {
        emit_indent(f);
    } else if (f->need_space) {
        emit_char(f, ' ');
        f->need_space = false;
    }
}

/* ── Main formatting loop ── */

/* Skip whitespace and newlines in source, counting newlines */
static int skip_ws_counting_newlines(Fmt *f) {
    int newlines = 0;
    while (f->pos < f->len) {
        char c = f->src[f->pos];
        if (c == '\n') {
            newlines++;
            f->pos++;
        } else if (c == '\r') {
            f->pos++;
        } else if (c == ' ' || c == '\t') {
            f->pos++;
        } else break;
    }
    return newlines;
}

char *lat_format(const char *source, char **err) {
    if (!source) {
        if (err) *err = strdup("null source");
        return NULL;
    }

    Fmt f;
    memset(&f, 0, sizeof(f));
    f.src = source;
    f.len = strlen(source);
    f.pos = 0;
    f.out = lat_str_new();
    f.indent = 0;
    f.at_line_start = true;
    f.last_was_newline = true;
    f.blank_lines = 0;
    f.last = LAST_NONE;
    f.last_word[0] = '\0';
    f.had_toplevel_item = false;
    f.paren_depth = 0;
    f.bracket_depth = 0;
    f.brace_depth = 0;
    f.in_struct_lit = false;
    f.need_space = false;

    while (f.pos < f.len) {
        /* Skip source whitespace and count newlines for blank line detection */
        int newlines = skip_ws_counting_newlines(&f);

        if (f.pos >= f.len) break;

        char c = f.src[f.pos];

        /* Handle newlines from source */
        if (newlines > 0 && f.out.len > 0) {
            if (newlines >= 2) {
                /* Blank line(s) in source: emit one blank line */
                ensure_on_new_line(&f);
                emit_blank_line(&f);
            } else if (newlines == 1 && f.brace_depth == 0 && f.last == LAST_COMMENT &&
                       c != '/' /* next is not another comment */) {
                /* At top level: blank line between comment block and declarations */
                ensure_on_new_line(&f);
                emit_blank_line(&f);
            } else {
                /* Single newline: statement separator */
                /* Only emit a newline if we're not already on a new line
                 * and we're not right after an opening brace (which already
                 * emitted a newline) */
                if (!f.at_line_start && f.last != LAST_OPEN_BRACE) { emit_newline(&f); }
            }
        }

        /* ── Mode directive: #mode ── */
        if (c == '#') {
            ensure_on_new_line(&f);
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f)); /* # */
            /* Emit the rest of the directive until newline */
            while (f.pos < f.len && f.src[f.pos] != '\n') { emit_char(&f, fmt_advance(&f)); }
            f.last = LAST_MODE_DIRECTIVE;
            emit_newline(&f);
            continue;
        }

        /* ── Line comment ── */
        if (c == '/' && fmt_peek_at(&f, 1) == '/') {
            if (f.at_line_start) {
                emit_indent(&f);
            } else if (f.last != LAST_NONE) {
                /* Space before inline comment */
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            }
            emit_line_comment(&f);
            /* consume the newline after the comment */
            if (f.pos < f.len && f.src[f.pos] == '\n') f.pos++;
            emit_newline(&f);
            continue;
        }

        /* ── Block comment ── */
        if (c == '/' && fmt_peek_at(&f, 1) == '*') {
            if (f.at_line_start) {
                emit_indent(&f);
            } else {
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            }
            emit_block_comment(&f);
            f.need_space = true;
            continue;
        }

        /* ── String literals ── */
        if (c == '"' || c == '\'') {
            space_before(&f);
            /* Need space after ident/number/closing delimiters */
            if (f.last == LAST_IDENT || f.last == LAST_NUMBER || f.last == LAST_CLOSE_PAREN ||
                f.last == LAST_CLOSE_BRACKET || f.last == LAST_CLOSE_BRACE || f.last == LAST_STRING) {
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ' && !f.at_line_start) emit_char(&f, ' ');
            }
            emit_string_literal(&f);
            f.need_space = false;
            continue;
        }

        /* ── Number literals ── */
        if (isdigit((unsigned char)c)) {
            space_before(&f);
            emit_number(&f);
            f.need_space = true;
            continue;
        }

        /* ── Identifiers and keywords ── */
        if (isalpha((unsigned char)c) || c == '_') {
            char word[128];
            size_t wlen = read_word(&f, word, sizeof(word));
            (void)wlen;

            bool kw = is_keyword(word);

            /* Determine spacing before word */
            if (f.at_line_start) {
                emit_indent(&f);
            } else {
                /* Need space after most things before a word */
                bool needs_space = true;
                if (f.last == LAST_OPEN_PAREN || f.last == LAST_OPEN_BRACKET) needs_space = false;
                if (f.last == LAST_DOT || f.last == LAST_COLONCOLON) needs_space = false;
                if (f.last == LAST_AT) needs_space = false;
                if (f.last == LAST_BANG) needs_space = false;
                if (f.last == LAST_TILDE) needs_space = false;
                if (f.last == LAST_PIPE) needs_space = false;
                if (f.last == LAST_STAR && f.paren_depth > 0) needs_space = false; /* dereference context */
                if (f.last == LAST_NONE) needs_space = false;
                if (needs_space && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            }

            /* Advance past the word */
            for (size_t i = 0; i < strlen(word); i++) emit_char(&f, fmt_advance(&f));

            strncpy(f.last_word, word, sizeof(f.last_word) - 1);
            f.last_word[sizeof(f.last_word) - 1] = '\0';
            f.last = kw ? LAST_KEYWORD : LAST_IDENT;
            f.need_space = true;
            continue;
        }

        /* ── Opening brace { ── */
        if (c == '{') {
            /* Attached brace style: space before { */
            if (!f.at_line_start) {
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            } else {
                emit_indent(&f);
            }
            emit_char(&f, fmt_advance(&f));
            f.indent++;
            f.brace_depth++;
            f.last = LAST_OPEN_BRACE;
            f.need_space = false;

            /* Newline after { */
            emit_newline(&f);
            continue;
        }

        /* ── Closing brace } ── */
        if (c == '}') {
            f.indent--;
            if (f.indent < 0) f.indent = 0;
            f.brace_depth--;
            if (f.brace_depth < 0) f.brace_depth = 0;
            ensure_on_new_line(&f);
            emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_CLOSE_BRACE;
            f.need_space = false;

            /* Check what follows the } */
            size_t saved_pos = f.pos;
            int nl = skip_ws_counting_newlines(&f);
            f.pos = saved_pos;

            /* Peek at next non-whitespace */
            size_t peek_pos = f.pos;
            while (peek_pos < f.len && (f.src[peek_pos] == ' ' || f.src[peek_pos] == '\t' || f.src[peek_pos] == '\n' ||
                                        f.src[peek_pos] == '\r'))
                peek_pos++;

            char next_nonws = (peek_pos < f.len) ? f.src[peek_pos] : '\0';

            /* Check if next is 'else' or 'catch' — keep on same line */
            if (peek_pos + 4 <= f.len && strncmp(f.src + peek_pos, "else", 4) == 0 &&
                (peek_pos + 4 >= f.len || !is_ident_char(f.src[peek_pos + 4]))) {
                emit_char(&f, ' ');
                /* Skip whitespace to reach 'else' */
                f.pos = peek_pos;
                continue;
            }
            if (peek_pos + 5 <= f.len && strncmp(f.src + peek_pos, "catch", 5) == 0 &&
                (peek_pos + 5 >= f.len || !is_ident_char(f.src[peek_pos + 5]))) {
                emit_char(&f, ' ');
                f.pos = peek_pos;
                continue;
            }

            /* Check if next is ',' — keep on same line (struct literal, etc.) */
            if (next_nonws == ',') {
                f.pos = peek_pos;
                emit_char(&f, fmt_advance(&f)); /* emit the comma */
                f.last = LAST_COMMA;
                f.need_space = false;
                emit_newline(&f);
                continue;
            }

            /* Check if next } should be on its own line */
            if (next_nonws == '}') {
                emit_newline(&f);
                continue;
            }

            /* After a top-level closing brace, we may want a blank line
             * before the next declaration (handled by blank line logic above) */
            (void)nl;
            emit_newline(&f);
            continue;
        }

        /* ── Opening paren ( ── */
        if (c == '(') {
            if (f.at_line_start) {
                emit_indent(&f);
            } else {
                /* Space before ( if preceded by flow keyword */
                if (f.last == LAST_KEYWORD && is_flow_keyword(f.last_word)) {
                    if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                }
                /* No space before ( after ident (function call) */
                /* No space before ( after ) */
            }
            emit_char(&f, fmt_advance(&f));
            f.paren_depth++;
            f.last = LAST_OPEN_PAREN;
            f.need_space = false;
            continue;
        }

        /* ── Closing paren ) ── */
        if (c == ')') {
            /* No space before ) */
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.paren_depth--;
            if (f.paren_depth < 0) f.paren_depth = 0;
            f.last = LAST_CLOSE_PAREN;
            f.need_space = false;
            continue;
        }

        /* ── Opening bracket [ ── */
        if (c == '[') {
            if (f.at_line_start) {
                emit_indent(&f);
            } else if (f.last == LAST_KEYWORD || f.last == LAST_OPERATOR || f.last == LAST_FAT_ARROW ||
                       f.last == LAST_ARROW || f.last == LAST_COLON || f.last == LAST_COMMA ||
                       f.last == LAST_OPEN_PAREN || f.last == LAST_OPEN_BRACKET) {
                /* Space before [ when used as value (not index access) */
                if (f.need_space && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            }
            /* No space before [ after ident (index), ], ), number */
            emit_char(&f, fmt_advance(&f));
            f.bracket_depth++;
            /* Check if content spans multiple lines (multiline array) */
            {
                size_t peek = f.pos;
                bool has_newline = false;
                int depth = 1;
                while (peek < f.len && depth > 0) {
                    if (f.src[peek] == '[') depth++;
                    else if (f.src[peek] == ']') {
                        depth--;
                        if (depth == 0) break;
                    } else if (f.src[peek] == '\n') {
                        has_newline = true;
                        break;
                    } else if (f.src[peek] == '"' || f.src[peek] == '\'') {
                        /* Skip strings */
                        char q = f.src[peek++];
                        while (peek < f.len && f.src[peek] != q) {
                            if (f.src[peek] == '\\') peek++;
                            peek++;
                        }
                    }
                    peek++;
                }
                if (has_newline) { f.indent++; }
            }
            f.last = LAST_OPEN_BRACKET;
            f.need_space = false;
            continue;
        }

        /* ── Closing bracket ] ── */
        if (c == ']') {
            /* Check if this ] closes a multi-line bracket by seeing if we
             * increased indent for this bracket level */
            if (f.at_line_start) {
                /* We're on a new line before ] — this means multi-line, decrease indent */
                f.indent--;
                if (f.indent < 0) f.indent = 0;
                emit_indent(&f);
            }
            emit_char(&f, fmt_advance(&f));
            f.bracket_depth--;
            if (f.bracket_depth < 0) f.bracket_depth = 0;
            f.last = LAST_CLOSE_BRACKET;
            f.need_space = false;
            continue;
        }

        /* ── Comma ── */
        if (c == ',') {
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_COMMA;
            f.need_space = true;
            continue;
        }

        /* ── Semicolon ── */
        if (c == ';') {
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_SEMICOLON;
            f.need_space = true;
            continue;
        }

        /* ── @ (decorator/annotation) ── */
        if (c == '@') {
            space_before(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_AT;
            f.need_space = false;
            continue;
        }

        /* ── ~ (tilde / phase prefix) ── */
        if (c == '~') {
            space_before(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_TILDE;
            f.need_space = false;
            continue;
        }

        /* ── Dot operators ── */
        if (c == '.') {
            if (fmt_peek_at(&f, 1) == '.' && fmt_peek_at(&f, 2) == '.') {
                /* ... (spread/variadic) */
                if (!f.at_line_start && f.last != LAST_OPEN_PAREN && f.last != LAST_OPEN_BRACKET &&
                    f.last != LAST_COMMA)
                    if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_DOTDOTDOT;
                f.need_space = false;
                continue;
            }
            if (fmt_peek_at(&f, 1) == '.') {
                /* .. (range) — spaces around it */
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_DOTDOT;
                f.need_space = false;
                continue;
            }
            /* Single dot (field access / method call) — no spaces */
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_DOT;
            f.need_space = false;
            continue;
        }

        /* ── Colon ── */
        if (c == ':') {
            if (fmt_peek_at(&f, 1) == ':') {
                /* :: (namespace) — no spaces */
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_COLONCOLON;
                f.need_space = false;
                continue;
            }
            /* Single colon (type annotation, struct field) — space after */
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_COLON;
            f.need_space = true;
            continue;
        }

        /* ── Arrow -> ── */
        if (c == '-' && fmt_peek_at(&f, 1) == '>') {
            if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_ARROW;
            f.need_space = true;
            continue;
        }

        /* ── Fat arrow => ── */
        if (c == '=' && fmt_peek_at(&f, 1) == '>') {
            if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_FAT_ARROW;
            f.need_space = true;
            continue;
        }

        /* ── Question mark operators ── */
        if (c == '?') {
            if (fmt_peek_at(&f, 1) == '?') {
                /* ?? (nil coalesce) — spaces */
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            if (fmt_peek_at(&f, 1) == '.') {
                /* ?. (optional chaining) — no spaces */
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_DOT;
                f.need_space = false;
                continue;
            }
            if (fmt_peek_at(&f, 1) == '[') {
                /* ?[ (optional index) — no spaces */
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.bracket_depth++;
                f.last = LAST_OPEN_BRACKET;
                f.need_space = false;
                continue;
            }
            /* Single ? (try propagate) — no space before */
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_QUESTION;
            f.need_space = false;
            continue;
        }

        /* ── Pipe | (closure params or bitwise OR) ── */
        if (c == '|') {
            if (fmt_peek_at(&f, 1) == '|') {
                /* || (logical OR) — spaces */
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            if (fmt_peek_at(&f, 1) == '=') {
                /* |= (compound assign) — spaces */
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            /* Single | — closure param delimiter or bitwise OR */
            if (f.in_closure_params) {
                /* Closing | of closure params — no space before */
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_PIPE;
                f.in_closure_params = false;
                f.need_space = true;
                continue;
            }
            /* Determine if this is an opening | for closure params or bitwise OR.
             * Heuristic: | starts closure params when preceded by:
             * colon, comma, open-paren, open-bracket, fat-arrow, operator,
             * keyword, open-brace, or at start of statement */
            if (f.at_line_start) emit_indent(&f);
            else if (f.last == LAST_COLON || f.last == LAST_COMMA || f.last == LAST_OPEN_PAREN ||
                     f.last == LAST_OPEN_BRACKET || f.last == LAST_FAT_ARROW || f.last == LAST_OPERATOR ||
                     f.last == LAST_KEYWORD || f.last == LAST_OPEN_BRACE) {
                /* Closure context — space before | if needed */
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ' && f.out.data[f.out.len - 1] != '\n')
                    emit_char(&f, ' ');
                f.in_closure_params = true;
            } else if (f.last == LAST_PIPE) {
                /* Empty params: || — treat as closing */
                f.in_closure_params = false;
            } else {
                /* Bitwise OR — spaces on both sides */
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            }
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_PIPE;
            f.need_space = false;
            continue;
        }

        /* ── Ampersand ── */
        if (c == '&') {
            if (fmt_peek_at(&f, 1) == '&') {
                /* && (logical AND) — spaces */
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            if (fmt_peek_at(&f, 1) == '=') {
                /* &= (compound assign) — spaces */
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            /* Single & — bitwise AND */
            if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_OPERATOR;
            f.need_space = true;
            continue;
        }

        /* ── Caret ^ ── */
        if (c == '^') {
            if (fmt_peek_at(&f, 1) == '=') {
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_OPERATOR;
            f.need_space = true;
            continue;
        }

        /* ── Bang ! ── */
        if (c == '!') {
            if (fmt_peek_at(&f, 1) == '=') {
                /* != — spaces */
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            /* Unary ! — no space after */
            space_before(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_BANG;
            f.need_space = false;
            continue;
        }

        /* ── Plus + / += ── */
        if (c == '+') {
            if (fmt_peek_at(&f, 1) == '=') {
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            /* Binary + — spaces */
            if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_OPERATOR;
            f.need_space = true;
            continue;
        }

        /* ── Minus - / -= / -> (-> handled above) ── */
        if (c == '-') {
            if (fmt_peek_at(&f, 1) == '=') {
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            if (is_unary_minus(&f)) {
                /* Unary minus — no space after */
                space_before(&f);
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = false;
                continue;
            }
            /* Binary - — spaces */
            if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_OPERATOR;
            f.need_space = true;
            continue;
        }

        /* ── Star * / *= ── */
        if (c == '*') {
            if (fmt_peek_at(&f, 1) == '=') {
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            /* Could be binary * (multiply) or phase prefix */
            if (f.last == LAST_IDENT || f.last == LAST_NUMBER || f.last == LAST_CLOSE_PAREN ||
                f.last == LAST_CLOSE_BRACKET || f.last == LAST_CLOSE_BRACE || f.last == LAST_STRING) {
                /* Binary multiply — spaces */
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
            } else {
                /* Phase prefix * — no space after */
                space_before(&f);
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_STAR;
                f.need_space = false;
            }
            continue;
        }

        /* ── Slash / /= ── */
        if (c == '/') {
            if (fmt_peek_at(&f, 1) == '=') {
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            /* Binary / — spaces */
            if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_OPERATOR;
            f.need_space = true;
            continue;
        }

        /* ── Percent % / %= ── */
        if (c == '%') {
            if (fmt_peek_at(&f, 1) == '=') {
                if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                if (f.at_line_start) emit_indent(&f);
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            if (!f.at_line_start && f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            if (f.at_line_start) emit_indent(&f);
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_OPERATOR;
            f.need_space = true;
            continue;
        }

        /* ── Comparison / assignment operators ── */
        if (c == '=' || c == '<' || c == '>') {
            if (f.at_line_start) emit_indent(&f);

            /* Check for multi-char operators */
            char next = fmt_peek_at(&f, 1);

            if (c == '=' && next == '=') {
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            if (c == '<' && next == '<') {
                char next2 = fmt_peek_at(&f, 2);
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                if (next2 == '=') { emit_char(&f, fmt_advance(&f)); /* <<= */ }
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            if (c == '>' && next == '>') {
                char next2 = fmt_peek_at(&f, 2);
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                if (next2 == '=') { emit_char(&f, fmt_advance(&f)); /* >>= */ }
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            if ((c == '<' || c == '>') && next == '=') {
                if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
                emit_char(&f, fmt_advance(&f));
                emit_char(&f, fmt_advance(&f));
                f.last = LAST_OPERATOR;
                f.need_space = true;
                continue;
            }
            /* Single = (assignment), <, > */
            if (f.out.len > 0 && f.out.data[f.out.len - 1] != ' ') emit_char(&f, ' ');
            emit_char(&f, fmt_advance(&f));
            f.last = LAST_OPERATOR;
            f.need_space = true;
            continue;
        }

        /* ── Anything else — emit as-is ── */
        space_before(&f);
        emit_char(&f, fmt_advance(&f));
        f.last = LAST_OPERATOR;
        f.need_space = false;
    }

    /* Ensure final newline */
    if (f.out.len > 0) {
        /* Trim trailing whitespace/blank lines */
        while (f.out.len > 0 && (f.out.data[f.out.len - 1] == ' ' || f.out.data[f.out.len - 1] == '\t')) {
            f.out.len--;
        }
        /* Trim multiple trailing newlines down to exactly one */
        while (f.out.len > 1 && f.out.data[f.out.len - 1] == '\n' && f.out.data[f.out.len - 2] == '\n') { f.out.len--; }
        /* Ensure exactly one trailing newline */
        if (f.out.data[f.out.len - 1] != '\n') {
            f.out.data[f.out.len] = '\0';
            lat_str_push(&f.out, '\n');
        }
        f.out.data[f.out.len] = '\0';
    }

    if (err) *err = NULL;

    /* Return ownership of the string data */
    char *result = f.out.data;
    /* Don't free f.out — we're transferring ownership */
    return result;
}

bool lat_format_check(const char *source, char **err) {
    char *formatted = lat_format(source, err);
    if (!formatted) return false;

    bool same = (strcmp(source, formatted) == 0);
    free(formatted);
    return same;
}

char *lat_format_stdin(char **err) {
    /* Read all of stdin into a buffer */
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        if (err) *err = strdup("out of memory");
        return NULL;
    }

    int c;
    while ((c = getchar()) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *new_buf = realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                if (err) *err = strdup("out of memory");
                return NULL;
            }
            buf = new_buf;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';

    char *result = lat_format(buf, err);
    free(buf);
    return result;
}
