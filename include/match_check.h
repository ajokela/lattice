#ifndef MATCH_CHECK_H
#define MATCH_CHECK_H

#include "ast.h"

/* Run match exhaustiveness checking on a program.
 * Walks all expressions looking for EXPR_MATCH, then checks whether the
 * arms cover all possible values.  Warnings are printed to stderr.
 * Returns the number of warnings emitted. */
int check_match_exhaustiveness(const Program *prog);

#endif /* MATCH_CHECK_H */
