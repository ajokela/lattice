#include "process_ops.h"
#include "ds/hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (pipe(stdout_pipe) != 0) {
        *err = strdup("exec: failed to create stdout pipe");
        return value_unit();
    }
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        *err = strdup("exec: failed to create stderr pipe");
        return value_unit();
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        *err = strdup("exec: fork failed");
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

    /* Build result Map */
    LatValue map = value_map_new();

    LatValue status_val = value_int(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    lat_map_set(map.as.map.map, "status", &status_val);
    value_free(&status_val);

    LatValue stdout_val = value_string_owned(stdout_buf);
    lat_map_set(map.as.map.map, "stdout", &stdout_val);
    value_free(&stdout_val);

    LatValue stderr_val = value_string_owned(stderr_buf);
    lat_map_set(map.as.map.map, "stderr", &stderr_val);
    value_free(&stderr_val);

    return map;
}
