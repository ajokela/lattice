#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "token.h"
#include "ds/vec.h"

typedef struct {
    Token *tokens;
    size_t count;
    size_t pos;
} Parser;

/* Create a parser from a LatVec of Token */
Parser parser_new(LatVec *tokens);

/* Parse entire program. On error, sets *err to heap-allocated message. */
Program parser_parse(Parser *p, char **err);

#endif /* PARSER_H */
