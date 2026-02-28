#include "process_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef __EMSCRIPTEN__

#include "ds/hashmap.h"
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include "win32_compat.h"
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifndef _WIN32
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
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
        }
    }
    buf[len] = '\0';
    return buf;
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
