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

/* Run a program directly, without a shell.
 *
 * args must contain:
 *   program (String)     - executable name/path and argv[0]
 *   argv    ([String])   - arguments after argv[0]
 *   stdin   (String|Nil) - exact standard input; Nil closes stdin immediately
 *   options  (Map|Nil)   - optional positive integer timeout/output limits
 *
 * Returns a Map with "exit_code", "stdout", and "stderr". A non-zero child
 * exit is returned as data. Timeout/output-limit, validation, and process/spawn
 * failures set *err. Three arguments retain the unbounded behavior. This
 * function owns no values in args. */
LatValue process_exec_argv(LatValue *args, int arg_count, char **err);

/* Return the current working directory as a heap-allocated string.
 * On failure, sets *err and returns NULL. */
char *process_cwd(char **err);

/* Return the OS platform as a static string: "macos", "linux", "windows", "wasm", or "unknown". */
const char *process_platform(void);

/* Return the system hostname as a heap-allocated string.
 * On failure, sets *err and returns NULL. */
char *process_hostname(char **err);

/* Return the current process ID. */
int process_pid(void);

#endif /* PROCESS_OPS_H */
