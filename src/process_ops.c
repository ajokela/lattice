#include "process_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static void process_set_error(char **err, const char *message) {
    if (err && !*err) *err = strdup(message);
}

static size_t process_string_len(const LatValue *value) {
    if (!value->as.str_val) return 0;
    return value->as.str_len ? value->as.str_len : strlen(value->as.str_val);
}

static bool process_string_has_nul(const LatValue *value) {
    if (!value->as.str_val) return true;
    size_t len = process_string_len(value);
    return len > 0 && memchr(value->as.str_val, '\0', len) != NULL;
}

/* Keep all user-facing validation here rather than in eval.c/runtime.c so the
 * tree walker, StackVM, RegVM, and WASM return byte-for-byte identical errors. */
static bool process_exec_argv_validate(LatValue *args, int arg_count, char **err) {
    if (arg_count != 3 || !args || args[0].type != VAL_STR || args[1].type != VAL_ARRAY ||
        (args[2].type != VAL_STR && args[2].type != VAL_NIL)) {
        process_set_error(err, "exec_argv() expects (program: String, args: [String], stdin: String|Nil)");
        return false;
    }
    if (!args[0].as.str_val || process_string_len(&args[0]) == 0) {
        process_set_error(err, "exec_argv: program must not be empty");
        return false;
    }
    if (process_string_has_nul(&args[0])) {
        process_set_error(err, "exec_argv: program must not contain NUL bytes");
        return false;
    }
    for (size_t i = 0; i < args[1].as.array.len; i++) {
        LatValue *arg = &args[1].as.array.elems[i];
        if (arg->type != VAL_STR) {
            char message[128];
            snprintf(message, sizeof(message), "exec_argv: args[%zu] must be a String", i);
            process_set_error(err, message);
            return false;
        }
        if (process_string_has_nul(arg)) {
            char message[128];
            snprintf(message, sizeof(message), "exec_argv: args[%zu] must not contain NUL bytes", i);
            process_set_error(err, message);
            return false;
        }
    }
    return true;
}

#ifndef __EMSCRIPTEN__

static void process_set_spawn_error(char **err, const char *program, const char *detail) {
    if (!err || *err) return;

    static const char prefix[] = "exec_argv: failed to spawn '";
    static const char separator[] = "': ";
    size_t program_len = strlen(program);
    size_t detail_len = strlen(detail);
    if (program_len > SIZE_MAX - sizeof(prefix) - sizeof(separator) - detail_len) {
        process_set_error(err, "exec_argv: failed to spawn program (error message too long)");
        return;
    }

    size_t message_len = (sizeof(prefix) - 1) + program_len + (sizeof(separator) - 1) + detail_len;
    char *message = malloc(message_len + 1);
    if (!message) {
        process_set_error(err, "exec_argv: out of memory while reporting spawn failure");
        return;
    }

    char *cursor = message;
    memcpy(cursor, prefix, sizeof(prefix) - 1);
    cursor += sizeof(prefix) - 1;
    memcpy(cursor, program, program_len);
    cursor += program_len;
    memcpy(cursor, separator, sizeof(separator) - 1);
    cursor += sizeof(separator) - 1;
    memcpy(cursor, detail, detail_len);
    cursor[detail_len] = '\0';
    *err = message;
}

#include "ds/hashmap.h"
#include <errno.h>
#include <pthread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include "win32_compat.h"
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
extern char **environ;
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} ProcessBuffer;

static bool process_buffer_init(ProcessBuffer *buffer) {
    buffer->cap = 1024;
    buffer->len = 0;
    buffer->oom = false;
    buffer->data = malloc(buffer->cap);
    if (!buffer->data) {
        buffer->cap = 0;
        buffer->oom = true;
        return false;
    }
    buffer->data[0] = '\0';
    return true;
}

/* Once growth fails, keep draining the pipe into the worker's stack buffer so
 * the child can still terminate; the caller reports OOM after all handles and
 * the child have been reaped. */
