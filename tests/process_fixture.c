#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

static int sleep_millis(uint64_t milliseconds) {
#ifdef _WIN32
    while (milliseconds > 0) {
        DWORD part = milliseconds > 60000 ? 60000 : (DWORD)milliseconds;
        Sleep(part);
        milliseconds -= part;
    }
    return 0;
#else
    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) return 74;
    }
    return 0;
#endif
}

static int parse_u64(const char *text, uint64_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return 64;
    *out = (uint64_t)parsed;
    return 0;
}

static void print_hex(const unsigned char *data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = data[i];
        fputc(digits[byte >> 4], stdout);
        fputc(digits[byte & 0x0f], stdout);
    }
}

static int emit_argv(int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        size_t len = strlen(argv[i]);
        printf("%d:%zu:", i - 2, len);
        print_hex((const unsigned char *)argv[i], len);
        fputc('\n', stdout);
    }
    return ferror(stdout) ? 74 : 0;
}

static int echo_stdin_hex(void) {
    size_t len = 0;
    size_t cap = 4096;
    unsigned char *data = malloc(cap);
    if (!data) return 71;

    for (;;) {
        if (len == cap) {
            if (cap > SIZE_MAX / 2) {
                free(data);
                return 71;
            }
            cap *= 2;
            unsigned char *next = realloc(data, cap);
            if (!next) {
                free(data);
                return 71;
            }
            data = next;
        }
        size_t count = fread(data + len, 1, cap - len, stdin);
        len += count;
        if (count == 0) break;
    }

    if (ferror(stdin)) {
        free(data);
        return 74;
    }
    printf("%zu:", len);
    print_hex(data, len);
    fputc('\n', stdout);
    free(data);
    return ferror(stdout) ? 74 : 0;
}

static int emit_repeated(FILE *stream, unsigned char byte, size_t total) {
    unsigned char block[4096];
    memset(block, byte, sizeof(block));
    while (total > 0) {
        size_t amount = total < sizeof(block) ? total : sizeof(block);
        if (fwrite(block, 1, amount, stream) != amount) return 74;
        total -= amount;
    }
    return fflush(stream) == 0 ? 0 : 74;
}

static int pressure(void) {
    const size_t stream_size = 256 * 1024;
    if (emit_repeated(stdout, 'O', stream_size) != 0) return 74;
    if (emit_repeated(stderr, 'E', stream_size) != 0) return 74;

    size_t stdin_len = 0;
    unsigned char block[4096];
    for (;;) {
        size_t count = fread(block, 1, sizeof(block), stdin);
        stdin_len += count;
        if (count == 0) break;
    }
    if (ferror(stdin)) return 74;
    printf("stdin=%zu\n", stdin_len);
    return ferror(stdout) ? 74 : 0;
}

static int pressure_interleaved(void) {
    for (size_t i = 0; i < 64; i++) {
        if (emit_repeated(stdout, 'O', 4096) != 0) return 74;
        if (emit_repeated(stderr, 'E', 4096) != 0) return 74;
    }

    unsigned char block[4096];
    while (fread(block, 1, sizeof(block), stdin) > 0) {}
    return ferror(stdin) ? 74 : 0;
}

static int write_marker(const char *path, const char *contents) {
    FILE *file = fopen(path, "wb");
    if (!file) return 74;
    bool write_failed = fputs(contents, file) == EOF;
    bool close_failed = fclose(file) != 0;
    return write_failed || close_failed ? 74 : 0;
}

static int spawn_descendant(const char *program, const char *pid_path, const char *marker_path, bool hold_parent) {
#ifdef _WIN32
    intptr_t raw = marker_path ? _spawnl(_P_NOWAIT, program, program, "descendant-worker", marker_path, NULL)
                               : _spawnl(_P_NOWAIT, program, program, "descendant-worker", NULL);
    if (raw == -1) return 71;
    HANDLE child = (HANDLE)raw;
    DWORD pid = GetProcessId(child);
    CloseHandle(child);
    if (pid == 0) return 71;
#else
    pid_t pid = fork();
    if (pid < 0) return 71;
    if (pid == 0) {
        if (marker_path) execl(program, program, "descendant-worker", marker_path, (char *)NULL);
        else execl(program, program, "descendant-worker", (char *)NULL);
        _exit(127);
    }
#endif
    FILE *file = fopen(pid_path, "wb");
    if (!file) return 74;
    bool write_failed = fprintf(file, "%lu\n", (unsigned long)pid) < 0;
    bool close_failed = fclose(file) != 0;
    if (write_failed || close_failed) return 74;
    return hold_parent ? sleep_millis(2000) : 0;
}

static int spawn_detached_marker(const char *program, const char *marker_path) {
#ifdef _WIN32
    intptr_t raw = _spawnl(_P_NOWAIT, program, program, "detached-marker-worker", marker_path, NULL);
    if (raw == -1) return 71;
    CloseHandle((HANDLE)raw);
#else
    pid_t pid = fork();
    if (pid < 0) return 71;
    if (pid == 0) {
        execl(program, program, "detached-marker-worker", marker_path, (char *)NULL);
        _exit(127);
    }
#endif
    return 0;
}

