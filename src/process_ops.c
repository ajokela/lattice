#include "process_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef __EMSCRIPTEN__

#include "ds/hashmap.h"
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

/* Read all data from a file descriptor into a heap-allocated string.
 * Returns NULL on allocation failure. */
static char *read_all_fd(int fd) {
    size_t cap = 1024;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    for (;;) {
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        len += (size_t)n;
        if (len == cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
    }
    buf[len] = '\0';
    return buf;
}

LatValue process_exec(const char *cmd, char **err) {
    FILE *fp = popen(cmd, "r");
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
        pclose(fp);
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
            if (!tmp) { free(buf); pclose(fp); *err = strdup("exec: out of memory"); return value_unit(); }
            buf = tmp;
        }
    }
    buf[len] = '\0';

    int status = pclose(fp);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

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
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
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

    char *stdout_buf = read_all_fd(stdout_pipe[0]);
    char *stderr_buf = read_all_fd(stderr_pipe[0]);
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
}

char *process_cwd(char **err) {
    char *buf = getcwd(NULL, 0);
    if (!buf) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cwd: %s", strerror(errno));
        *err = strdup(msg);
        return NULL;
    }
    return buf;
}

#else /* __EMSCRIPTEN__ */

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

#endif /* __EMSCRIPTEN__ */
