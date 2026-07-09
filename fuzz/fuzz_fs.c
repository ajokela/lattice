/* Fuzz harness for lattice's filesystem path functions (Windows-focused).
 *
 * Two things per input:
 *  1. read-only path queries fed the raw fuzzed bytes (fs_realpath/fs_stat/...),
 *     exercising path parsing/canonicalization;
 *  2. the DESTRUCTIVE fs_ops (create/copy/rename/chmod/mkdir/rmdir/delete) on
 *     paths CONFINED to a sandbox directory, so weird Windows names (reserved
 *     devices, trailing '.'/' ', unicode, long components) stress the _WIN32
 *     destructive-op branches without touching anything outside the sandbox.
 *
 * Confinement: destructive ops run only for inputs that are safe RELATIVE names
 * (no leading separator, no ':' drive/ADS, no '..' traversal) AND only when we
 * successfully chdir'd into $FUZZ_FS_SANDBOX. A safe-relative name joined to the
 * sandbox cannot escape it. The traversing/absolute cases are still covered by
 * the read-only queries above.
 *
 * Build:  make fuzz-fs
 * Run:    FUZZ_FS_SANDBOX=/tmp/sb ./build/fuzz_fs -max_len=1024 -timeout=5
 */
#include "fs_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <direct.h>
#define chdir  _chdir
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

static char g_sb[4096]; /* absolute sandbox path */
static int g_sb_ok = 0; /* destructive ops enabled only if we're inside it */

static void ensure_sandbox(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *dir = getenv("FUZZ_FS_SANDBOX");
    if (dir && dir[0] && chdir(dir) == 0) {
        if (getcwd(g_sb, sizeof g_sb)) g_sb_ok = 1;
    }
}

/* A fuzzed input is a SAFE relative name (cannot escape the sandbox) iff it has
 * no absolute/drive/traversal markers. */
static int is_safe_rel(const char *p, size_t n) {
    if (n == 0 || n > 200) return 0;
    if (p[0] == '/' || p[0] == '\\') return 0; /* absolute */
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\0' || p[i] == ':') return 0;                 /* NUL / drive / ADS */
        if (p[i] == '.' && i + 1 < n && p[i + 1] == '.') return 0; /* traversal */
    }
    return 1;
}

/* Exercise the destructive ops on sandbox-confined paths, then clean up. */
static void destructive(const char *rel) {
    if (!g_sb_ok) return;
    char a[4400], b[4400], d[4400];
    if ((size_t)snprintf(a, sizeof a, "%s/%s", g_sb, rel) >= sizeof a) return;
    if ((size_t)snprintf(b, sizeof b, "%s/%s~b", g_sb, rel) >= sizeof b) return;
    if ((size_t)snprintf(d, sizeof d, "%s/%s~d", g_sb, rel) >= sizeof d) return;
    char *e;
    e = NULL;
    fs_append_file(a, "x", &e);
    free(e); /* create file a */
    e = NULL;
    fs_copy_file(a, b, &e);
    free(e); /* copy a -> b */
    e = NULL;
    fs_rename(b, a, &e);
    free(e); /* rename b -> a */
    e = NULL;
    fs_chmod(a, 0644, &e);
    free(e);
    e = NULL;
    fs_mkdir(d, &e);
    free(e); /* make dir d */
    e = NULL;
    fs_rmdir(d, &e);
    free(e);
    e = NULL;
    fs_delete_file(a, &e);
    free(e); /* cleanup */
    e = NULL;
    fs_delete_file(b, &e);
    free(e);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4096) return 0;
    ensure_sandbox();

    char *path = malloc(size + 1);
    if (!path) return 0;
    memcpy(path, data, size);
    path[size] = '\0';

    /* 1. read-only queries on the raw path */
    (void)fs_file_exists(path);
    (void)fs_is_dir(path);
    (void)fs_is_file(path);
    char *err = NULL;
    char *rp = fs_realpath(path, &err);
    free(rp);
    free(err);
    err = NULL;
    (void)fs_file_size(path, &err);
    free(err);
    err = NULL;
    int64_t sz, mt, md;
    const char *type = NULL;
    (void)fs_stat(path, &sz, &mt, &md, &type, &err);
    free(err);

    /* 2. destructive ops, sandbox-confined */
    if (is_safe_rel(path, size)) destructive(path);

    free(path);
    return 0;
}
