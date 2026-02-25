#include "token.h"
#include <stdlib.h>
#include <string.h>

Token token_simple(TokenType type, size_t line, size_t col) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = type;
    t.line = line;
    t.col = col;
    return t;
}

Token token_str(TokenType type, char *str, size_t line, size_t col) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = type;
    t.as.str_val = str;
    t.line = line;
    t.col = col;
    return t;
}

Token token_int(int64_t val, size_t line, size_t col) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = TOK_INT_LIT;
    t.as.int_val = val;
    t.line = line;
    t.col = col;
    return t;
}

Token token_float(double val, size_t line, size_t col) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = TOK_FLOAT_LIT;
    t.as.float_val = val;
    t.line = line;
    t.col = col;
    return t;
}

void token_free(Token *t) {
    if (t->type == TOK_IDENT || t->type == TOK_STRING_LIT ||
        t->type == TOK_MODE_DIRECTIVE ||
        t->type == TOK_INTERP_START || t->type == TOK_INTERP_MID ||
        t->type == TOK_INTERP_END) {
        free(t->as.str_val);
        t->as.str_val = NULL;
    }
}

static const char *simple_names[] = {
    [TOK_FLUX] = "flux", [TOK_FIX] = "fix", [TOK_LET] = "let",
    [TOK_FREEZE] = "freeze", [TOK_THAW] = "thaw", [TOK_FORGE] = "forge",
    [TOK_FN] = "fn", [TOK_STRUCT] = "struct",
    [TOK_IF] = "if", [TOK_ELSE] = "else", [TOK_FOR] = "for", [TOK_IN] = "in",
    [TOK_WHILE] = "while", [TOK_LOOP] = "loop",
    [TOK_RETURN] = "return", [TOK_BREAK] = "break", [TOK_CONTINUE] = "continue",
    [TOK_SPAWN] = "spawn", [TOK_TRUE] = "true", [TOK_FALSE] = "false", [TOK_NIL] = "nil",
    [TOK_CLONE] = "clone", [TOK_PRINT] = "print",
    [TOK_TRY] = "try", [TOK_CATCH] = "catch", [TOK_SCOPE] = "scope", [TOK_TEST] = "test", [TOK_MATCH] = "match", [TOK_ENUM] = "enum",
    [TOK_IMPORT] = "import", [TOK_FROM] = "from", [TOK_AS] = "as",
    [TOK_CRYSTALLIZE] = "crystallize", [TOK_BORROW] = "borrow",
    [TOK_SUBLIMATE] = "sublimate",
    [TOK_DEFER] = "defer", [TOK_SELECT] = "select",
    [TOK_TRAIT] = "trait", [TOK_IMPL] = "impl",
    [TOK_TILDE] = "~", [TOK_STAR] = "*",
    [TOK_PLUS] = "+", [TOK_MINUS] = "-", [TOK_SLASH] = "/", [TOK_PERCENT] = "%",
    [TOK_EQ] = "=", [TOK_EQEQ] = "==", [TOK_BANGEQ] = "!=",
    [TOK_LT] = "<", [TOK_GT] = ">", [TOK_LTEQ] = "<=", [TOK_GTEQ] = ">=",
    [TOK_AND] = "&&", [TOK_OR] = "||", [TOK_BANG] = "!",
    [TOK_DOT] = ".", [TOK_DOTDOT] = "..", [TOK_DOTDOTDOT] = "...", [TOK_ARROW] = "->", [TOK_FATARROW] = "=>",
    [TOK_QUESTION_QUESTION] = "??", [TOK_QUESTION_DOT] = "?.", [TOK_QUESTION_LBRACKET] = "?[", [TOK_QUESTION] = "?",
    [TOK_PIPE] = "|", [TOK_AMPERSAND] = "&",
    [TOK_CARET] = "^", [TOK_LSHIFT] = "<<", [TOK_RSHIFT] = ">>",
    [TOK_PLUS_EQ] = "+=", [TOK_MINUS_EQ] = "-=", [TOK_STAR_EQ] = "*=",
    [TOK_SLASH_EQ] = "/=", [TOK_PERCENT_EQ] = "%=",
    [TOK_AMP_EQ] = "&=", [TOK_PIPE_EQ] = "|=", [TOK_CARET_EQ] = "^=",
    [TOK_LSHIFT_EQ] = "<<=", [TOK_RSHIFT_EQ] = ">>=",
    [TOK_LPAREN] = "(", [TOK_RPAREN] = ")",
    [TOK_LBRACE] = "{", [TOK_RBRACE] = "}",
    [TOK_LBRACKET] = "[", [TOK_RBRACKET] = "]",
    [TOK_COMMA] = ",", [TOK_COLON] = ":", [TOK_COLONCOLON] = "::",
    [TOK_SEMICOLON] = ";",
    [TOK_AT] = "@",
    [TOK_EOF] = "EOF",
};

const char *token_type_name(TokenType type) {
    if (type == TOK_MODE_DIRECTIVE) return "#mode";
    if (type == TOK_IDENT) return "identifier";
    if (type == TOK_INT_LIT) return "integer";
    if (type == TOK_FLOAT_LIT) return "float";
    if (type == TOK_STRING_LIT) return "string";
    if (type == TOK_INTERP_START) return "INTERP_START";
    if (type == TOK_INTERP_MID) return "INTERP_MID";
    if (type == TOK_INTERP_END) return "INTERP_END";
    if ((size_t)type < sizeof(simple_names)/sizeof(simple_names[0]) && simple_names[type]) {
        return simple_names[type];
    }
    return "?";
}
