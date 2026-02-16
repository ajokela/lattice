#ifndef TOKEN_H
#define TOKEN_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    /* Keywords */
    TOK_FLUX, TOK_FIX, TOK_LET, TOK_FREEZE, TOK_THAW, TOK_FORGE,
    TOK_FN, TOK_STRUCT, TOK_IF, TOK_ELSE, TOK_FOR, TOK_IN,
    TOK_WHILE, TOK_LOOP, TOK_RETURN, TOK_BREAK, TOK_CONTINUE,
    TOK_SPAWN, TOK_TRUE, TOK_FALSE, TOK_NIL, TOK_CLONE, TOK_ANNEAL, TOK_PRINT,
    TOK_TRY, TOK_CATCH,
    TOK_SCOPE,
    TOK_TEST,
    TOK_MATCH,
    TOK_ENUM,
    TOK_IMPORT,
    TOK_FROM,
    TOK_AS,
    TOK_CRYSTALLIZE,
    TOK_SUBLIMATE,

    /* Mode directive */
    TOK_MODE_DIRECTIVE,   /* #mode casual / #mode strict */

    /* Identifiers and literals */
    TOK_IDENT,
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,

    /* Phase prefixes */
    TOK_TILDE,    /* ~ */
    TOK_STAR,     /* * */

    /* Operators */
    TOK_PLUS, TOK_MINUS, TOK_SLASH, TOK_PERCENT,
    TOK_EQ,       /* = */
    TOK_EQEQ,     /* == */
    TOK_BANGEQ,   /* != */
    TOK_LT,       /* < */
    TOK_GT,       /* > */
    TOK_LTEQ,     /* <= */
    TOK_GTEQ,     /* >= */
    TOK_AND,       /* && */
    TOK_OR,        /* || */
    TOK_BANG,      /* ! */
    TOK_DOT,       /* . */
    TOK_DOTDOT,    /* .. */
    TOK_DOTDOTDOT, /* ... */
    TOK_ARROW,     /* -> */
    TOK_FATARROW,  /* => */
    TOK_QUESTION_QUESTION, /* ?? */
    TOK_PIPE,      /* | */
    TOK_AMPERSAND, /* & */
    TOK_CARET,     /* ^ */
    TOK_LSHIFT,    /* << */
    TOK_RSHIFT,    /* >> */

    /* Compound assignment */
    TOK_PLUS_EQ,    /* += */
    TOK_MINUS_EQ,   /* -= */
    TOK_STAR_EQ,    /* *= */
    TOK_SLASH_EQ,   /* /= */
    TOK_PERCENT_EQ, /* %= */
    TOK_AMP_EQ,     /* &= */
    TOK_PIPE_EQ,    /* |= */
    TOK_CARET_EQ,   /* ^= */
    TOK_LSHIFT_EQ,  /* <<= */
    TOK_RSHIFT_EQ,  /* >>= */

    /* Delimiters */
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,

    /* Punctuation */
    TOK_COMMA, TOK_COLON, TOK_COLONCOLON, TOK_SEMICOLON,

    /* String interpolation */
    TOK_INTERP_START,  /* first segment of interpolated string */
    TOK_INTERP_MID,    /* middle segment between interpolations */
    TOK_INTERP_END,    /* final segment of interpolated string */

    /* Special */
    TOK_EOF,
} TokenType;

/* Token with its associated data */
typedef struct {
    TokenType type;
    union {
        int64_t int_val;
        double  float_val;
        char   *str_val;    /* heap-allocated for IDENT, STRING_LIT, MODE_DIRECTIVE */
    } as;
    size_t line;
    size_t col;
} Token;

/* Create a simple token (no payload) */
Token token_simple(TokenType type, size_t line, size_t col);

/* Create a token with string payload (takes ownership of str) */
Token token_str(TokenType type, char *str, size_t line, size_t col);

/* Create a token with int payload */
Token token_int(int64_t val, size_t line, size_t col);

/* Create a token with float payload */
Token token_float(double val, size_t line, size_t col);

/* Free a token's heap data */
void token_free(Token *t);

/* Get display name of a token type */
const char *token_type_name(TokenType type);

#endif /* TOKEN_H */
