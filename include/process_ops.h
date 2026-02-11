#ifndef PROCESS_OPS_H
#define PROCESS_OPS_H

#include "value.h"

/* Run a shell command and return a Map with keys:
 *   "status" (Int)  - exit status code
 *   "stdout" (String) - captured standard output
 *   "stderr" (String) - captured standard error
 * On failure (e.g. fork fails), sets *err to a heap-allocated message. */
LatValue process_exec(const char *cmd, char **err);

#endif /* PROCESS_OPS_H */
