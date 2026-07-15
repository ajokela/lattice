#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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

    fputs("unknown mode\n", stderr);
    return 64;
}