static void process_buffer_append(ProcessBuffer *buffer, const char *data, size_t len) {
    if (buffer->oom || len == 0) return;
    if (len > SIZE_MAX - buffer->len - 1) {
        buffer->oom = true;
        return;
    }
    size_t needed = buffer->len + len + 1;
    if (needed > buffer->cap) {
        size_t next = buffer->cap;
        while (next < needed) {
            if (next > SIZE_MAX / 2) {
                next = needed;
                break;
            }
            next *= 2;
        }
        char *grown = realloc(buffer->data, next);
        if (!grown) {
            buffer->oom = true;
            return;
        }
        buffer->data = grown;
        buffer->cap = next;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
}

static void process_buffer_free(ProcessBuffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = buffer->cap = 0;
}

static LatValue process_result_map(int64_t exit_code, ProcessBuffer *stdout_buffer, ProcessBuffer *stderr_buffer) {
    LatValue map = value_map_new();
    LatValue exit_code_value = value_int(exit_code);
    lat_map_set(map.as.map.map, "exit_code", &exit_code_value);

    LatValue stdout_value = value_string_owned_len(stdout_buffer->data, stdout_buffer->len);
    stdout_buffer->data = NULL;
    lat_map_set(map.as.map.map, "stdout", &stdout_value);

    LatValue stderr_value = value_string_owned_len(stderr_buffer->data, stderr_buffer->len);
    stderr_buffer->data = NULL;
    lat_map_set(map.as.map.map, "stderr", &stderr_value);
    return map;
}

#ifndef _WIN32
/* Drain two pipe fds CONCURRENTLY into heap-allocated, NUL-terminated strings.
 *
 * Reading one pipe fully to EOF before touching the other deadlocks when the
 * child fills the second pipe (the OS buffer is only ~64KB): the child blocks
 * writing while the parent blocks reading the first pipe. poll() lets us service
 * whichever fd has data ready, so neither side stalls.
 *
 * Each *out is set to a heap string ("" worth of bytes plus a terminator), or to
 * NULL on allocation failure for that stream (callers substitute ""). The caller
 * owns the fds and closes them. */
static void drain_two_fds(int fd_a, int fd_b, char **out_a, char **out_b) {
    struct fd_state {
        int fd;
        char *buf;
        size_t cap;
        size_t len;
        bool done;
    } st[2];
    int fds[2] = {fd_a, fd_b};

    for (int i = 0; i < 2; i++) {
        st[i].fd = fds[i];
        st[i].len = 0;
        st[i].cap = 1024;
        st[i].done = false;
        st[i].buf = malloc(st[i].cap);
        if (!st[i].buf) st[i].done = true; /* give up on this stream only */
    }

    while (!(st[0].done && st[1].done)) {
        struct pollfd pfds[2];
        int map[2];
        nfds_t n = 0;
        for (int i = 0; i < 2; i++) {
            if (st[i].done) continue;
            pfds[n].fd = st[i].fd;
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            map[n] = i;
            n++;
        }
        if (n == 0) break;

        int pr = poll(pfds, n, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break; /* unexpected poll failure: stop draining */
        }

        for (nfds_t j = 0; j < n; j++) {
            if (!(pfds[j].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            struct fd_state *s = &st[map[j]];
            ssize_t rd = read(s->fd, s->buf + s->len, s->cap - s->len);
            if (rd < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                s->done = true;
            } else if (rd == 0) {
                s->done = true; /* EOF */
            } else {
                s->len += (size_t)rd;
                if (s->len == s->cap) {
                    size_t ncap = s->cap * 2;
                    char *tmp = realloc(s->buf, ncap);
                    if (!tmp) {
                        free(s->buf);
                        s->buf = NULL;
                        s->done = true;
                    } else {
                        s->buf = tmp;
                        s->cap = ncap;
                    }
                }
            }
        }
    }

    for (int i = 0; i < 2; i++) {
        if (st[i].buf) st[i].buf[st[i].len] = '\0';
    }
    *out_a = st[0].buf;
    *out_b = st[1].buf;
}
#endif

LatValue process_exec(const char *cmd, char **err) {
#ifdef _WIN32
    FILE *fp = _popen(cmd, "r");
#else
    FILE *fp = popen(cmd, "r");
#endif
    if (!fp) {
        char msg[256];
        snprintf(msg, sizeof(msg), "exec: failed to run command: %s", strerror(errno));
        *err = strdup(msg);
        return value_unit();
    }

    size_t cap = 1024;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
#ifdef _WIN32
        _pclose(fp);
#else
        pclose(fp);
#endif
        *err = strdup("exec: out of memory");
        return value_unit();
    }

    for (;;) {
        size_t n = fread(buf + len, 1, cap - len, fp);
        if (n == 0) break;
        len += n;
        if (len == cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
#ifdef _WIN32
                _pclose(fp);
#else
                pclose(fp);
#endif
                *err = strdup("exec: out of memory");
                return value_unit();
            }
            buf = tmp;
        }
    }
    buf[len] = '\0';

#ifdef _WIN32
    int exit_code = _pclose(fp);
#else
    int status = pclose(fp);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    if (exit_code != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "exec: command failed with exit code %d", exit_code);
        free(buf);
        *err = strdup(msg);
        return value_unit();
    }

    return value_string_owned(buf);
}

