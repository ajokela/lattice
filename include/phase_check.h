#ifndef PHASE_CHECK_H
#define PHASE_CHECK_H

#include "ast.h"
#include "ds/vec.h"

/* Run phase checking on a program.
 * Returns NULL on success, or a LatVec of heap-allocated char* error strings. */
LatVec phase_check(const Program *prog);

#endif /* PHASE_CHECK_H */
