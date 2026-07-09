/* minifuzz — a tiny coverage-guided fuzzer for platforms without libFuzzer.
 *
 * libFuzzer is unsupported on the MinGW (x86_64-w64-windows-gnu) target, so this
 * provides a minimal libFuzzer-compatible driver: it links against a harness'
 * LLVMFuzzerTestOneInput and drives it with corpus-based, coverage-guided
 * mutation. Coverage comes from `-fsanitize-coverage=trace-pc-guard`; crashes are
 * caught by AddressSanitizer, whose death callback saves the reproducer.
 *
 * Build (with the target + harness objects, all compiled with
 *   -fsanitize=address -fsanitize-coverage=trace-pc-guard):
 *     clang --target=x86_64-w64-windows-gnu -fsanitize=address \
 *       <lib objs> <harness>.o minifuzz.c -o fuzz.exe
 * Run:
 *     fuzz.exe <corpus_dir> [-max_len=N] [-runs=N] [-artifact_prefix=PFX] [-seed=S]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* ---------- coverage (trace-pc-guard) ---------- */
#define COV_SIZE (1u << 21) /* enough for lattice's ~170k edges */
static uint8_t cov_map[COV_SIZE];
static uint32_t g_new_edges; /* total unique edges seen so far */

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {
    static uint32_t n = 0;
    if (start == stop || *start) return;
    for (uint32_t *p = start; p < stop; p++) *p = ++n; /* assign 1..n */
}
void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
    uint32_t idx = *guard;
    if (idx < COV_SIZE && !cov_map[idx]) {
        cov_map[idx] = 1;
        g_new_edges++;
    }
}

/* ---------- crash reproducer saving (ASan death callback) ---------- */
void __asan_set_death_callback(void (*cb)(void)); /* provided by the ASan runtime */
static const uint8_t *g_cur;
static size_t g_cur_len;
static const char *g_artifact_prefix = "";
static void on_death(void) {
    char name[1024];
    snprintf(name, sizeof name, "%scrash-%lu-%u", g_artifact_prefix, (unsigned long)time(NULL), (unsigned)g_new_edges);
    FILE *f = fopen(name, "wb");
    if (f) {
        if (g_cur && g_cur_len) fwrite(g_cur, 1, g_cur_len, f);
        fclose(f);
        fprintf(stderr, "\n== minifuzz: crash reproducer written to %s ==\n", name);
    }
}

/* ---------- corpus ---------- */
typedef struct {
    uint8_t *buf;
    size_t len;
} Unit;
static Unit *corpus;
static size_t corpus_n, corpus_cap;

static void corpus_add(const uint8_t *buf, size_t len) {
    if (corpus_n == corpus_cap) {
        corpus_cap = corpus_cap ? corpus_cap * 2 : 256;
        corpus = realloc(corpus, corpus_cap * sizeof(Unit));
    }
    uint8_t *copy = malloc(len ? len : 1);
    memcpy(copy, buf, len);
    corpus[corpus_n].buf = copy;
    corpus[corpus_n].len = len;
    corpus_n++;
}

static void load_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[2048];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n < 0) {
            fclose(f);
            continue;
        }
        uint8_t *buf = malloc((size_t)(n ? n : 1));
        size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        corpus_add(buf, got);
        free(buf);
    }
    closedir(d);
}

/* ---------- mutation ---------- */
static uint8_t mut_buf[1 << 20];
static size_t g_max_len = 4096;

static size_t mutate(const uint8_t *in, size_t len) {
    size_t n = len;
    if (n > g_max_len) n = g_max_len;
    memcpy(mut_buf, in, n);
    int rounds = 1 + (rand() % 5);
    for (int r = 0; r < rounds; r++) {
        int op = rand() % 4;
        if (op == 0 && n > 0) { /* flip a byte */
            mut_buf[rand() % n] ^= (uint8_t)(1 << (rand() % 8));
        } else if (op == 1 && n < g_max_len) { /* insert a byte */
            size_t p = rand() % (n + 1);
            memmove(mut_buf + p + 1, mut_buf + p, n - p);
            mut_buf[p] = (uint8_t)(rand() & 0xff);
            n++;
        } else if (op == 2 && n > 0) { /* delete a byte */
            size_t p = rand() % n;
            memmove(mut_buf + p, mut_buf + p + 1, n - p - 1);
            n--;
        } else if (op == 3 && n > 0) { /* set a random byte (interesting values) */
            static const uint8_t interesting[] = {0, 0xff, '/', '\\', ':', '.', '*', '?', '"', '<', '>', '|'};
            mut_buf[rand() % n] = interesting[rand() % (int)sizeof(interesting)];
        }
    }
    return n;
}