LatValue process_shell(const char *cmd, char **err) {
#ifdef _WIN32
    /* Windows: use CreateProcess with pipe redirection */
    HANDLE stdout_rd = NULL, stdout_wr = NULL;
    HANDLE stderr_rd = NULL, stderr_wr = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0) || !CreatePipe(&stderr_rd, &stderr_wr, &sa, 0)) {
        *err = strdup("shell: failed to create pipes");
        return value_unit();
    }
    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = stdout_wr;
    si.hStdError = stderr_wr;
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));

    /* Build command line: cmd.exe /c <cmd> */
    size_t cmd_len = strlen(cmd) + 16;
    char *cmdline = malloc(cmd_len);
    snprintf(cmdline, cmd_len, "cmd.exe /c %s", cmd);

    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(cmdline);

    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    if (!ok) {
        CloseHandle(stdout_rd);
        CloseHandle(stderr_rd);
        *err = strdup("shell: CreateProcess failed");
        return value_unit();
    }

    /* Read stdout */
    size_t out_cap = 1024, out_len = 0;
    char *stdout_buf = malloc(out_cap);
    DWORD nr;
    while (ReadFile(stdout_rd, stdout_buf + out_len, (DWORD)(out_cap - out_len), &nr, NULL) && nr > 0) {
        out_len += nr;
        if (out_len == out_cap) {
            out_cap *= 2;
            stdout_buf = realloc(stdout_buf, out_cap);
        }
    }
    stdout_buf[out_len] = '\0';
    CloseHandle(stdout_rd);

    /* Read stderr */
    size_t err_cap = 1024, err_len = 0;
    char *stderr_buf = malloc(err_cap);
    while (ReadFile(stderr_rd, stderr_buf + err_len, (DWORD)(err_cap - err_len), &nr, NULL) && nr > 0) {
        err_len += nr;
        if (err_len == err_cap) {
            err_cap *= 2;
            stderr_buf = realloc(stderr_buf, err_cap);
        }
    }
    stderr_buf[err_len] = '\0';
    CloseHandle(stderr_rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code_dw;
    GetExitCodeProcess(pi.hProcess, &exit_code_dw);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    LatValue map = value_map_new();
    LatValue exit_code_val = value_int((int64_t)exit_code_dw);
    lat_map_set(map.as.map.map, "exit_code", &exit_code_val);
    LatValue stdout_val = value_string_owned(stdout_buf);
    lat_map_set(map.as.map.map, "stdout", &stdout_val);
    LatValue stderr_val = value_string_owned(stderr_buf);
    lat_map_set(map.as.map.map, "stderr", &stderr_val);
    return map;
#else
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (pipe(stdout_pipe) != 0) {
        *err = strdup("shell: failed to create stdout pipe");
        return value_unit();
    }
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        *err = strdup("shell: failed to create stderr pipe");
        return value_unit();
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        *err = strdup("shell: fork failed");
        return value_unit();
    }

    if (pid == 0) {
        /* Child process */
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent process */
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    /* Drain stdout and stderr concurrently to avoid a pipe-buffer deadlock when
     * the child fills one pipe while we are blocked reading the other. */
    char *stdout_buf = NULL;
    char *stderr_buf = NULL;
    drain_two_fds(stdout_pipe[0], stderr_pipe[0], &stdout_buf, &stderr_buf);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if (!stdout_buf) stdout_buf = strdup("");
    if (!stderr_buf) stderr_buf = strdup("");

    /* Build result Map.
     * lat_map_set does a shallow memcpy, so the map takes ownership
     * of the inner data (strings). Do NOT value_free the locals. */
    LatValue map = value_map_new();

    LatValue exit_code_val = value_int(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    lat_map_set(map.as.map.map, "exit_code", &exit_code_val);

    LatValue stdout_val = value_string_owned(stdout_buf);
    lat_map_set(map.as.map.map, "stdout", &stdout_val);

    LatValue stderr_val = value_string_owned(stderr_buf);
    lat_map_set(map.as.map.map, "stderr", &stderr_val);

    return map;
#endif
}

#ifndef _WIN32

typedef struct {
    int fd;
    ProcessBuffer *buffer;
    int io_error;
} PosixReadWorker;

typedef struct {
    int fd;
    const char *data;
    size_t len;
    int io_error;
} PosixWriteWorker;

static void *posix_read_worker(void *opaque) {
    PosixReadWorker *worker = opaque;
    char chunk[8192];
    for (;;) {
        ssize_t count = read(worker->fd, chunk, sizeof(chunk));
        if (count > 0) {
            process_buffer_append(worker->buffer, chunk, (size_t)count);
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        worker->io_error = errno;
        break;
    }
    close(worker->fd);
    worker->fd = -1;
    return NULL;
}

static void *posix_write_worker(void *opaque) {
    PosixWriteWorker *worker = opaque;

    /* A child is allowed to close stdin early. Block SIGPIPE in this worker so
     * that case becomes EPIPE instead of terminating the whole interpreter.
     * The mask is intentionally not restored: any thread-directed pending
     * SIGPIPE is discarded when this short-lived worker exits. */
    sigset_t sigpipe;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);
    int mask_error = pthread_sigmask(SIG_BLOCK, &sigpipe, NULL);
    if (mask_error != 0) {
        worker->io_error = mask_error;
        close(worker->fd);
        worker->fd = -1;
        return NULL;
    }

    size_t written = 0;
    while (written < worker->len) {
        size_t remaining = worker->len - written;
        size_t request = remaining > 65536 ? 65536 : remaining;
        ssize_t count = write(worker->fd, worker->data + written, request);
        if (count > 0) {
            written += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno == EPIPE) break; /* child deliberately stopped reading */
        worker->io_error = count < 0 ? errno : EIO;
        break;
    }
    close(worker->fd); /* exact EOF after the last byte (or after EPIPE) */
    worker->fd = -1;
    return NULL;
}

static void posix_close_pipe(int pipe_fds[2]) {
    for (int i = 0; i < 2; i++) {
        if (pipe_fds[i] >= 0) close(pipe_fds[i]);
        pipe_fds[i] = -1;
    }
}

/* Keep pipe descriptors away from 0/1/2 so spawn file-actions can always add
 * dup2 followed by close actions without accidentally closing a standard fd in
 * a parent process that entered with one of its standard streams closed. */
static int posix_move_above_stdio(int fd) {
    if (fd > STDERR_FILENO) return fd;
#ifdef F_DUPFD_CLOEXEC
    int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
    int moved = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
    if (moved >= 0 && fcntl(moved, F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(moved);
        errno = saved;
        moved = -1;
    }
#endif
    if (moved >= 0) close(fd);
    return moved;
}

static bool posix_make_pipe(int pipe_fds[2]) {
    pipe_fds[0] = pipe_fds[1] = -1;
    int raw[2];
    if (pipe(raw) != 0) return false;
    for (int i = 0; i < 2; i++) {
        int original = raw[i];
        int moved = posix_move_above_stdio(original);
        if (moved < 0) {
            int saved = errno;
            close(original);
            if (i == 0) close(raw[1]);
            else close(raw[0]);
            errno = saved;
            return false;
        }
        raw[i] = moved;
        int flags = fcntl(raw[i], F_GETFD);
        if (flags < 0 || fcntl(raw[i], F_SETFD, flags | FD_CLOEXEC) < 0) {
            int saved = errno;
            close(raw[i]);
            if (i == 0) close(raw[1]);
            else close(raw[0]);
            errno = saved;
            return false;
        }
    }
    pipe_fds[0] = raw[0];
    pipe_fds[1] = raw[1];
    return true;
}

static void posix_set_system_error(char **err, const char *operation, int error_code) {
    char message[512];
    snprintf(message, sizeof(message), "exec_argv: %s: %s", operation, strerror(error_code));
    process_set_error(err, message);
}

static LatValue process_exec_argv_posix(LatValue *args, char **err) {
    ProcessBuffer stdout_buffer = {0};
    ProcessBuffer stderr_buffer = {0};
    if (!process_buffer_init(&stdout_buffer) || !process_buffer_init(&stderr_buffer)) {
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        process_set_error(err, "exec_argv: out of memory");
        return value_unit();
    }

    size_t supplied_argc = args[1].as.array.len;
    if (supplied_argc > SIZE_MAX / sizeof(char *) - 2) {
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        process_set_error(err, "exec_argv: too many arguments");
        return value_unit();
    }
    char **argv = malloc((supplied_argc + 2) * sizeof(char *));
    if (!argv) {
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        process_set_error(err, "exec_argv: out of memory");
        return value_unit();
    }
    argv[0] = args[0].as.str_val;
    for (size_t i = 0; i < supplied_argc; i++) argv[i + 1] = args[1].as.array.elems[i].as.str_val;
    argv[supplied_argc + 1] = NULL;

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (!posix_make_pipe(stdin_pipe) || !posix_make_pipe(stdout_pipe) || !posix_make_pipe(stderr_pipe)) {
        int saved = errno;
        posix_close_pipe(stdin_pipe);
        posix_close_pipe(stdout_pipe);
        posix_close_pipe(stderr_pipe);
        free(argv);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        posix_set_system_error(err, "failed to create pipes", saved);
        return value_unit();
    }

    posix_spawn_file_actions_t actions;
    int action_error = posix_spawn_file_actions_init(&actions);
    bool actions_initialized = action_error == 0;
    if (action_error == 0) action_error = posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
    if (action_error == 0) action_error = posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    if (action_error == 0) action_error = posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
    int all_fds[] = {stdin_pipe[0], stdin_pipe[1], stdout_pipe[0], stdout_pipe[1], stderr_pipe[0], stderr_pipe[1]};
    for (size_t i = 0; action_error == 0 && i < sizeof(all_fds) / sizeof(all_fds[0]); i++)
        action_error = posix_spawn_file_actions_addclose(&actions, all_fds[i]);

    if (action_error != 0) {
        if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
        posix_close_pipe(stdin_pipe);
        posix_close_pipe(stdout_pipe);
        posix_close_pipe(stderr_pipe);
        free(argv);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        posix_set_system_error(err, "failed to configure child process", action_error);
        return value_unit();
    }

    pid_t child = -1;
    int spawn_error = posix_spawnp(&child, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    free(argv);

    if (spawn_error != 0) {
        posix_close_pipe(stdin_pipe);
        posix_close_pipe(stdout_pipe);
        posix_close_pipe(stderr_pipe);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        process_set_spawn_error(err, args[0].as.str_val, strerror(spawn_error));
        return value_unit();
    }

    /* Parent owns the opposite ends only. */
    close(stdin_pipe[0]);
    stdin_pipe[0] = -1;
    close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    close(stderr_pipe[1]);
    stderr_pipe[1] = -1;

    PosixReadWorker stdout_worker = {.fd = stdout_pipe[0], .buffer = &stdout_buffer, .io_error = 0};
    PosixReadWorker stderr_worker = {.fd = stderr_pipe[0], .buffer = &stderr_buffer, .io_error = 0};
    pthread_t stdout_thread;
    pthread_t stderr_thread;
    pthread_t stdin_thread;
    bool stdout_started = false;
    bool stderr_started = false;
    bool stdin_started = false;
    int worker_error = pthread_create(&stdout_thread, NULL, posix_read_worker, &stdout_worker);
    if (worker_error == 0) {
        stdout_started = true;
        stdout_pipe[0] = -1;
    } else {
        close(stdout_pipe[0]);
        stdout_pipe[0] = -1;
    }
    int create_error = pthread_create(&stderr_thread, NULL, posix_read_worker, &stderr_worker);
    if (create_error == 0) {
        stderr_started = true;
        stderr_pipe[0] = -1;
    } else {
        if (worker_error == 0) worker_error = create_error;
        close(stderr_pipe[0]);
        stderr_pipe[0] = -1;
    }

    const char *stdin_data = args[2].type == VAL_STR ? args[2].as.str_val : NULL;
    size_t stdin_len = args[2].type == VAL_STR ? process_string_len(&args[2]) : 0;
    PosixWriteWorker stdin_worker = {.fd = stdin_pipe[1], .data = stdin_data, .len = stdin_len, .io_error = 0};
    if (stdin_len == 0) {
        close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
    } else {
        create_error = pthread_create(&stdin_thread, NULL, posix_write_worker, &stdin_worker);
        if (create_error == 0) {
            stdin_started = true;
            stdin_pipe[1] = -1;
        } else {
            if (worker_error == 0) worker_error = create_error;
            close(stdin_pipe[1]);
            stdin_pipe[1] = -1;
        }
    }

    if (worker_error != 0) kill(child, SIGKILL);

    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    int wait_error = waited < 0 ? errno : 0;

    int join_error = 0;
    if (stdout_started) {
        int joined = pthread_join(stdout_thread, NULL);
        if (joined != 0 && join_error == 0) join_error = joined;
    }
    if (stderr_started) {
        int joined = pthread_join(stderr_thread, NULL);
        if (joined != 0 && join_error == 0) join_error = joined;
    }
    if (stdin_started) {
        int joined = pthread_join(stdin_thread, NULL);
        if (joined != 0 && join_error == 0) join_error = joined;
    }

    if (worker_error != 0) posix_set_system_error(err, "failed to start I/O worker", worker_error);
    else if (wait_error != 0) posix_set_system_error(err, "failed to wait for child", wait_error);
    else if (join_error != 0) posix_set_system_error(err, "failed to join I/O worker", join_error);
    else if (stdout_worker.io_error != 0)
        posix_set_system_error(err, "failed to read child stdout", stdout_worker.io_error);
    else if (stderr_worker.io_error != 0)
        posix_set_system_error(err, "failed to read child stderr", stderr_worker.io_error);
    else if (stdin_worker.io_error != 0)
        posix_set_system_error(err, "failed to write child stdin", stdin_worker.io_error);
    else if (stdout_buffer.oom || stderr_buffer.oom)
        process_set_error(err, "exec_argv: out of memory while capturing output");

    if (err && *err) {
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        return value_unit();
    }

    int64_t exit_code = WIFEXITED(status) ? (int64_t)WEXITSTATUS(status) : -1;
    return process_result_map(exit_code, &stdout_buffer, &stderr_buffer);
}

#endif /* !_WIN32 */

#ifdef _WIN32

typedef struct {
    HANDLE handle;
    ProcessBuffer *buffer;
    DWORD io_error;
} Win32ReadWorker;

typedef struct {
    HANDLE handle;
    const char *data;
    size_t len;
    DWORD io_error;
} Win32WriteWorker;

static void win32_close_handle(HANDLE *handle) {
    if (*handle && *handle != INVALID_HANDLE_VALUE) CloseHandle(*handle);
    *handle = NULL;
}

static void win32_set_system_error(char **err, const char *operation, DWORD error_code) {
    char message[256];
    snprintf(message, sizeof(message), "exec_argv: %s (Windows error %lu)", operation, (unsigned long)error_code);
    process_set_error(err, message);
}

static void *win32_read_worker(void *opaque) {
    Win32ReadWorker *worker = opaque;
    char chunk[8192];
    for (;;) {
        DWORD count = 0;
        BOOL ok = ReadFile(worker->handle, chunk, (DWORD)sizeof(chunk), &count, NULL);
        if (ok && count > 0) {
            process_buffer_append(worker->buffer, chunk, (size_t)count);
            continue;
        }
        if (ok) break;
        DWORD code = GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA) break;
        worker->io_error = code;
        break;
    }
    win32_close_handle(&worker->handle);
    return NULL;
}

static void *win32_write_worker(void *opaque) {
    Win32WriteWorker *worker = opaque;
    size_t written = 0;
    while (written < worker->len) {
        size_t remaining = worker->len - written;
        DWORD request = remaining > 65536 ? 65536 : (DWORD)remaining;
        DWORD count = 0;
        BOOL ok = WriteFile(worker->handle, worker->data + written, request, &count, NULL);
        if (ok && count > 0) {
            written += (size_t)count;
            continue;
        }
        if (!ok) {
            DWORD code = GetLastError();
            if (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA) break;
            worker->io_error = code;
        } else {
            worker->io_error = ERROR_WRITE_FAULT;
        }
        break;
    }
    win32_close_handle(&worker->handle);
    return NULL;
}

static void win32_append_repeated(ProcessBuffer *buffer, char byte, size_t count) {
    char bytes[64];
    memset(bytes, byte, sizeof(bytes));
    while (count > 0 && !buffer->oom) {
        size_t part = count < sizeof(bytes) ? count : sizeof(bytes);
        process_buffer_append(buffer, bytes, part);
        count -= part;
    }
}

/* Quote one argument according to the Microsoft C runtime argv grammar. Every
 * argument is quoted (including the empty string); runs of backslashes before a
 * quote and before the closing quote are doubled exactly as required. */
static void win32_append_quoted_arg(ProcessBuffer *command, const char *arg) {
    process_buffer_append(command, "\"", 1);
    size_t backslashes = 0;
    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            backslashes++;
            continue;
        }
        if (*p == '"') {
            if (backslashes > (SIZE_MAX - 1) / 2) {
                command->oom = true;
                return;
            }
            win32_append_repeated(command, '\\', backslashes * 2 + 1);
            process_buffer_append(command, "\"", 1);
            backslashes = 0;
            continue;
        }
        win32_append_repeated(command, '\\', backslashes);
        backslashes = 0;
        process_buffer_append(command, p, 1);
    }
    if (backslashes > SIZE_MAX / 2) {
        command->oom = true;
        return;
    }
    win32_append_repeated(command, '\\', backslashes * 2);
    process_buffer_append(command, "\"", 1);
}

