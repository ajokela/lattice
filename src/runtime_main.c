/* runtime_main.c — Minimal entry point for the Lattice bytecode runtime.
 *
 * This is the main() for the `clat-run` binary, which can only execute
 * pre-compiled bytecode files: .latc (stack VM) or .rlat (register VM).
 * No REPL, no source compilation, no LSP, no debugger, no formatter.
 *
 * Usage: clat-run <file.latc|file.rlat> [args...]
 */

#include "lattice.h"
#include "stackvm.h"
#include "regvm.h"
#include "latc.h"
#include "runtime.h"
#include "value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool has_suffix(const char *str, const char *suffix) {
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    return slen >= xlen && strcmp(str + slen - xlen, suffix) == 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Lattice bytecode runtime v%s\n", LATTICE_VERSION);
        fprintf(stderr, "Usage: clat-run <file.latc|file.rlat> [args...]\n");
        return 1;
    }

    const char *path = argv[1];

    /* Validate file extension */
    if (!has_suffix(path, ".latc") && !has_suffix(path, ".rlat") && !has_suffix(path, ".rlatc")) {
        fprintf(stderr, "error: expected a .latc or .rlat bytecode file, got '%s'\n", path);
        return 1;
    }

    /* Sniff the magic to pick the VM (same auto-detection as the full clat
     * binary): "RLAT" = register VM, "LATC" = stack VM */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 1;
    }
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4) {
        fprintf(stderr, "error: '%s' is too small for a bytecode header\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);

    if (memcmp(magic, RLATC_MAGIC, 4) == 0) {
        /* ── Register VM bytecode ── */
        char *err = NULL;
        RegChunk *rchunk = regchunk_load(path, &err);
        if (!rchunk) {
            fprintf(stderr, "error: %s\n", err);
            free(err);
            return 1;
        }

        /* Disconnect the fluid heap — the VM uses plain malloc/free */
        value_set_heap(NULL);
        value_set_arena(NULL);

        LatRuntime rt;
        lat_runtime_init(&rt);
        rt.prog_argc = argc - 1; /* argv[1..] from the script's perspective */
        rt.prog_argv = argv + 1;

        RegVM rvm;
        regvm_init(&rvm, &rt);

        LatValue result;
        RegVMResult rvm_res = regvm_run(&rvm, rchunk, &result);
        if (rvm_res != REGVM_OK) {
            fprintf(stderr, "regvm error: %s\n", rvm.error);
            regvm_free(&rvm);
            lat_runtime_free(&rt);
            regchunk_free(rchunk);
            return 1;
        }

        value_free(&result);
        regvm_free(&rvm);
        lat_runtime_free(&rt);
        regchunk_free(rchunk);
        return 0;
    }

    /* ── Stack VM bytecode ── */
    char *err = NULL;
    Chunk *chunk = chunk_load(path, &err);
    if (!chunk) {
        fprintf(stderr, "error: %s\n", err);
        free(err);
        return 1;
    }

    /* Disconnect the fluid heap — the VM uses plain malloc/free */
    value_set_heap(NULL);
    value_set_arena(NULL);

    /* Initialize runtime with native functions */
    LatRuntime rt;
    lat_runtime_init(&rt);
    rt.prog_argc = argc - 1; /* argv[1..] from the script's perspective */
    rt.prog_argv = argv + 1;

    /* Create and run the VM */
    StackVM vm;
    stackvm_init(&vm, &rt);

    LatValue result;
    StackVMResult vm_res = stackvm_run(&vm, chunk, &result);
    if (vm_res != STACKVM_OK) {
        fprintf(stderr, "vm error: %s\n", vm.error);
        stackvm_print_stack_trace(&vm);
        stackvm_free(&vm);
        lat_runtime_free(&rt);
        chunk_free(chunk);
        return 1;
    }

    value_free(&result);
    stackvm_free(&vm);
    lat_runtime_free(&rt);
    chunk_free(chunk);
    return 0;
}