int main(int argc, char **argv) {
    unsigned seed = (unsigned)time(NULL);
    long runs = -1; /* infinite */
    const char *corpus_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "-max_len=", 9)) g_max_len = (size_t)strtoul(argv[i] + 9, NULL, 10);
        else if (!strncmp(argv[i], "-runs=", 6)) runs = strtol(argv[i] + 6, NULL, 10);
        else if (!strncmp(argv[i], "-seed=", 6)) seed = (unsigned)strtoul(argv[i] + 6, NULL, 10);
        else if (!strncmp(argv[i], "-artifact_prefix=", 17)) g_artifact_prefix = argv[i] + 17;
        else if (argv[i][0] != '-') corpus_dir = argv[i];
    }
    if (g_max_len > sizeof(mut_buf)) g_max_len = sizeof(mut_buf);
    srand(seed);
    __asan_set_death_callback(on_death);

    /* libFuzzer-compatible repro mode: if the positional arg is a regular file
     * (not a directory), run it once through the harness and exit. crash-report.py
     * reproduces a finding by invoking `exe <crashfile>`; a real crash aborts via
     * ASan here, a benign input exits 0. Only a directory is fuzzed as a corpus. */
    if (corpus_dir) {
        struct stat st;
        if (stat(corpus_dir, &st) == 0 && !(st.st_mode & S_IFDIR)) {
            FILE *f = fopen(corpus_dir, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                uint8_t *buf = malloc((size_t)(n > 0 ? n : 1));
                size_t got = fread(buf, 1, (size_t)(n > 0 ? n : 0), f);
                fclose(f);
                g_cur = buf;
                g_cur_len = got;
                LLVMFuzzerTestOneInput(buf, got);
                free(buf);
            }
            return 0;
        }
    }

    /* Seed the corpus from files in corpus_dir (MinGW provides dirent). */
    if (corpus_dir) load_dir(corpus_dir);
    if (corpus_n == 0) {
        uint8_t z = 0;
        corpus_add(&z, 1); /* never start empty */
    }
    fprintf(stderr, "minifuzz: seed=%u corpus=%zu max_len=%zu\n", seed, corpus_n, g_max_len);

    /* Replay the seed corpus once (surfaces initial-corpus crashes). */
    for (size_t i = 0; i < corpus_n; i++) {
        g_cur = corpus[i].buf;
        g_cur_len = corpus[i].len;
        LLVMFuzzerTestOneInput(corpus[i].buf, corpus[i].len);
    }

    time_t t0 = time(NULL), tlast = t0;
    long execs = 0;
    for (; runs < 0 || execs < runs; execs++) {
        Unit *u = &corpus[rand() % corpus_n];
        size_t mlen = mutate(u->buf, u->len);
        uint32_t before = g_new_edges;
        g_cur = mut_buf;
        g_cur_len = mlen;
        LLVMFuzzerTestOneInput(mut_buf, mlen);
        if (g_new_edges > before) corpus_add(mut_buf, mlen); /* coverage gain */

        time_t now = time(NULL);
        if (now != tlast) {
            long dt = (long)(now - t0);
            fprintf(stderr, "#%ld cov: %u corp: %zu exec/s: %ld\n", execs, g_new_edges, corpus_n,
                    dt > 0 ? execs / dt : execs);
            tlast = now;
        }
    }
    fprintf(stderr, "minifuzz: done, %ld execs, cov %u\n", execs, g_new_edges);
    return 0;
}