static wchar_t *win32_utf8_to_wide(const char *utf8, char **err) {
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (needed <= 0) {
        win32_set_system_error(err, "arguments are not valid UTF-8", GetLastError());
        return NULL;
    }
    if (needed > 32767) {
        process_set_error(err, "exec_argv: command line exceeds the Windows 32767-character limit");
        return NULL;
    }
    wchar_t *wide = malloc((size_t)needed * sizeof(wchar_t));
    if (!wide) {
        process_set_error(err, "exec_argv: out of memory");
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, needed) == 0) {
        DWORD code = GetLastError();
        free(wide);
        win32_set_system_error(err, "failed to convert arguments to UTF-16", code);
        return NULL;
    }
    return wide;
}

static LatValue process_exec_argv_windows(LatValue *args, char **err) {
    ProcessBuffer command = {0};
    ProcessBuffer stdout_buffer = {0};
    ProcessBuffer stderr_buffer = {0};
    if (!process_buffer_init(&command) || !process_buffer_init(&stdout_buffer) ||
        !process_buffer_init(&stderr_buffer)) {
        process_buffer_free(&command);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        process_set_error(err, "exec_argv: out of memory");
        return value_unit();
    }

    win32_append_quoted_arg(&command, args[0].as.str_val);
    for (size_t i = 0; i < args[1].as.array.len && !command.oom; i++) {
        process_buffer_append(&command, " ", 1);
        win32_append_quoted_arg(&command, args[1].as.array.elems[i].as.str_val);
    }
    if (command.oom) {
        process_buffer_free(&command);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        process_set_error(err, "exec_argv: out of memory while building command line");
        return value_unit();
    }
    wchar_t *command_line = win32_utf8_to_wide(command.data, err);
    process_buffer_free(&command);
    if (!command_line) {
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        return value_unit();
    }

    SECURITY_ATTRIBUTES security = {.nLength = sizeof(security), .lpSecurityDescriptor = NULL, .bInheritHandle = TRUE};
    HANDLE stdin_read = NULL;
    HANDLE stdin_write = NULL;
    HANDLE stdout_read = NULL;
    HANDLE stdout_write = NULL;
    HANDLE stderr_read = NULL;
    HANDLE stderr_write = NULL;
    if (!CreatePipe(&stdin_read, &stdin_write, &security, 0) ||
        !CreatePipe(&stdout_read, &stdout_write, &security, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &security, 0)) {
        DWORD code = GetLastError();
        win32_close_handle(&stdin_read);
        win32_close_handle(&stdin_write);
        win32_close_handle(&stdout_read);
        win32_close_handle(&stdout_write);
        win32_close_handle(&stderr_read);
        win32_close_handle(&stderr_write);
        free(command_line);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        win32_set_system_error(err, "failed to create pipes", code);
        return value_unit();
    }

    if (!SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        DWORD code = GetLastError();
        win32_close_handle(&stdin_read);
        win32_close_handle(&stdin_write);
        win32_close_handle(&stdout_read);
        win32_close_handle(&stdout_write);
        win32_close_handle(&stderr_read);
        win32_close_handle(&stderr_write);
        free(command_line);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        win32_set_system_error(err, "failed to configure pipe inheritance", code);
        return value_unit();
    }

    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_read;
    startup.StartupInfo.hStdOutput = stdout_write;
    startup.StartupInfo.hStdError = stderr_write;

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    startup.lpAttributeList = attribute_size ? malloc(attribute_size) : NULL;
    if (!startup.lpAttributeList ||
        !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) {
        DWORD code = startup.lpAttributeList ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        free(startup.lpAttributeList);
        win32_close_handle(&stdin_read);
        win32_close_handle(&stdin_write);
        win32_close_handle(&stdout_read);
        win32_close_handle(&stdout_write);
        win32_close_handle(&stderr_read);
        win32_close_handle(&stderr_write);
        free(command_line);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        win32_set_system_error(err, "failed to allocate process attribute list", code);
        return value_unit();
    }

    HANDLE inherited_handles[] = {stdin_read, stdout_write, stderr_write};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles,
                                   sizeof(inherited_handles), NULL, NULL)) {
        DWORD code = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        free(startup.lpAttributeList);
        win32_close_handle(&stdin_read);
        win32_close_handle(&stdin_write);
        win32_close_handle(&stdout_read);
        win32_close_handle(&stdout_write);
        win32_close_handle(&stderr_read);
        win32_close_handle(&stderr_write);
        free(command_line);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        win32_set_system_error(err, "failed to restrict inherited handles", code);
        return value_unit();
    }

    BOOL spawned = CreateProcessW(NULL, command_line, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                                  &startup.StartupInfo, &process);
    DWORD spawn_error = spawned ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList);
    free(command_line);

    /* Never keep the child's copies open in the parent: reader EOF and stdin
     * EOF both depend on these closes. */
    win32_close_handle(&stdin_read);
    win32_close_handle(&stdout_write);
    win32_close_handle(&stderr_write);

    if (!spawned) {
        win32_close_handle(&stdin_write);
        win32_close_handle(&stdout_read);
        win32_close_handle(&stderr_read);
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        char detail[64];
        snprintf(detail, sizeof(detail), "Windows error %lu", (unsigned long)spawn_error);
        process_set_spawn_error(err, args[0].as.str_val, detail);
        return value_unit();
    }

    Win32ReadWorker stdout_worker = {.handle = stdout_read, .buffer = &stdout_buffer, .io_error = 0};
    Win32ReadWorker stderr_worker = {.handle = stderr_read, .buffer = &stderr_buffer, .io_error = 0};
    pthread_t stdout_thread;
    pthread_t stderr_thread;
    pthread_t stdin_thread;
    bool stdout_started = false;
    bool stderr_started = false;
    bool stdin_started = false;
    int worker_error = pthread_create(&stdout_thread, NULL, win32_read_worker, &stdout_worker);
    if (worker_error == 0) {
        stdout_started = true;
        stdout_read = NULL;
    } else {
        win32_close_handle(&stdout_read);
    }
    int create_error = pthread_create(&stderr_thread, NULL, win32_read_worker, &stderr_worker);
    if (create_error == 0) {
        stderr_started = true;
        stderr_read = NULL;
    } else {
        if (worker_error == 0) worker_error = create_error;
        win32_close_handle(&stderr_read);
    }

    const char *stdin_data = args[2].type == VAL_STR ? args[2].as.str_val : NULL;
    size_t stdin_len = args[2].type == VAL_STR ? process_string_len(&args[2]) : 0;
    Win32WriteWorker stdin_worker = {.handle = stdin_write, .data = stdin_data, .len = stdin_len, .io_error = 0};
    if (stdin_len == 0) {
        win32_close_handle(&stdin_write);
    } else {
        create_error = pthread_create(&stdin_thread, NULL, win32_write_worker, &stdin_worker);
        if (create_error == 0) {
            stdin_started = true;
            stdin_write = NULL;
        } else {
            if (worker_error == 0) worker_error = create_error;
            win32_close_handle(&stdin_write);
        }
    }

    if (worker_error != 0) TerminateProcess(process.hProcess, 127);
    DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD wait_error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    if (wait_result == WAIT_FAILED) {
        TerminateProcess(process.hProcess, 127);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exit_code = 0;
    DWORD exit_error = ERROR_SUCCESS;
    if (wait_result != WAIT_FAILED && !GetExitCodeProcess(process.hProcess, &exit_code)) exit_error = GetLastError();

    int join_error = 0;
    if (stdout_started) {
        int joined = pthread_join(stdout_thread, NULL);
        if (joined != 0 && join_error == 0) join_error = joined;
    }
    if (stderr_started) {
        int joined = pthread_join(stderr_thread, NULL);
        if (joined != 0 && join_error == 0) join_error = joined;
    }
    if (stdin_started) {
        int joined = pthread_join(stdin_thread, NULL);
        if (joined != 0 && join_error == 0) join_error = joined;
    }
    win32_close_handle(&process.hThread);
    win32_close_handle(&process.hProcess);

    if (worker_error != 0) {
        char message[256];
        snprintf(message, sizeof(message), "exec_argv: failed to start I/O worker: %s", strerror(worker_error));
        process_set_error(err, message);
    } else if (wait_error != ERROR_SUCCESS) win32_set_system_error(err, "failed to wait for child", wait_error);
    else if (exit_error != ERROR_SUCCESS) win32_set_system_error(err, "failed to obtain child exit code", exit_error);
    else if (join_error != 0) {
        char message[256];
        snprintf(message, sizeof(message), "exec_argv: failed to join I/O worker: %s", strerror(join_error));
        process_set_error(err, message);
    } else if (stdout_worker.io_error != ERROR_SUCCESS)
        win32_set_system_error(err, "failed to read child stdout", stdout_worker.io_error);
    else if (stderr_worker.io_error != ERROR_SUCCESS)
        win32_set_system_error(err, "failed to read child stderr", stderr_worker.io_error);
    else if (stdin_worker.io_error != ERROR_SUCCESS)
        win32_set_system_error(err, "failed to write child stdin", stdin_worker.io_error);
    else if (stdout_buffer.oom || stderr_buffer.oom)
        process_set_error(err, "exec_argv: out of memory while capturing output");

    if (err && *err) {
        process_buffer_free(&stdout_buffer);
        process_buffer_free(&stderr_buffer);
        return value_unit();
    }
    return process_result_map((int64_t)exit_code, &stdout_buffer, &stderr_buffer);
}

