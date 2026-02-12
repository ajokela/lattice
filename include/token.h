#ifndef TOKEN_H
#define TOKEN_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    /* Keywords */
    TOK_FLUX, TOK_FIX, TOK_LET, TOK_FREEZE, TOK_THAW, TOK_FORGE,
    TOK_FN, TOK_STRUCT, TOK_IF, TOK_ELSE, TOK_FOR, TOK_IN,
    TOK_WHILE, TOK_LOOP, TOK_RETURN, TOK_BREAK, TOK_CONTINUE,
    TOK_SPAWN, TOK_TRUE, TOK_FALSE, TOK_CLONE, TOK_PRINT,
    TOK_TRY, TOK_CATCH,
    TOK_SCOPE,

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
    TOK_ARROW,     /* -> */
    TOK_FATARROW,  /* => */
    TOK_PIPE,      /* | */
    TOK_AMPERSAND, /* & */

    /* Compound assignment */
    TOK_PLUS_EQ,    /* += */
    TOK_MINUS_EQ,   /* -= */
    TOK_STAR_EQ,    /* *= */
    TOK_SLASH_EQ,   /* /= */
    TOK_PERCENT_EQ, /* %= */

    /* Delimiters */
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,

    /* Punctuation */
    TOK_COMMA, TOK_COLON, TOK_COLONCOLON, TOK_SEMICOLON,

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
