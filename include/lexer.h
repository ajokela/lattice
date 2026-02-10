#ifndef LEXER_H
#define LEXER_H

#include "token.h"
#include "ds/vec.h"

typedef struct {
    const char *source;
    size_t      len;
    size_t      pos;
    size_t      line;
    size_t      col;
} Lexer;

/* Initialize a lexer with source code */
Lexer lexer_new(const char *source);

/* Tokenize the entire source. Returns a LatVec of Token.
 * On error, sets *err to a heap-allocated error message and returns empty vec. */
LatVec lexer_tokenize(Lexer *lex, char **err);

#endif /* LEXER_H */
