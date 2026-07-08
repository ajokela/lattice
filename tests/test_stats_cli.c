/* CLI integration tests for the `--stats` flag (LAT-455).
 *
 * `--stats` is implemented in main.c's run_source(), which is NOT linked into
 * the in-process test_runner (main.c has its own main()). So these tests drive
 * the built `./clat` binary as a subprocess and inspect its stderr, where the
 * memory report is emitted on every backend.
 *
 * Regression guard: before LAT-455, `--stats` printed a full report on the
 * tree-walker but NOTHING on the default stack VM (and regvm) path. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Import test harness from test_main.c */
extern void register_test(const char *name, void (*fn)(void));
extern int test_current_failed;

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            test_current_failed = 1;                                           \
            return;                                                            \
        }                                                                      \
    } while (0)

#define TEST(name)                                                    \
    static void name(void);                                           \
    static void name##_register(void) __attribute__((constructor));   \
    static void name##_register(void) { register_test(#name, name); } \
    static void name(void)

/* Subprocess-driven tests need fork/exec + pipes; POSIX only. Also self-skip
 * under ThreadSanitizer: forking from an already-threaded TSan process is
 * unsupported (see tests/test_memory.c). */
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
#define STATS_CLI_UNSUPPORTED 1
#endif
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define STATS_CLI_UNSUPPORTED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define STATS_CLI_UNSUPPORTED 1
#endif

#ifndef STATS_CLI_UNSUPPORTED

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

/* Skip gracefully when the CLI binary hasn't been built (e.g. `make test`
 * without a prior `make`). */
static int clat_available(void) { return access("./clat", X_OK) == 0; }

#define SKIP_IF_NO_CLAT()                                  \
    do {                                                   \
        if (!clat_available()) {                           \
            fprintf(stderr, "  skip: ./clat not found\n"); \
            return;                                        \
        }                                                  \
    } while (0)

/* Fork ./clat with the given NULL-terminated argv, discard its stdout, and
 * capture its stderr into buf. Returns the child's exit code (>=0) or -1 on
 * spawn failure. */
static int run_clat_capture_stderr(char *const argv[], char *buf, size_t buf_sz) {
    int err_pipe[2];
    if (pipe(err_pipe) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: stderr -> pipe, stdout -> /dev/null (program output noise). */
        close(err_pipe[0]);
        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execv(argv[0], argv);
        _exit(127);
    }

    /* Parent: drain the pipe fully so the child never blocks on a full pipe. */
    close(err_pipe[1]);
    size_t total = 0;
    for (;;) {
        if (total + 1 >= buf_sz) break;
        ssize_t n = read(err_pipe[0], buf + total, buf_sz - 1 - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n == 0) {
            break;
        } else {
            break;
        }
    }
    buf[total] = '\0';
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Write a small program that freezes an array (populating the shared-crystal
 * region registry the --stats report reads) to a temp file. Returns 0 on
 * success and fills path_out (must be >= 64 bytes). */
static int write_stats_program(char *path_out, size_t path_sz) {
    if (path_sz < 40) return -1;
    strcpy(path_out, "/tmp/lat_stats_cli_XXXXXX");
    int fd = mkstemp(path_out);
    if (fd < 0) return -1;
    const char *src = "let a = [1, 2, 3, \"stats cli padded 0123456789 0123456789\"]\n"
                      "fix b = a\n"
                      "print(b[0])\n";
    size_t len = strlen(src);
    ssize_t w = write(fd, src, len);
    close(fd);
    return (w == (ssize_t)len) ? 0 : -1;
}

/* Regression: the default (stack VM) backend must emit the --stats report. */
TEST(stats_flag_stackvm_emits_report) {
    SKIP_IF_NO_CLAT();
    char path[64];
    ASSERT(write_stats_program(path, sizeof(path)) == 0);

    char out[8192];
    char *const argv[] = {(char *)"./clat", (char *)"--stats", path, NULL};
    int code = run_clat_capture_stderr(argv, out, sizeof(out));
    unlink(path);

    ASSERT(code == 0);
    /* Before LAT-455 this was EMPTY on the stack VM. */
    ASSERT(strstr(out, "=== Memory Statistics ===") != NULL);
    ASSERT(strstr(out, "region peak:") != NULL);
    ASSERT(strstr(out, "region live:") != NULL);
}

/* The regvm backend must also emit the report. */
TEST(stats_flag_regvm_emits_report) {
    SKIP_IF_NO_CLAT();
    char path[64];
    ASSERT(write_stats_program(path, sizeof(path)) == 0);

    char out[8192];
    char *const argv[] = {(char *)"./clat", (char *)"--regvm", (char *)"--stats", path, NULL};
    int code = run_clat_capture_stderr(argv, out, sizeof(out));
    unlink(path);

    ASSERT(code == 0);
    ASSERT(strstr(out, "=== Memory Statistics ===") != NULL);
    ASSERT(strstr(out, "region peak:") != NULL);
}

/* Control: the tree-walker path must keep emitting its report. */
TEST(stats_flag_treewalk_still_emits_report) {
    SKIP_IF_NO_CLAT();
    char path[64];
    ASSERT(write_stats_program(path, sizeof(path)) == 0);

    char out[8192];
    char *const argv[] = {(char *)"./clat", (char *)"--tree-walk", (char *)"--stats", path, NULL};
    int code = run_clat_capture_stderr(argv, out, sizeof(out));
    unlink(path);

    ASSERT(code == 0);
    ASSERT(strstr(out, "=== Memory Statistics ===") != NULL);
}

#endif /* !STATS_CLI_UNSUPPORTED */