#endif /* _WIN32 */

LatValue process_exec_argv(LatValue *args, int arg_count, char **err) {
    if (err) *err = NULL;
    if (!process_exec_argv_validate(args, arg_count, err)) return value_unit();
#ifdef _WIN32
    return process_exec_argv_windows(args, err);
#else
    return process_exec_argv_posix(args, err);
#endif
}

char *process_cwd(char **err) {
#ifdef _WIN32
    char *buf = _getcwd(NULL, 0);
#else
    char *buf = getcwd(NULL, 0);
#endif
    if (!buf) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cwd: %s", strerror(errno));
        *err = strdup(msg);
        return NULL;
    }
    return buf;
}

char *process_hostname(char **err) {
    char buf[256];
#ifdef _WIN32
    win32_net_init();
#endif
    if (gethostname(buf, sizeof(buf)) != 0) {
        *err = strdup("hostname: failed to get hostname");
        return NULL;
    }
    return strdup(buf);
}

int process_pid(void) {
#ifdef _WIN32
    return (int)_getpid();
#else
    return (int)getpid();
#endif
}

#else /* __EMSCRIPTEN__ */

LatValue process_exec_argv(LatValue *args, int arg_count, char **err) {
    if (err) *err = NULL;
    if (!process_exec_argv_validate(args, arg_count, err)) return value_unit();
    process_set_error(err, "exec_argv: not available in browser");
    return value_unit();
}

LatValue process_exec(const char *cmd, char **err) {
    (void)cmd;
    *err = strdup("exec: not available in browser");
    return value_unit();
}

LatValue process_shell(const char *cmd, char **err) {
    (void)cmd;
    *err = strdup("shell: not available in browser");
    return value_unit();
}

char *process_cwd(char **err) {
    *err = strdup("cwd: not available in browser");
    return NULL;
}

char *process_hostname(char **err) {
    *err = strdup("hostname: not available in browser");
    return NULL;
}

int process_pid(void) { return 0; }

#endif /* __EMSCRIPTEN__ */

/* process_platform works on all targets via preprocessor */
const char *process_platform(void) {
#if defined(__EMSCRIPTEN__)
    return "wasm";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#else
    return "unknown";
#endif
}
