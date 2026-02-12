#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

Lexer lexer_new(const char *source) {
    Lexer lex;
    lex.source = source;
    lex.len = strlen(source);
    lex.pos = 0;
    lex.line = 1;
    lex.col = 1;
    return lex;
}

static char lex_peek(const Lexer *lex) {
    if (lex->pos >= lex->len) return '\0';
    return lex->source[lex->pos];
}

static char lex_peek_ahead(const Lexer *lex, size_t offset) {
    size_t idx = lex->pos + offset;
    if (idx >= lex->len) return '\0';
    return lex->source[idx];
}

static char lex_advance(Lexer *lex) {
    if (lex->pos >= lex->len) return '\0';
    char ch = lex->source[lex->pos++];
    if (ch == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return ch;
}

static void skip_whitespace_and_comments(Lexer *lex) {
    for (;;) {
        /* Skip whitespace */
        while (lex->pos < lex->len && isspace((unsigned char)lex_peek(lex))) {
            lex_advance(lex);
        }
        /* Line comment */
        if (lex_peek(lex) == '/' && lex_peek_ahead(lex, 1) == '/') {
            while (lex->pos < lex->len && lex_peek(lex) != '\n') {
                lex_advance(lex);
            }
            continue;
        }
        /* Block comment (nestable) */
        if (lex_peek(lex) == '/' && lex_peek_ahead(lex, 1) == '*') {
            lex_advance(lex); /* / */
            lex_advance(lex); /* * */
            int depth = 1;
            while (depth > 0 && lex->pos < lex->len) {
                char c = lex_advance(lex);
                if (c == '/' && lex_peek(lex) == '*') {
                    lex_advance(lex);
                    depth++;
                } else if (c == '*' && lex_peek(lex) == '/') {
                    lex_advance(lex);
                    depth--;
                }
            }
            continue;
        }
        break;
    }
}

static char *read_ident(Lexer *lex) {
    size_t start = lex->pos;
    while (lex->pos < lex->len && (isalnum((unsigned char)lex_peek(lex)) || lex_peek(lex) == '_')) {
        lex_advance(lex);
    }
    size_t len = lex->pos - start;
    char *s = malloc(len + 1);
    memcpy(s, lex->source + start, len);
    s[len] = '\0';
    return s;
}

static TokenType keyword_lookup(const char *ident) {
    if (strcmp(ident, "flux") == 0)     return TOK_FLUX;
    if (strcmp(ident, "fix") == 0)      return TOK_FIX;
    if (strcmp(ident, "let") == 0)      return TOK_LET;
    if (strcmp(ident, "freeze") == 0)   return TOK_FREEZE;
    if (strcmp(ident, "thaw") == 0)     return TOK_THAW;
    if (strcmp(ident, "forge") == 0)    return TOK_FORGE;
    if (strcmp(ident, "fn") == 0)       return TOK_FN;
    if (strcmp(ident, "struct") == 0)   return TOK_STRUCT;
    if (strcmp(ident, "if") == 0)       return TOK_IF;
    if (strcmp(ident, "else") == 0)     return TOK_ELSE;
    if (strcmp(ident, "for") == 0)      return TOK_FOR;
    if (strcmp(ident, "in") == 0)       return TOK_IN;
    if (strcmp(ident, "while") == 0)    return TOK_WHILE;
    if (strcmp(ident, "loop") == 0)     return TOK_LOOP;
    if (strcmp(ident, "return") == 0)   return TOK_RETURN;
    if (strcmp(ident, "break") == 0)    return TOK_BREAK;
    if (strcmp(ident, "continue") == 0) return TOK_CONTINUE;
    if (strcmp(ident, "spawn") == 0)    return TOK_SPAWN;
    if (strcmp(ident, "true") == 0)     return TOK_TRUE;
    if (strcmp(ident, "false") == 0)    return TOK_FALSE;
    if (strcmp(ident, "clone") == 0)    return TOK_CLONE;
    if (strcmp(ident, "print") == 0)    return TOK_PRINT;
    if (strcmp(ident, "try") == 0)      return TOK_TRY;
    if (strcmp(ident, "catch") == 0)    return TOK_CATCH;
    if (strcmp(ident, "scope") == 0)    return TOK_SCOPE;
    if (strcmp(ident, "test") == 0)     return TOK_TEST;
    if (strcmp(ident, "match") == 0)    return TOK_MATCH;
    if (strcmp(ident, "enum") == 0)     return TOK_ENUM;
    return TOK_IDENT;
}

/* Forward declarations for mutual recursion (string interpolation) */
static bool lex_string_or_interp(Lexer *lex, LatVec *tokens, char **err);
static bool lex_one(Lexer *lex, LatVec *tokens, char **err);

static bool next_token(Lexer *lex, Token *out, char **err) {
    size_t line = lex->line;
    size_t col = lex->col;
    char ch = lex_peek(lex);

    /* Mode directive: #mode */
    if (ch == '#') {
        lex_advance(lex);
        char *word = read_ident(lex);
        if (strcmp(word, "mode") != 0) {
            *err = NULL;
            (void)asprintf(err, "%zu:%zu: unexpected directive '#%s'", line, col, word);
            free(word);
            return false;
        }
        free(word);
        skip_whitespace_and_comments(lex);
        char *mode = read_ident(lex);
        if (strcmp(mode, "casual") != 0 && strcmp(mode, "strict") != 0) {
            *err = NULL;
            (void)asprintf(err, "%zu:%zu: expected 'casual' or 'strict' after #mode, got '%s'", line, col, mode);
            free(mode);
            return false;
        }
        *out = token_str(TOK_MODE_DIRECTIVE, mode, line, col);
        return true;
    }

    /* String literals are handled by lex_string_or_interp() via lex_one() */

    /* Number literal */
    if (isdigit((unsigned char)ch)) {
        size_t start = lex->pos;
        bool is_float = false;
        while (lex->pos < lex->len && isdigit((unsigned char)lex_peek(lex))) {
            lex_advance(lex);
        }
        if (lex_peek(lex) == '.' && isdigit((unsigned char)lex_peek_ahead(lex, 1))) {
            is_float = true;
            lex_advance(lex); /* '.' */
            while (lex->pos < lex->len && isdigit((unsigned char)lex_peek(lex))) {
                lex_advance(lex);
            }
        }
        size_t num_len = lex->pos - start;
        char *num_str = malloc(num_len + 1);
        memcpy(num_str, lex->source + start, num_len);
        num_str[num_len] = '\0';
        if (is_float) {
            double val = strtod(num_str, NULL);
            free(num_str);
            *out = token_float(val, line, col);
        } else {
            int64_t val = strtoll(num_str, NULL, 10);
            free(num_str);
            *out = token_int(val, line, col);
        }
        return true;
    }

    /* Identifiers and keywords */
    if (isalpha((unsigned char)ch) || ch == '_') {
        char *ident = read_ident(lex);
        TokenType type = keyword_lookup(ident);
        if (type != TOK_IDENT) {
            free(ident);
            *out = token_simple(type, line, col);
        } else {
            *out = token_str(TOK_IDENT, ident, line, col);
        }
        return true;
    }

    /* Operators and punctuation */
    lex_advance(lex);
    switch (ch) {
        case '~': *out = token_simple(TOK_TILDE, line, col); return true;
        case '+':
            if (lex_peek(lex) == '=') { lex_advance(lex); *out = token_simple(TOK_PLUS_EQ, line, col); }
            else { *out = token_simple(TOK_PLUS, line, col); }
            return true;
        case '%':
            if (lex_peek(lex) == '=') { lex_advance(lex); *out = token_simple(TOK_PERCENT_EQ, line, col); }
            else { *out = token_simple(TOK_PERCENT, line, col); }
            return true;
        case '(': *out = token_simple(TOK_LPAREN, line, col); return true;
        case ')': *out = token_simple(TOK_RPAREN, line, col); return true;
        case '{': *out = token_simple(TOK_LBRACE, line, col); return true;
        case '}': *out = token_simple(TOK_RBRACE, line, col); return true;
        case '[': *out = token_simple(TOK_LBRACKET, line, col); return true;
        case ']': *out = token_simple(TOK_RBRACKET, line, col); return true;
        case ',': *out = token_simple(TOK_COMMA, line, col); return true;
        case ';': *out = token_simple(TOK_SEMICOLON, line, col); return true;
        case '/':
            if (lex_peek(lex) == '=') { lex_advance(lex); *out = token_simple(TOK_SLASH_EQ, line, col); }
            else { *out = token_simple(TOK_SLASH, line, col); }
            return true;
        case '*':
            if (lex_peek(lex) == '=') { lex_advance(lex); *out = token_simple(TOK_STAR_EQ, line, col); }
            else { *out = token_simple(TOK_STAR, line, col); }
            return true;
        case '&':
            if (lex_peek(lex) == '&') { lex_advance(lex); *out = token_simple(TOK_AND, line, col); }
            else { *out = token_simple(TOK_AMPERSAND, line, col); }
            return true;
        case '|':
            if (lex_peek(lex) == '|') { lex_advance(lex); *out = token_simple(TOK_OR, line, col); }
            else { *out = token_simple(TOK_PIPE, line, col); }
            return true;
        case '=':
            if (lex_peek(lex) == '=') { lex_advance(lex); *out = token_simple(TOK_EQEQ, line, col); }
            else if (lex_peek(lex) == '>') { lex_advance(lex); *out = token_simple(TOK_FATARROW, line, col); }
            else { *out = token_simple(TOK_EQ, line, col); }
            return true;
        case '!':
            if (lex_peek(lex) == '=') { lex_advance(lex); *out = token_simple(TOK_BANGEQ, line, col); }
            else { *out = token_simple(TOK_BANG, line, col); }
            return true;
        case '<':
            if (lex_peek(lex) == '=') { lex_advance(lex); *out = token_simple(TOK_LTEQ, line, col); }
            else { *out = token_simple(TOK_LT, line, col); }
            return true;
        case '>':
            if (lex_peek(lex) == '=') { lex_advance(lex); *out = token_simple(TOK_GTEQ, line, col); }
            else { *out = token_simple(TOK_GT, line, col); }
            return true;
        case '-':
            if (lex_peek(lex) == '>') { lex_advance(lex); *out = token_simple(TOK_ARROW, line, col); }
            else if (lex_peek(lex) == '=') { lex_advance(lex); *out = token_simple(TOK_MINUS_EQ, line, col); }
            else { *out = token_simple(TOK_MINUS, line, col); }
            return true;
        case '.':
            if (lex_peek(lex) == '.') {
                lex_advance(lex);
                if (lex_peek(lex) == '.') { lex_advance(lex); *out = token_simple(TOK_DOTDOTDOT, line, col); }
                else { *out = token_simple(TOK_DOTDOT, line, col); }
            }
            else { *out = token_simple(TOK_DOT, line, col); }
            return true;
        case ':':
            if (lex_peek(lex) == ':') { lex_advance(lex); *out = token_simple(TOK_COLONCOLON, line, col); }
            else { *out = token_simple(TOK_COLON, line, col); }
            return true;
        default:
            *err = NULL;
            (void)asprintf(err, "%zu:%zu: unexpected character '%c'", line, col, ch);
            return false;
    }
}

/* Helper: scan escape sequence inside a string, appending to buf.
 * On entry, the backslash has already been consumed. Returns false on error. */
static bool lex_string_escape(Lexer *lex, char **buf, size_t *buf_len,
                               size_t *buf_cap, size_t line, size_t col, char **err) {
    if (lex->pos >= lex->len) {
        *err = NULL;
        (void)asprintf(err, "%zu:%zu: unterminated string escape", line, col);
        return false;
    }
    char esc = lex_advance(lex);
    char c;
    switch (esc) {
        case 'n':  c = '\n'; break;
        case 't':  c = '\t'; break;
        case 'r':  c = '\r'; break;
        case '0':  c = '\0'; break;
        case '\\': c = '\\'; break;
        case '"':  c = '"';  break;
        case '$':  c = '$';  break;
        case 'x': {
            if (lex->pos + 1 >= lex->len) {
                *err = NULL;
                (void)asprintf(err, "%zu:%zu: incomplete \\x escape", line, col);
                return false;
            }
            char h1 = lex_advance(lex);
            char h2 = lex_advance(lex);
            int d1 = -1, d2 = -1;
            if (h1 >= '0' && h1 <= '9') d1 = h1 - '0';
            else if (h1 >= 'a' && h1 <= 'f') d1 = h1 - 'a' + 10;
            else if (h1 >= 'A' && h1 <= 'F') d1 = h1 - 'A' + 10;
            if (h2 >= '0' && h2 <= '9') d2 = h2 - '0';
            else if (h2 >= 'a' && h2 <= 'f') d2 = h2 - 'a' + 10;
            else if (h2 >= 'A' && h2 <= 'F') d2 = h2 - 'A' + 10;
            if (d1 < 0 || d2 < 0) {
                *err = NULL;
                (void)asprintf(err, "%zu:%zu: invalid hex escape '\\x%c%c'", line, col, h1, h2);
                return false;
            }
            c = (char)((d1 << 4) | d2);
            break;
        }
        default: c = esc; break;
    }
    if (*buf_len + 1 >= *buf_cap) {
        *buf_cap *= 2;
        *buf = realloc(*buf, *buf_cap);
    }
    (*buf)[(*buf_len)++] = c;
    return true;
}

/* Scan a string literal, handling interpolation with ${...}.
 * On entry, lex is positioned at the opening '"'.
 * Pushes TOK_STRING_LIT (no interpolation) or
 * TOK_INTERP_START / expression tokens / TOK_INTERP_MID / ... / TOK_INTERP_END. */
static bool lex_string_or_interp(Lexer *lex, LatVec *tokens, char **err) {
    size_t line = lex->line;
    size_t col = lex->col;
    lex_advance(lex); /* consume opening " */

    bool has_interp = false;
    size_t buf_cap = 64;
    size_t buf_len = 0;
    char *buf = malloc(buf_cap);

    for (;;) {
        if (lex->pos >= lex->len) {
            free(buf);
            *err = NULL;
            (void)asprintf(err, "%zu:%zu: unterminated string literal", line, col);
            return false;
        }

        /* Check for interpolation: ${ */
        if (lex_peek(lex) == '$' && lex_peek_ahead(lex, 1) == '{') {
            /* Emit accumulated text as INTERP_START or INTERP_MID */
            buf[buf_len] = '\0';
            TokenType seg_type = has_interp ? TOK_INTERP_MID : TOK_INTERP_START;
            Token seg = token_str(seg_type, buf, line, col);
            lat_vec_push(tokens, &seg);
            has_interp = true;

            lex_advance(lex); /* consume $ */
            lex_advance(lex); /* consume { */

            /* Lex expression tokens until brace depth returns to 0 */
            int depth = 1;
            while (depth > 0) {
                skip_whitespace_and_comments(lex);
                if (lex->pos >= lex->len) {
                    *err = NULL;
                    (void)asprintf(err, "%zu:%zu: unterminated string interpolation", line, col);
                    return false;
                }
                /* End of interpolation */
                if (lex_peek(lex) == '}' && depth == 1) {
                    lex_advance(lex); /* consume closing } */
                    break;
                }
                /* Lex one token (handles nested strings with interpolation) */
                size_t before = tokens->len;
                if (!lex_one(lex, tokens, err)) return false;
                /* Track brace depth */
                if (tokens->len > before) {
                    Token *last = lat_vec_get(tokens, tokens->len - 1);
                    if (last->type == TOK_LBRACE) depth++;
                    else if (last->type == TOK_RBRACE) depth--;
                }
            }

            /* Reset buffer for next string segment */
            buf_cap = 64;
            buf_len = 0;
            buf = malloc(buf_cap);
            continue;
        }

        /* Check for end of string */
        if (lex_peek(lex) == '"') {
            lex_advance(lex); /* consume closing " */
            buf[buf_len] = '\0';
            if (has_interp) {
                Token seg = token_str(TOK_INTERP_END, buf, line, col);
                lat_vec_push(tokens, &seg);
            } else {
                Token seg = token_str(TOK_STRING_LIT, buf, line, col);
                lat_vec_push(tokens, &seg);
            }
            return true;
        }

        /* Check for escape sequence */
        if (lex_peek(lex) == '\\') {
            lex_advance(lex); /* consume backslash */
            if (!lex_string_escape(lex, &buf, &buf_len, &buf_cap, line, col, err)) {
                free(buf);
                return false;
            }
            continue;
        }

        /* Regular character */
        char c = lex_advance(lex);
        if (buf_len + 1 >= buf_cap) {
            buf_cap *= 2;
            buf = realloc(buf, buf_cap);
        }
        buf[buf_len++] = c;
    }
}

/* Lex one token (or multiple for interpolated strings) and push to tokens. */
static bool lex_one(Lexer *lex, LatVec *tokens, char **err) {
    skip_whitespace_and_comments(lex);
    if (lex_peek(lex) == '"') {
        return lex_string_or_interp(lex, tokens, err);
    }
    Token tok;
    if (!next_token(lex, &tok, err)) return false;
    lat_vec_push(tokens, &tok);
    return true;
}

LatVec lexer_tokenize(Lexer *lex, char **err) {
    LatVec tokens = lat_vec_new(sizeof(Token));
    *err = NULL;

    for (;;) {
        skip_whitespace_and_comments(lex);
        if (lex->pos >= lex->len) {
            Token eof = token_simple(TOK_EOF, lex->line, lex->col);
            lat_vec_push(&tokens, &eof);
            break;
        }
        if (!lex_one(lex, &tokens, err)) {
            /* Free tokens on error */
            for (size_t i = 0; i < tokens.len; i++) {
                token_free(lat_vec_get(&tokens, i));
            }
            lat_vec_free(&tokens);
            return lat_vec_new(sizeof(Token));
        }
    }

    return tokens;
}
