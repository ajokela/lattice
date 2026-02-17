#include "lsp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <limits.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* Find eval.c relative to the binary for builtin extraction */
static char *find_eval_path(void) {
    char exe_path[PATH_MAX] = {0};

#ifdef __APPLE__
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0)
        return NULL;
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) return NULL;
    exe_path[len] = '\0';
#else
    return NULL;
#endif

    /* exe is in project root, eval.c is in src/ */
    char *dir = dirname(exe_path);
    char *path = malloc(strlen(dir) + 32);
    sprintf(path, "%s/src/eval.c", dir);

    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return path;
    }
    free(path);
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Disable buffering on stdout for LSP */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);

    LspServer *srv = lsp_server_new();

    /* Load builtin symbol index */
    char *eval_path = find_eval_path();
    if (eval_path) {
        srv->index = lsp_symbol_index_new(eval_path);
        free(eval_path);
    }

    lsp_server_run(srv);
    lsp_server_free(srv);
    return 0;
}