static int detached_marker_worker(const char *marker_path) {
#ifdef _WIN32
    _close(0);
    _close(1);
    _close(2);
#else
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
#endif
    if (sleep_millis(100) != 0) return 74;
    return write_marker(marker_path, "alive\n");
}

static int pid_is_alive(const char *text) {
    uint64_t parsed = 0;
    if (parse_u64(text, &parsed) != 0 || parsed == 0) return 64;
#ifdef _WIN32
    if (parsed > UINT32_MAX) return 64;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)parsed);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? 1 : 74;
    DWORD state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state == WAIT_TIMEOUT ? 0 : 1;
#else
    if (parsed > (uint64_t)INT32_MAX) return 64;
    if (kill((pid_t)parsed, 0) == 0 || errno == EPERM) {
#ifdef __linux__
        /* An orphan killed with its process group may briefly remain as a
         * zombie under a container PID 1. It is no longer running, even
         * though kill(pid, 0) still finds its process-table entry. */
        char path[64];
        snprintf(path, sizeof(path), "/proc/%llu/stat", (unsigned long long)parsed);
        FILE *stat_file = fopen(path, "rb");
        if (stat_file) {
            char stat_line[512];
            bool read_stat = fgets(stat_line, sizeof(stat_line), stat_file) != NULL;
            char *name_end = read_stat ? strrchr(stat_line, ')') : NULL;
            bool zombie = name_end && name_end[1] == ' ' && name_end[2] == 'Z';
            fclose(stat_file);
            if (zombie) return 1;
        }
#endif
        return 0;
    }
    return errno == ESRCH ? 1 : 74;
#endif
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Keep the byte-oriented fixture identical on Windows: the CRT otherwise
     * translates newlines on redirected standard streams. */
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1 ||
        _setmode(_fileno(stderr), _O_BINARY) == -1)
        return 74;
#endif
    if (argc < 2) {
        fputs("missing mode\n", stderr);
        return 64;
    }
    if (strcmp(argv[1], "argv") == 0) return emit_argv(argc, argv);
    if (strcmp(argv[1], "stdin") == 0) return echo_stdin_hex();
    if (strcmp(argv[1], "streams") == 0) {
        fputs("stdout\n", stdout);
        fputs("stderr\n", stderr);
        if (argc < 3) return 0;
        char *end = NULL;
        errno = 0;
        long code = strtol(argv[2], &end, 10);
        if (errno != 0 || !end || *end != '\0' || code < 0 || code > 255) return 64;
        return (int)code;
    }
    if (strcmp(argv[1], "binary") == 0) {
        static const unsigned char bytes[] = {'A', 0, 'B'};
        return fwrite(bytes, 1, sizeof(bytes), stdout) == sizeof(bytes) ? 0 : 74;
    }
    if (strcmp(argv[1], "pressure") == 0) return pressure();
    if (strcmp(argv[1], "pressure-interleaved") == 0) return pressure_interleaved();
    if (strcmp(argv[1], "sleep") == 0) {
        uint64_t milliseconds = 0;
        if (argc != 3 || parse_u64(argv[2], &milliseconds) != 0) return 64;
        return sleep_millis(milliseconds);
    }
    if (strcmp(argv[1], "flood") == 0) {
        uint64_t count = 0;
        if (argc != 4 || parse_u64(argv[3], &count) != 0 || count > SIZE_MAX) return 64;
        FILE *stream = strcmp(argv[2], "stdout") == 0 ? stdout : strcmp(argv[2], "stderr") == 0 ? stderr : NULL;
        if (!stream) return 64;
        return emit_repeated(stream, stream == stdout ? 'O' : 'E', (size_t)count);
    }
    if (strcmp(argv[1], "descendant") == 0) {
        if (argc != 3) return 64;
        return spawn_descendant(argv[0], argv[2], NULL, true);
    }
    if (strcmp(argv[1], "descendant-exit") == 0) {
        if (argc != 4) return 64;
        return spawn_descendant(argv[0], argv[2], argv[3], false);
    }
    if (strcmp(argv[1], "descendant-worker") == 0) {
        if (argc == 2) return sleep_millis(2000);
        if (argc == 3) {
            if (sleep_millis(250) != 0) return 74;
            return write_marker(argv[2], "survived\n");
        }
        return 64;
    }
    if (strcmp(argv[1], "detached-marker") == 0) {
        if (argc != 3) return 64;
        return spawn_detached_marker(argv[0], argv[2]);
    }
    if (strcmp(argv[1], "detached-marker-worker") == 0) {
        if (argc != 3) return 64;
        return detached_marker_worker(argv[2]);
    }
    if (strcmp(argv[1], "pid-alive") == 0) {
        if (argc != 3) return 64;
        return pid_is_alive(argv[2]);
    }

    fputs("unknown mode\n", stderr);
    return 64;
}
