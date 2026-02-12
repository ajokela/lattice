#ifndef PROCESS_OPS_H
#define PROCESS_OPS_H

#include "value.h"

/* Run a shell command and return its stdout as a String.
 * If the command exits with a non-zero status, sets *err.
 * On failure (e.g. popen fails), sets *err. */
LatValue process_exec(const char *cmd, char **err);

/* Run a shell command and return a Map with keys:
 *   "exit_code" (Int)    - exit status code
 *   "stdout"   (String)  - captured standard output
 *   "stderr"   (String)  - captured standard error
 * On failure (e.g. fork fails), sets *err to a heap-allocated message. */
LatValue process_shell(const char *cmd, char **err);

/* Return the current working directory as a heap-allocated string.
 * On failure, sets *err and returns NULL. */
char *process_cwd(char **err);

#endif /* PROCESS_OPS_H */
