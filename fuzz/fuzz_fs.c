/* Fuzz harness for lattice's read-only filesystem path functions.
 * Targets the Windows _WIN32 branches of fs_ops (path canonicalization, stat,
 * glob) fed arbitrary path bytes. Read-only: NEVER calls a destructive op.
 *
 * Build:  make fuzz-fs
 * Run:    FUZZ_FS_SANDBOX=/tmp/sb ./build/fuzz_fs -max_len=1024 -timeout=5
 */
#include "fs_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

/* Run inside an empty sandbox dir. We fuzz the single-path, read-only queries
 * that PARSE/CANONICALIZE a path (fs_realpath is the juiciest on Windows); the
 * traversing ops (fs_glob/fs_list_dir) are deliberately excluded — on absolute or
 * recursive patterns they walk the real filesystem, which is slow (timeout noise)
 * and not what we're testing. libFuzzer's -timeout still backstops any hang. */
static void ensure_sandbox(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *dir = getenv("FUZZ_FS_SANDBOX");
    if (dir && dir[0]) { (void)chdir(dir); }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4096) return 0;
    ensure_sandbox();

    char *path = malloc(size + 1);
    if (!path) return 0;
    memcpy(path, data, size);
    path[size] = '\0';

    /* Single-path, read-only queries */
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

    free(path);
    return 0;
}
