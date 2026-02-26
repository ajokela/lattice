#include "package.h"
#include "toml_ops.h"
#include "fs_ops.h"
#include "builtins.h"
#include "value.h"
#include "http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

/* Default HTTP registry URL */
#define PKG_DEFAULT_REGISTRY "https://registry.lattice-lang.org/v1"

/* ========================================================================
 * Helpers
 * ======================================================================== */

static char *safe_strdup(const char *s) { return s ? strdup(s) : NULL; }

/* Get string value from a TOML map, or NULL. Result is a borrowed pointer. */
static const char *map_get_str(LatValue *map, const char *key) {
    if (!map || map->type != VAL_MAP) return NULL;
    LatValue *v = lat_map_get(map->as.map.map, key);
    if (!v || v->type != VAL_STR) return NULL;
    return v->as.str_val;
}

/* ========================================================================
 * Semver utilities
 * ======================================================================== */

/* Parse a semver string "MAJOR.MINOR.PATCH" into components.
 * Returns true if valid (at least major is parsed). */
bool pkg_semver_parse(const char *s, int *major, int *minor, int *patch) {
    *major = *minor = *patch = 0;
    if (!s || !*s) return false;

    char *end;
    long val = strtol(s, &end, 10);
    if (end == s || val < 0) return false;
    *major = (int)val;

    if (*end != '.') return true;
    s = end + 1;
    val = strtol(s, &end, 10);
    if (end == s || val < 0) return true;
    *minor = (int)val;

    if (*end != '.') return true;
    s = end + 1;
    val = strtol(s, &end, 10);
    if (end == s || val < 0) return true;
    *patch = (int)val;

    return true;
}

/* Compare two semver strings.
 * Returns <0 if a < b, 0 if a == b, >0 if a > b. */
int pkg_semver_compare(const char *a, const char *b) {
    int a_maj, a_min, a_pat;
    int b_maj, b_min, b_pat;

    if (!pkg_semver_parse(a, &a_maj, &a_min, &a_pat)) return -1;
    if (!pkg_semver_parse(b, &b_maj, &b_min, &b_pat)) return 1;

    if (a_maj != b_maj) return a_maj - b_maj;
    if (a_min != b_min) return a_min - b_min;
    return a_pat - b_pat;
}

/* Check if a version satisfies a constraint.
 * Supports: "*" (any), "1.2.3" (exact), "^1.2.3" (compatible),
 * ">=1.2.3" (minimum), "<=1.2.3" (maximum). */
bool pkg_semver_satisfies(const char *constraint, const char *version) {
    if (!constraint || strcmp(constraint, "*") == 0) return true;
    if (!version) return false;

    if (constraint[0] == '^') {
        /* Caret: compatible with (same major, >= minor.patch, < next major) */
        int c_maj, c_min, c_pat;
        int v_maj, v_min, v_pat;
        if (!pkg_semver_parse(constraint + 1, &c_maj, &c_min, &c_pat)) return false;
        if (!pkg_semver_parse(version, &v_maj, &v_min, &v_pat)) return false;
        if (v_maj != c_maj) return false;
        if (v_min > c_min) return true;
        if (v_min < c_min) return false;
        return v_pat >= c_pat;
    }

    if (constraint[0] == '~') {
        /* Tilde: approximately (same major.minor, >= patch, < next minor) */
        int c_maj, c_min, c_pat;
        int v_maj, v_min, v_pat;
        if (!pkg_semver_parse(constraint + 1, &c_maj, &c_min, &c_pat)) return false;
        if (!pkg_semver_parse(version, &v_maj, &v_min, &v_pat)) return false;
        if (v_maj != c_maj) return false;
        if (v_min != c_min) return false;
        return v_pat >= c_pat;
    }

    if (constraint[0] == '>' && constraint[1] == '=') { return pkg_semver_compare(version, constraint + 2) >= 0; }
    if (constraint[0] == '<' && constraint[1] == '=') { return pkg_semver_compare(version, constraint + 2) <= 0; }

    /* Exact match */
    return strcmp(constraint, version) == 0;
}

/* ========================================================================
 * Global cache directory (~/.lattice/packages/)
 * ======================================================================== */

/* Build the path to the global package cache directory.
 * Returns a heap-allocated string, or NULL if HOME is unset. */
static char *get_cache_dir(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;

    size_t len = strlen(home) + 32;
    char *path = malloc(len);
    snprintf(path, len, "%s/.lattice/packages", home);
    return path;
}

/* Ensure the cache directory hierarchy exists (~/.lattice/packages/).
 * Returns true on success. */
static bool ensure_cache_dir(void) {
    const char *home = getenv("HOME");
    if (!home) return false;

    char path[PATH_MAX];
    char *mkdir_err = NULL;

    /* ~/.lattice/ */
    snprintf(path, sizeof(path), "%s/.lattice", home);
    if (!fs_is_dir(path)) {
        if (!fs_mkdir(path, &mkdir_err)) {
            free(mkdir_err);
            return false;
        }
    }

    /* ~/.lattice/packages/ */
    snprintf(path, sizeof(path), "%s/.lattice/packages", home);
    if (!fs_is_dir(path)) {
        if (!fs_mkdir(path, &mkdir_err)) {
            free(mkdir_err);
            return false;
        }
    }

    return true;
}

/* Check if a package@version is already cached.
 * Returns a heap-allocated path to the cached package directory, or NULL. */
static char *find_cached_package(const char *name, const char *version) {
    char *cache_dir = get_cache_dir();
    if (!cache_dir) return NULL;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/%s", cache_dir, name, version);
    free(cache_dir);

    if (fs_is_dir(path)) return strdup(path);
    return NULL;
}

/* ========================================================================
 * HTTP registry client
 * ======================================================================== */

/* Build the registry base URL from env or default. Returned string is
 * static or from getenv — do NOT free. */
static const char *get_registry_url(void) {
    const char *url = getenv("LATTICE_REGISTRY");
    /* Only use LATTICE_REGISTRY if it looks like an HTTP(S) URL */
    if (url && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)) return url;
    return PKG_DEFAULT_REGISTRY;
}

/* Fetch package version list from registry.
 * GET <registry>/packages/<name>/versions
 * Expected JSON response: {"versions":["1.0.0","1.1.0","2.0.0"]}
 * On success, returns a heap-allocated array of version strings (caller frees
 * each string and the array). Sets *out_count. Returns NULL on error. */
static char **registry_fetch_versions(const char *name, size_t *out_count, char **err) {
    *out_count = 0;

    const char *base = get_registry_url();
    size_t url_len = strlen(base) + strlen(name) + 32;
    char *url = malloc(url_len);
    snprintf(url, url_len, "%s/packages/%s/versions", base, name);

    HttpRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = url;
    req.timeout_ms = 15000;

    /* Add Accept header */
    char *hdr_keys[] = {"Accept"};
    char *hdr_vals[] = {"application/json"};
    req.header_keys = hdr_keys;
    req.header_values = hdr_vals;
    req.header_count = 1;

    HttpResponse *resp = http_execute(&req, err);
    free(url);

    if (!resp) return NULL;

    if (resp->status_code == 404) {
        size_t elen = strlen(name) + 64;
        *err = malloc(elen);
        snprintf(*err, elen, "package '%s' not found in registry", name);
        http_response_free(resp);
        return NULL;
    }

    if (resp->status_code != 200) {
        size_t elen = 128;
        *err = malloc(elen);
        snprintf(*err, elen, "registry returned HTTP %d for package '%s'", resp->status_code, name);
        http_response_free(resp);
        return NULL;
    }

    /* Minimal JSON parsing for {"versions":["x","y",...]}
     * We look for the "versions" array and extract quoted strings. */
    const char *body = resp->body;
    const char *arr_start = strstr(body, "\"versions\"");
    if (!arr_start) {
        *err = strdup("registry response: missing 'versions' field");
        http_response_free(resp);
        return NULL;
    }
    arr_start = strchr(arr_start, '[');
    if (!arr_start) {
        *err = strdup("registry response: malformed versions array");
        http_response_free(resp);
        return NULL;
    }
    arr_start++; /* skip '[' */

    const char *arr_end = strchr(arr_start, ']');
    if (!arr_end) {
        *err = strdup("registry response: unterminated versions array");
        http_response_free(resp);
        return NULL;
    }

    /* Extract quoted strings */
    size_t cap = 8;
    char **versions = malloc(cap * sizeof(char *));
    size_t count = 0;

    const char *p = arr_start;
    while (p < arr_end) {
        const char *q1 = memchr(p, '"', (size_t)(arr_end - p));
        if (!q1) break;
        q1++; /* skip opening quote */
        const char *q2 = memchr(q1, '"', (size_t)(arr_end - q1));
        if (!q2) break;

        if (count >= cap) {
            cap *= 2;
            versions = realloc(versions, cap * sizeof(char *));
        }
        versions[count++] = strndup(q1, (size_t)(q2 - q1));
        p = q2 + 1;
    }

    http_response_free(resp);
    *out_count = count;
    return versions;
}

/* Find the best matching version from a list given a constraint.
 * Returns a heap-allocated copy of the best version, or NULL. */
static char *find_best_version(char **versions, size_t count, const char *constraint) {
    char *best = NULL;
    for (size_t i = 0; i < count; i++) {
        if (!pkg_semver_satisfies(constraint, versions[i])) continue;
        if (!best || pkg_semver_compare(versions[i], best) > 0) {
            free(best);
            best = strdup(versions[i]);
        }
    }
    return best;
}

/* Download a package tarball/source from the registry and cache it.
 * GET <registry>/packages/<name>/<version>
 * The response body is expected to be the contents of a lattice.toml + source
 * files (for now we treat the body as a single main.lat file, or a tarball
 * that we store directly). Returns true on success. */
static bool registry_download_package(const char *name, const char *version, const char *dest_dir, char **err) {
    const char *base = get_registry_url();
    size_t url_len = strlen(base) + strlen(name) + strlen(version) + 32;
    char *url = malloc(url_len);
    snprintf(url, url_len, "%s/packages/%s/%s", base, name, version);

    HttpRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url = url;
    req.timeout_ms = 30000;

    HttpResponse *resp = http_execute(&req, err);
    free(url);

    if (!resp) return false;

    if (resp->status_code != 200) {
        size_t elen = 128;
        *err = malloc(elen);
        snprintf(*err, elen, "registry download returned HTTP %d for %s@%s", resp->status_code, name, version);
        http_response_free(resp);
        return false;
    }

    /* Write the response body to dest_dir.
     * Check Content-Type to decide how to handle it.
     * For now, we treat the body as the package source. */
    bool is_toml_bundle = false;
    for (size_t i = 0; i < resp->header_count; i++) {
        if (strcmp(resp->header_keys[i], "content-type") == 0) {
            if (strstr(resp->header_values[i], "application/toml") || strstr(resp->header_values[i], "text/toml")) {
                is_toml_bundle = true;
            }
            break;
        }
    }

    /* Create destination directory */
    char *mkdir_err = NULL;
    if (!fs_is_dir(dest_dir)) {
        if (!fs_mkdir(dest_dir, &mkdir_err)) {
            size_t elen = strlen(mkdir_err) + 64;
            *err = malloc(elen);
            snprintf(*err, elen, "cannot create package directory: %s", mkdir_err);
            free(mkdir_err);
            http_response_free(resp);
            return false;
        }
    }

    if (is_toml_bundle) {
        /* If the response is a TOML manifest, write it as lattice.toml */
        char toml_path[PATH_MAX + 16];
        snprintf(toml_path, sizeof(toml_path), "%s/lattice.toml", dest_dir);
        if (!builtin_write_file(toml_path, resp->body)) {
            *err = strdup("failed to write package lattice.toml");
            http_response_free(resp);
            return false;
        }
    } else {
        /* Default: write body as main.lat (single-file package) */
        char main_path[PATH_MAX + 16];
        snprintf(main_path, sizeof(main_path), "%s/main.lat", dest_dir);
        if (!builtin_write_file(main_path, resp->body)) {
            *err = strdup("failed to write package main.lat");
            http_response_free(resp);
            return false;
        }

        /* Also generate a minimal lattice.toml for the cached package */
        char toml_path[PATH_MAX + 16];
        snprintf(toml_path, sizeof(toml_path), "%s/lattice.toml", dest_dir);
        char toml_buf[512];
        snprintf(toml_buf, sizeof(toml_buf), "[package]\nname = \"%s\"\nversion = \"%s\"\n", name, version);
        builtin_write_file(toml_path, toml_buf);
    }

    http_response_free(resp);
    return true;
}

/* High-level: fetch a package from the HTTP registry, caching in ~/.lattice/packages/.
 * On success, copies the package into lat_modules/<name>/ and returns true. */
static bool fetch_from_registry(const char *name, const char *version, char **err) {
    /* Step 1: Query available versions */
    size_t ver_count = 0;
    char **versions = registry_fetch_versions(name, &ver_count, err);
    if (!versions) {
        /* err is already set by registry_fetch_versions */
        return false;
    }

    if (ver_count == 0) {
        free(versions);
        size_t elen = strlen(name) + 64;
        *err = malloc(elen);
        snprintf(*err, elen, "package '%s' has no published versions", name);
        return false;
    }

    /* Step 2: Find best matching version */
    char *resolved = find_best_version(versions, ver_count, version);
    for (size_t i = 0; i < ver_count; i++) free(versions[i]);
    free(versions);

    if (!resolved) {
        size_t elen = strlen(name) + strlen(version) + 80;
        *err = malloc(elen);
        snprintf(*err, elen, "no version of '%s' satisfies constraint '%s'", name, version);
        return false;
    }

    /* Step 3: Check local cache first */
    char *cached = find_cached_package(name, resolved);
    if (cached) {
        /* Copy from cache to lat_modules/ */
        if (!fs_is_dir("lat_modules")) {
            char *mkdir_err = NULL;
            if (!fs_mkdir("lat_modules", &mkdir_err)) {
                size_t elen = strlen(mkdir_err) + 64;
                *err = malloc(elen);
                snprintf(*err, elen, "cannot create lat_modules/: %s", mkdir_err);
                free(mkdir_err);
                free(cached);
                free(resolved);
                return false;
            }
        }
        char cmd[PATH_MAX * 2 + 32];
        snprintf(cmd, sizeof(cmd), "cp -R '%s' 'lat_modules/%s'", cached, name);
        int rc = system(cmd);
        free(cached);
        if (rc != 0) {
            size_t elen = 256;
            *err = malloc(elen);
            snprintf(*err, elen, "failed to copy cached package '%s' to lat_modules/", name);
            free(resolved);
            return false;
        }
        fprintf(stderr, "  (cached %s@%s)\n", name, resolved);
        free(resolved);
        return true;
    }

    /* Step 4: Download from registry to cache */
    if (!ensure_cache_dir()) {
        size_t elen = 128;
        *err = malloc(elen);
        snprintf(*err, elen, "cannot create package cache directory (~/.lattice/packages/)");
        free(resolved);
        return false;
    }

    char *cache_dir = get_cache_dir();
    char pkg_cache[PATH_MAX];
    /* Create name subdirectory in cache */
    snprintf(pkg_cache, sizeof(pkg_cache), "%s/%s", cache_dir, name);
    if (!fs_is_dir(pkg_cache)) {
        char *mkdir_err = NULL;
        if (!fs_mkdir(pkg_cache, &mkdir_err)) {
            free(mkdir_err);
            /* Non-fatal, we can still try the version dir */
        }
    }
    /* Create version subdirectory */
    snprintf(pkg_cache, sizeof(pkg_cache), "%s/%s/%s", cache_dir, name, resolved);
    free(cache_dir);

    if (!registry_download_package(name, resolved, pkg_cache, err)) {
        free(resolved);
        return false;
    }

    /* Step 5: Copy from cache to lat_modules/ */
    if (!fs_is_dir("lat_modules")) {
        char *mkdir_err = NULL;
        if (!fs_mkdir("lat_modules", &mkdir_err)) {
            size_t elen = strlen(mkdir_err) + 64;
            *err = malloc(elen);
            snprintf(*err, elen, "cannot create lat_modules/: %s", mkdir_err);
            free(mkdir_err);
            free(resolved);
            return false;
        }
    }

    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "cp -R '%s' 'lat_modules/%s'", pkg_cache, name);
    int rc = system(cmd);
    if (rc != 0) {
        size_t elen = 256;
        *err = malloc(elen);
        snprintf(*err, elen, "failed to copy package '%s' from cache to lat_modules/", name);
        free(resolved);
        return false;
    }

    fprintf(stderr, "  (downloaded %s@%s from registry)\n", name, resolved);
    free(resolved);
    return true;
}

/* ========================================================================
 * Manifest parsing
 * ======================================================================== */

bool pkg_manifest_parse(const char *toml_str, PkgManifest *out, char **err) {
    memset(out, 0, sizeof(PkgManifest));

    LatValue root = toml_ops_parse(toml_str, err);
    if (*err) return false;

    if (root.type != VAL_MAP) {
        value_free(&root);
        *err = strdup("lattice.toml: expected a TOML document (map)");
        return false;
    }

    /* [package] section */
    LatValue *pkg = lat_map_get(root.as.map.map, "package");
    if (pkg && pkg->type == VAL_MAP) {
        out->meta.name = safe_strdup(map_get_str(pkg, "name"));
        out->meta.version = safe_strdup(map_get_str(pkg, "version"));
        out->meta.description = safe_strdup(map_get_str(pkg, "description"));
        out->meta.license = safe_strdup(map_get_str(pkg, "license"));
        out->meta.entry = safe_strdup(map_get_str(pkg, "entry"));
    }

    /* Default entry point */
    if (!out->meta.entry) out->meta.entry = strdup("main.lat");

    /* [dependencies] section */
    LatValue *deps = lat_map_get(root.as.map.map, "dependencies");
    if (deps && deps->type == VAL_MAP) {
        size_t cap = 8;
        out->deps = malloc(cap * sizeof(PkgDep));
        if (!out->deps) return false;
        out->dep_count = 0;
        out->dep_cap = cap;

        for (size_t i = 0; i < deps->as.map.map->cap; i++) {
            if (deps->as.map.map->entries[i].state != MAP_OCCUPIED) continue;
            const char *dep_name = deps->as.map.map->entries[i].key;
            LatValue *dep_val = (LatValue *)deps->as.map.map->entries[i].value;

            if (out->dep_count >= out->dep_cap) {
                out->dep_cap *= 2;
                out->deps = realloc(out->deps, out->dep_cap * sizeof(PkgDep));
            }

            out->deps[out->dep_count].name = strdup(dep_name);
            if (dep_val->type == VAL_STR) {
                out->deps[out->dep_count].version = strdup(dep_val->as.str_val);
            } else {
                out->deps[out->dep_count].version = strdup("*");
            }
            out->dep_count++;
        }
    }

    value_free(&root);
    return true;
}

char *pkg_manifest_to_toml(const PkgManifest *m) {
    /* Build TOML string manually for clean output */
    size_t cap = 512;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

#define APPEND(...)                                          \
    do {                                                     \
        int n = snprintf(buf + len, cap - len, __VA_ARGS__); \
        if (n < 0) break;                                    \
        while (len + (size_t)n >= cap) {                     \
            cap *= 2;                                        \
            buf = realloc(buf, cap);                         \
        }                                                    \
        n = snprintf(buf + len, cap - len, __VA_ARGS__);     \
        if (n > 0) len += (size_t)n;                         \
    } while (0)

    APPEND("[package]\n");
    if (m->meta.name) APPEND("name = \"%s\"\n", m->meta.name);
    if (m->meta.version) APPEND("version = \"%s\"\n", m->meta.version);
    if (m->meta.description) APPEND("description = \"%s\"\n", m->meta.description);
    if (m->meta.license) APPEND("license = \"%s\"\n", m->meta.license);
    if (m->meta.entry && strcmp(m->meta.entry, "main.lat") != 0) APPEND("entry = \"%s\"\n", m->meta.entry);

    if (m->dep_count > 0) {
        APPEND("\n[dependencies]\n");
        for (size_t i = 0; i < m->dep_count; i++) { APPEND("%s = \"%s\"\n", m->deps[i].name, m->deps[i].version); }
    }

#undef APPEND
    return buf;
}

void pkg_manifest_free(PkgManifest *m) {
    free(m->meta.name);
    free(m->meta.version);
    free(m->meta.description);
    free(m->meta.license);
    free(m->meta.entry);
    for (size_t i = 0; i < m->dep_count; i++) {
        free(m->deps[i].name);
        free(m->deps[i].version);
    }
    free(m->deps);
    memset(m, 0, sizeof(PkgManifest));
}

/* ========================================================================
 * Lock file
 * ======================================================================== */

bool pkg_lock_parse(const char *toml_str, PkgLock *out, char **err) {
    memset(out, 0, sizeof(PkgLock));

    LatValue root = toml_ops_parse(toml_str, err);
    if (*err) return false;

    if (root.type != VAL_MAP) {
        value_free(&root);
        *err = strdup("lattice.lock: expected a TOML document");
        return false;
    }

    /* [[package]] array of tables */
    LatValue *packages = lat_map_get(root.as.map.map, "package");
    if (packages && packages->type == VAL_ARRAY) {
        size_t cap = packages->as.array.len < 4 ? 4 : packages->as.array.len;
        out->entries = malloc(cap * sizeof(PkgLockEntry));
        if (!out->entries) return false;
        out->entry_count = 0;
        out->entry_cap = cap;

        for (size_t i = 0; i < packages->as.array.len; i++) {
            LatValue *entry = &packages->as.array.elems[i];
            if (entry->type != VAL_MAP) continue;

            if (out->entry_count >= out->entry_cap) {
                out->entry_cap *= 2;
                out->entries = realloc(out->entries, out->entry_cap * sizeof(PkgLockEntry));
            }

            PkgLockEntry *le = &out->entries[out->entry_count];
            le->name = safe_strdup(map_get_str(entry, "name"));
            le->version = safe_strdup(map_get_str(entry, "version"));
            le->source = safe_strdup(map_get_str(entry, "source"));
            le->checksum = safe_strdup(map_get_str(entry, "checksum"));
            out->entry_count++;
        }
    }

    value_free(&root);
    return true;
}

char *pkg_lock_to_toml(const PkgLock *lock) {
    size_t cap = 512;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;

#define APPEND(...)                                          \
    do {                                                     \
        int n = snprintf(buf + len, cap - len, __VA_ARGS__); \
        if (n < 0) break;                                    \
        while (len + (size_t)n >= cap) {                     \
            cap *= 2;                                        \
            buf = realloc(buf, cap);                         \
        }                                                    \
        n = snprintf(buf + len, cap - len, __VA_ARGS__);     \
        if (n > 0) len += (size_t)n;                         \
    } while (0)

    APPEND("# This file is auto-generated by clat. Do not edit manually.\n\n");

    for (size_t i = 0; i < lock->entry_count; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        APPEND("[[package]]\n");
        if (e->name) APPEND("name = \"%s\"\n", e->name);
        if (e->version) APPEND("version = \"%s\"\n", e->version);
        if (e->source) APPEND("source = \"%s\"\n", e->source);
        if (e->checksum) APPEND("checksum = \"%s\"\n", e->checksum);
        APPEND("\n");
    }

#undef APPEND
    return buf;
}

void pkg_lock_free(PkgLock *lock) {
    for (size_t i = 0; i < lock->entry_count; i++) {
        free(lock->entries[i].name);
        free(lock->entries[i].version);
        free(lock->entries[i].source);
        free(lock->entries[i].checksum);
    }
    free(lock->entries);
    memset(lock, 0, sizeof(PkgLock));
}

/* ========================================================================
 * Dependency graph with cycle detection
 * ======================================================================== */

void pkg_dep_graph_init(PkgDepGraph *g) {
    g->node_count = 0;
    g->node_cap = 8;
    g->nodes = malloc(g->node_cap * sizeof(PkgDepNode));
}

/* Find a node by name; returns (size_t)-1 if not found. */
static size_t dep_graph_find(const PkgDepGraph *g, const char *name) {
    for (size_t i = 0; i < g->node_count; i++) {
        if (strcmp(g->nodes[i].name, name) == 0) return i;
    }
    return (size_t)-1;
}

size_t pkg_dep_graph_add_node(PkgDepGraph *g, const char *name) {
    size_t idx = dep_graph_find(g, name);
    if (idx != (size_t)-1) return idx;

    if (g->node_count >= g->node_cap) {
        g->node_cap *= 2;
        g->nodes = realloc(g->nodes, g->node_cap * sizeof(PkgDepNode));
    }

    PkgDepNode *n = &g->nodes[g->node_count];
    n->name = strdup(name);
    n->edge_count = 0;
    n->edge_cap = 4;
    n->edges = malloc(n->edge_cap * sizeof(size_t));
    return g->node_count++;
}

void pkg_dep_graph_add_edge(PkgDepGraph *g, size_t from, size_t to) {
    PkgDepNode *n = &g->nodes[from];
    /* Avoid duplicate edges */
    for (size_t i = 0; i < n->edge_count; i++) {
        if (n->edges[i] == to) return;
    }
    if (n->edge_count >= n->edge_cap) {
        n->edge_cap *= 2;
        n->edges = realloc(n->edges, n->edge_cap * sizeof(size_t));
    }
    n->edges[n->edge_count++] = to;
}

/* DFS states for cycle detection. */
enum { DFS_WHITE = 0, DFS_GRAY = 1, DFS_BLACK = 2 };

/* Recursive DFS helper.
 * stack[] stores node indices on the current DFS path.
 * stack_depth is the current depth. Returns true if cycle found. */
static bool dfs_visit(const PkgDepGraph *g, size_t node, int *color, size_t *stack, size_t stack_depth,
                      char **cycle_path) {
    color[node] = DFS_GRAY;
    stack[stack_depth] = node;

    const PkgDepNode *n = &g->nodes[node];
    for (size_t i = 0; i < n->edge_count; i++) {
        size_t neighbor = n->edges[i];
        if (color[neighbor] == DFS_GRAY) {
            /* Found a cycle — build a human-readable path */
            if (cycle_path) {
                /* Find where the cycle starts in the stack */
                size_t cycle_start = 0;
                for (size_t j = 0; j <= stack_depth; j++) {
                    if (stack[j] == neighbor) {
                        cycle_start = j;
                        break;
                    }
                }
                /* Build path string: A -> B -> C -> A */
                size_t buf_cap = 256;
                char *buf = malloc(buf_cap);
                size_t buf_len = 0;
                buf[0] = '\0';

                for (size_t j = cycle_start; j <= stack_depth; j++) {
                    const char *name = g->nodes[stack[j]].name;
                    size_t name_len = strlen(name);
                    while (buf_len + name_len + 8 >= buf_cap) {
                        buf_cap *= 2;
                        buf = realloc(buf, buf_cap);
                    }
                    if (buf_len > 0) {
                        memcpy(buf + buf_len, " -> ", 4);
                        buf_len += 4;
                    }
                    memcpy(buf + buf_len, name, name_len);
                    buf_len += name_len;
                    buf[buf_len] = '\0';
                }
                /* Close the cycle */
                const char *cycle_name = g->nodes[neighbor].name;
                size_t cname_len = strlen(cycle_name);
                while (buf_len + cname_len + 8 >= buf_cap) {
                    buf_cap *= 2;
                    buf = realloc(buf, buf_cap);
                }
                memcpy(buf + buf_len, " -> ", 4);
                buf_len += 4;
                memcpy(buf + buf_len, cycle_name, cname_len);
                buf_len += cname_len;
                buf[buf_len] = '\0';

                *cycle_path = buf;
            }
            return true;
        }
        if (color[neighbor] == DFS_WHITE) {
            if (dfs_visit(g, neighbor, color, stack, stack_depth + 1, cycle_path)) return true;
        }
    }

    color[node] = DFS_BLACK;
    return false;
}

bool pkg_dep_graph_has_cycle(const PkgDepGraph *g, char **cycle_path) {
    if (cycle_path) *cycle_path = NULL;
    if (g->node_count == 0) return false;

    int *color = calloc(g->node_count, sizeof(int)); /* all DFS_WHITE */
    size_t *stack = malloc(g->node_count * sizeof(size_t));

    bool found = false;
    for (size_t i = 0; i < g->node_count; i++) {
        if (color[i] == DFS_WHITE) {
            if (dfs_visit(g, i, color, stack, 0, cycle_path)) {
                found = true;
                break;
            }
        }
    }

    free(color);
    free(stack);
    return found;
}

bool pkg_dep_graph_build(const PkgManifest *root, const char *project_dir, PkgDepGraph *out, char **err) {
    pkg_dep_graph_init(out);

    /* Add root node */
    const char *root_name = root->meta.name ? root->meta.name : "(root)";
    size_t root_idx = pkg_dep_graph_add_node(out, root_name);

    /* BFS queue for transitive dependency discovery */
    size_t *queue = malloc(64 * sizeof(size_t));
    size_t queue_cap = 64;
    size_t queue_head = 0, queue_tail = 0;

    /* Add root's direct dependencies */
    for (size_t i = 0; i < root->dep_count; i++) {
        size_t dep_idx = pkg_dep_graph_add_node(out, root->deps[i].name);
        pkg_dep_graph_add_edge(out, root_idx, dep_idx);
        if (queue_tail >= queue_cap) {
            queue_cap *= 2;
            queue = realloc(queue, queue_cap * sizeof(size_t));
        }
        queue[queue_tail++] = dep_idx;
    }

    /* Process transitive dependencies via BFS */
    while (queue_head < queue_tail) {
        size_t cur = queue[queue_head++];
        const char *pkg_name = out->nodes[cur].name;

        /* Try to read this package's lattice.toml from lat_modules/ */
        char toml_path[PATH_MAX];
        if (project_dir) {
            snprintf(toml_path, sizeof(toml_path), "%s/lat_modules/%s/lattice.toml", project_dir, pkg_name);
        } else {
            snprintf(toml_path, sizeof(toml_path), "lat_modules/%s/lattice.toml", pkg_name);
        }

        if (!fs_file_exists(toml_path)) continue;

        char *toml_src = builtin_read_file(toml_path);
        if (!toml_src) continue;

        PkgManifest dep_manifest;
        char *parse_err = NULL;
        if (!pkg_manifest_parse(toml_src, &dep_manifest, &parse_err)) {
            free(parse_err);
            free(toml_src);
            continue;
        }
        free(toml_src);

        for (size_t i = 0; i < dep_manifest.dep_count; i++) {
            size_t dep_idx = pkg_dep_graph_add_node(out, dep_manifest.deps[i].name);
            pkg_dep_graph_add_edge(out, cur, dep_idx);

            /* Only enqueue if this is the first time we've seen this node
             * (i.e., it was just created by add_node) */
            if (out->nodes[dep_idx].edge_count == 0 && dep_idx != cur) {
                if (queue_tail >= queue_cap) {
                    queue_cap *= 2;
                    queue = realloc(queue, queue_cap * sizeof(size_t));
                }
                queue[queue_tail++] = dep_idx;
            }
        }

        pkg_manifest_free(&dep_manifest);
    }

    free(queue);

    /* Check for cycles */
    char *cycle = NULL;
    if (pkg_dep_graph_has_cycle(out, &cycle)) {
        if (err) {
            size_t elen = strlen(cycle) + 64;
            *err = malloc(elen);
            snprintf(*err, elen, "circular dependency detected: %s", cycle);
        }
        free(cycle);
        return false;
    }

    return true;
}

void pkg_dep_graph_free(PkgDepGraph *g) {
    for (size_t i = 0; i < g->node_count; i++) {
        free(g->nodes[i].name);
        free(g->nodes[i].edges);
    }
    free(g->nodes);
    memset(g, 0, sizeof(PkgDepGraph));
}

/* ========================================================================
 * CLI: clat init
 * ======================================================================== */

int pkg_cmd_init(void) {
    if (fs_file_exists("lattice.toml")) {
        fprintf(stderr, "error: lattice.toml already exists\n");
        return 1;
    }

    /* Derive package name from directory name */
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "error: cannot get working directory\n");
        return 1;
    }
    char *cwd_copy = strdup(cwd);
    char *dir_name = basename(cwd_copy);

    PkgManifest m;
    memset(&m, 0, sizeof(m));
    m.meta.name = strdup(dir_name);
    m.meta.version = strdup("0.1.0");
    m.meta.entry = strdup("main.lat");

    char *toml = pkg_manifest_to_toml(&m);
    pkg_manifest_free(&m);
    free(cwd_copy);

    bool ok = builtin_write_file("lattice.toml", toml);
    free(toml);

    if (!ok) {
        fprintf(stderr, "error: cannot write lattice.toml\n");
        return 1;
    }

    printf("Created lattice.toml\n");
    return 0;
}

/* ========================================================================
 * CLI: clat install
 * ======================================================================== */

static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)flen + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)flen, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Check if a version matches a constraint (wrapper around pkg_semver_satisfies). */
static bool version_matches(const char *constraint, const char *actual) {
    return pkg_semver_satisfies(constraint, actual);
}

/* Attempt to fetch a package. Sets *out_source to "local", "registry", or "path"
 * to indicate how the package was resolved. Caller should not free out_source. */
static bool fetch_package(const char *name, const char *version, char **err, const char **out_source) {
    if (out_source) *out_source = "local";
    /* Check if the package already exists in lat_modules/ */
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "lat_modules/%s", name);
    if (fs_is_dir(path)) {
        /* Already installed. Verify version if lattice.toml exists in the package. */
        char toml_path[PATH_MAX];
        snprintf(toml_path, sizeof(toml_path), "lat_modules/%s/lattice.toml", name);
        if (fs_file_exists(toml_path)) {
            char *toml_src = read_file_str(toml_path);
            if (toml_src) {
                PkgManifest dep_manifest;
                char *parse_err = NULL;
                if (pkg_manifest_parse(toml_src, &dep_manifest, &parse_err)) {
                    if (dep_manifest.meta.version && !version_matches(version, dep_manifest.meta.version)) {
                        fprintf(stderr, "  warning: %s@%s installed, but %s requested\n", name,
                                dep_manifest.meta.version, version);
                    }
                    pkg_manifest_free(&dep_manifest);
                } else {
                    free(parse_err);
                }
                free(toml_src);
            }
        }
        return true; /* already present */
    }

    /* Try registry URL (env var LATTICE_REGISTRY or default). */
    const char *registry = getenv("LATTICE_REGISTRY");

    /* file:// URL support: copy from a local registry directory */
    if (registry && strncmp(registry, "file://", 7) == 0) {
        const char *registry_dir = registry + 7;
        char src_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", registry_dir, name);
        if (fs_is_dir(src_path)) {
            /* Create lat_modules/ if needed */
            if (!fs_is_dir("lat_modules")) {
                char *mkdir_err = NULL;
                if (!fs_mkdir("lat_modules", &mkdir_err)) {
                    if (err) {
                        size_t elen = strlen(mkdir_err) + 64;
                        *err = malloc(elen);
                        if (!*err) return false;
                        snprintf(*err, elen, "cannot create lat_modules/: %s", mkdir_err);
                    }
                    free(mkdir_err);
                    return false;
                }
            }

            /* Copy directory recursively using system cp */
            char cmd[PATH_MAX * 2 + 32];
            snprintf(cmd, sizeof(cmd), "cp -R '%s' 'lat_modules/%s'", src_path, name);
            int rc = system(cmd);
            if (rc != 0) {
                if (err) {
                    size_t elen = 256;
                    *err = malloc(elen);
                    snprintf(*err, elen, "failed to copy package '%s' from registry", name);
                }
                return false;
            }
            if (out_source) *out_source = "path";
            return true;
        }
    }

    /* HTTP registry support: try to fetch from remote registry.
     * Uses LATTICE_REGISTRY env (if HTTP/HTTPS) or the default registry URL. */
    {
        char *fetch_err = NULL;
        if (fetch_from_registry(name, version, &fetch_err)) {
            if (out_source) *out_source = "registry";
            return true;
        }
        /* Registry fetch failed — store error but keep going for a friendlier message */
        if (fetch_err) {
            if (err) {
                size_t elen = strlen(name) + strlen(fetch_err) + 128;
                *err = malloc(elen);
                snprintf(*err, elen, "package '%s' not found locally; registry fetch also failed: %s", name, fetch_err);
            }
            free(fetch_err);
            return false;
        }
    }

    if (err) {
        size_t elen = 256;
        *err = malloc(elen);
        if (!*err) return false;
        snprintf(*err, elen, "package '%s' not found (not in lat_modules/ and registry unavailable)", name);
    }
    return false;
}

/* Look up a locked version for a package name.
 * Returns a borrowed pointer to the version string, or NULL if not locked. */
static const char *lock_find_version(const PkgLock *lock, const char *name) {
    for (size_t i = 0; i < lock->entry_count; i++) {
        if (lock->entries[i].name && strcmp(lock->entries[i].name, name) == 0) return lock->entries[i].version;
    }
    return NULL;
}

int pkg_cmd_install(void) {
    if (!fs_file_exists("lattice.toml")) {
        fprintf(stderr, "error: no lattice.toml found. Run 'clat init' first.\n");
        return 1;
    }

    char *toml_src = read_file_str("lattice.toml");
    if (!toml_src) {
        fprintf(stderr, "error: cannot read lattice.toml\n");
        return 1;
    }

    PkgManifest manifest;
    char *parse_err = NULL;
    if (!pkg_manifest_parse(toml_src, &manifest, &parse_err)) {
        fprintf(stderr, "error: %s\n", parse_err);
        free(parse_err);
        free(toml_src);
        return 1;
    }
    free(toml_src);

    if (manifest.dep_count == 0) {
        printf("No dependencies to install.\n");
        pkg_manifest_free(&manifest);
        return 0;
    }

    /* Read existing lock file for reproducible builds */
    PkgLock existing_lock;
    memset(&existing_lock, 0, sizeof(existing_lock));
    bool have_lock = false;
    if (fs_file_exists("lattice.lock")) {
        char *lock_src = read_file_str("lattice.lock");
        if (lock_src) {
            char *lock_err = NULL;
            if (pkg_lock_parse(lock_src, &existing_lock, &lock_err)) {
                have_lock = true;
            } else {
                fprintf(stderr, "warning: cannot parse lattice.lock: %s\n", lock_err);
                free(lock_err);
            }
            free(lock_src);
        }
    }

    /* Create lat_modules/ directory if needed */
    if (!fs_is_dir("lat_modules")) {
        char *mkdir_err = NULL;
        if (!fs_mkdir("lat_modules", &mkdir_err)) {
            fprintf(stderr, "error: cannot create lat_modules/: %s\n", mkdir_err);
            free(mkdir_err);
            if (have_lock) pkg_lock_free(&existing_lock);
            pkg_manifest_free(&manifest);
            return 1;
        }
    }

    /* Resolve each dependency */
    PkgLock lock;
    memset(&lock, 0, sizeof(lock));
    lock.entry_cap = manifest.dep_count < 4 ? 4 : manifest.dep_count;
    lock.entries = malloc(lock.entry_cap * sizeof(PkgLockEntry));
    if (!lock.entries) {
        if (have_lock) pkg_lock_free(&existing_lock);
        pkg_manifest_free(&manifest);
        return 1;
    }
    lock.entry_count = 0;

    int failures = 0;
    for (size_t i = 0; i < manifest.dep_count; i++) {
        const char *dep_name = manifest.deps[i].name;
        const char *dep_ver = manifest.deps[i].version;

        /* If we have a lock file, prefer the locked version for reproducibility.
         * Only use it if the locked version still satisfies the manifest constraint. */
        const char *locked_ver = have_lock ? lock_find_version(&existing_lock, dep_name) : NULL;
        if (locked_ver && pkg_semver_satisfies(dep_ver, locked_ver)) { dep_ver = locked_ver; }

        printf("  Installing %s@%s...", dep_name, dep_ver);
        fflush(stdout);

        char *fetch_err = NULL;
        const char *pkg_source = "local";
        if (fetch_package(dep_name, dep_ver, &fetch_err, &pkg_source)) {
            printf(" ok\n");

            /* Determine resolved version */
            char toml_path[PATH_MAX];
            snprintf(toml_path, sizeof(toml_path), "lat_modules/%s/lattice.toml", dep_name);
            char *resolved_ver = strdup(dep_ver);
            if (fs_file_exists(toml_path)) {
                char *dep_toml = read_file_str(toml_path);
                if (dep_toml) {
                    PkgManifest dep_m;
                    char *dep_err = NULL;
                    if (pkg_manifest_parse(dep_toml, &dep_m, &dep_err)) {
                        if (dep_m.meta.version) {
                            free(resolved_ver);
                            resolved_ver = strdup(dep_m.meta.version);
                        }
                        pkg_manifest_free(&dep_m);
                    } else {
                        free(dep_err);
                    }
                    free(dep_toml);
                }
            }

            /* Add to lock */
            if (lock.entry_count >= lock.entry_cap) {
                lock.entry_cap *= 2;
                lock.entries = realloc(lock.entries, lock.entry_cap * sizeof(PkgLockEntry));
            }
            PkgLockEntry *le = &lock.entries[lock.entry_count++];
            le->name = strdup(dep_name);
            le->version = resolved_ver;
            le->source = strdup(pkg_source);
            le->checksum = strdup("");
        } else {
            printf(" FAILED\n");
            fprintf(stderr, "  error: %s\n", fetch_err ? fetch_err : "unknown error");
            free(fetch_err);
            failures++;
        }
    }

    if (have_lock) pkg_lock_free(&existing_lock);

    /* Check for circular dependencies in the resolved graph */
    if (failures == 0) {
        PkgDepGraph graph;
        char *graph_err = NULL;
        if (!pkg_dep_graph_build(&manifest, NULL, &graph, &graph_err)) {
            fprintf(stderr, "error: %s\n", graph_err);
            free(graph_err);
            pkg_dep_graph_free(&graph);
            pkg_lock_free(&lock);
            pkg_manifest_free(&manifest);
            return 1;
        }
        pkg_dep_graph_free(&graph);
    }

    /* Write lock file */
    if (lock.entry_count > 0) {
        char *lock_toml = pkg_lock_to_toml(&lock);
        if (!builtin_write_file("lattice.lock", lock_toml)) { fprintf(stderr, "warning: cannot write lattice.lock\n"); }
        free(lock_toml);
    }

    pkg_lock_free(&lock);
    pkg_manifest_free(&manifest);

    if (failures > 0) {
        fprintf(stderr, "\n%d package(s) failed to install.\n", failures);
        return 1;
    }

    printf("\nAll dependencies installed.\n");
    return 0;
}

/* ========================================================================
 * CLI: clat add <package> [version]
 * ======================================================================== */

int pkg_cmd_add(const char *name, const char *version) {
    if (!name || name[0] == '\0') {
        fprintf(stderr, "error: package name required\n");
        return 1;
    }

    if (!version) version = "*";

    /* Read existing manifest or create one */
    PkgManifest manifest;
    memset(&manifest, 0, sizeof(manifest));

    if (fs_file_exists("lattice.toml")) {
        char *toml_src = read_file_str("lattice.toml");
        if (toml_src) {
            char *parse_err = NULL;
            if (!pkg_manifest_parse(toml_src, &manifest, &parse_err)) {
                fprintf(stderr, "error: %s\n", parse_err);
                free(parse_err);
                free(toml_src);
                return 1;
            }
            free(toml_src);
        }
    } else {
        /* No manifest yet — initialize a minimal one */
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            char *cwd_copy = strdup(cwd);
            manifest.meta.name = strdup(basename(cwd_copy));
            free(cwd_copy);
        } else {
            manifest.meta.name = strdup("my-project");
        }
        manifest.meta.version = strdup("0.1.0");
        manifest.meta.entry = strdup("main.lat");
    }

    /* Check if dependency already exists */
    for (size_t i = 0; i < manifest.dep_count; i++) {
        if (strcmp(manifest.deps[i].name, name) == 0) {
            free(manifest.deps[i].version);
            manifest.deps[i].version = strdup(version);
            printf("Updated %s to version %s\n", name, version);
            goto write_manifest;
        }
    }

    /* Add new dependency */
    if (manifest.dep_count >= manifest.dep_cap) {
        manifest.dep_cap = manifest.dep_cap < 4 ? 4 : manifest.dep_cap * 2;
        manifest.deps = realloc(manifest.deps, manifest.dep_cap * sizeof(PkgDep));
    }
    manifest.deps[manifest.dep_count].name = strdup(name);
    manifest.deps[manifest.dep_count].version = strdup(version);
    manifest.dep_count++;
    printf("Added %s@%s to dependencies\n", name, version);

write_manifest:;
    char *toml = pkg_manifest_to_toml(&manifest);
    if (!builtin_write_file("lattice.toml", toml)) {
        fprintf(stderr, "error: cannot write lattice.toml\n");
        free(toml);
        pkg_manifest_free(&manifest);
        return 1;
    }
    free(toml);
    pkg_manifest_free(&manifest);

    /* Try to install */
    return pkg_cmd_install();
}

/* ========================================================================
 * CLI: clat remove <package>
 * ======================================================================== */

int pkg_cmd_remove(const char *name) {
    if (!name || name[0] == '\0') {
        fprintf(stderr, "error: package name required\n");
        return 1;
    }

    if (!fs_file_exists("lattice.toml")) {
        fprintf(stderr, "error: no lattice.toml found\n");
        return 1;
    }

    char *toml_src = read_file_str("lattice.toml");
    if (!toml_src) {
        fprintf(stderr, "error: cannot read lattice.toml\n");
        return 1;
    }

    PkgManifest manifest;
    char *parse_err = NULL;
    if (!pkg_manifest_parse(toml_src, &manifest, &parse_err)) {
        fprintf(stderr, "error: %s\n", parse_err);
        free(parse_err);
        free(toml_src);
        return 1;
    }
    free(toml_src);

    /* Find and remove the dependency */
    bool found = false;
    for (size_t i = 0; i < manifest.dep_count; i++) {
        if (strcmp(manifest.deps[i].name, name) == 0) {
            free(manifest.deps[i].name);
            free(manifest.deps[i].version);
            /* Shift remaining entries */
            for (size_t j = i; j + 1 < manifest.dep_count; j++) manifest.deps[j] = manifest.deps[j + 1];
            manifest.dep_count--;
            found = true;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "error: '%s' is not a dependency\n", name);
        pkg_manifest_free(&manifest);
        return 1;
    }

    /* Write updated manifest */
    char *toml = pkg_manifest_to_toml(&manifest);
    if (!builtin_write_file("lattice.toml", toml)) {
        fprintf(stderr, "error: cannot write lattice.toml\n");
        free(toml);
        pkg_manifest_free(&manifest);
        return 1;
    }
    free(toml);
    pkg_manifest_free(&manifest);

    /* Remove from lat_modules/ */
    char mod_path[PATH_MAX];
    snprintf(mod_path, sizeof(mod_path), "lat_modules/%s", name);
    if (fs_is_dir(mod_path)) {
        char cmd[PATH_MAX + 16];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", mod_path);
        if (system(cmd) != 0) { fprintf(stderr, "warning: failed to remove '%s'\n", mod_path); }
    }

    /* Update lock file */
    if (fs_file_exists("lattice.lock")) {
        char *lock_src = read_file_str("lattice.lock");
        if (lock_src) {
            PkgLock lock;
            char *lock_err = NULL;
            if (pkg_lock_parse(lock_src, &lock, &lock_err)) {
                /* Remove entry from lock */
                for (size_t i = 0; i < lock.entry_count; i++) {
                    if (lock.entries[i].name && strcmp(lock.entries[i].name, name) == 0) {
                        free(lock.entries[i].name);
                        free(lock.entries[i].version);
                        free(lock.entries[i].source);
                        free(lock.entries[i].checksum);
                        for (size_t j = i; j + 1 < lock.entry_count; j++) lock.entries[j] = lock.entries[j + 1];
                        lock.entry_count--;
                        break;
                    }
                }
                char *lock_toml = pkg_lock_to_toml(&lock);
                builtin_write_file("lattice.lock", lock_toml);
                free(lock_toml);
                pkg_lock_free(&lock);
            } else {
                free(lock_err);
            }
            free(lock_src);
        }
    }

    printf("Removed %s\n", name);
    return 0;
}

/* ========================================================================
 * Module resolution: resolve import name via lat_modules/
 * ======================================================================== */

char *pkg_resolve_module(const char *name, const char *project_dir) {
    /* Only bare module names (no path separators, no leading dot) */
    if (!name || strchr(name, '/') || strchr(name, '\\') || name[0] == '.') return NULL;

    char candidate[PATH_MAX];
    char resolved[PATH_MAX];

    /* Strategy 1: lat_modules/<name>/main.lat (relative to project_dir) */
    if (project_dir) {
        snprintf(candidate, sizeof(candidate), "%s/lat_modules/%s/main.lat", project_dir, name);
    } else {
        snprintf(candidate, sizeof(candidate), "lat_modules/%s/main.lat", name);
    }
    if (realpath(candidate, resolved)) return strdup(resolved);

    /* Strategy 2: lat_modules/<name>/src/main.lat */
    if (project_dir) {
        snprintf(candidate, sizeof(candidate), "%s/lat_modules/%s/src/main.lat", project_dir, name);
    } else {
        snprintf(candidate, sizeof(candidate), "lat_modules/%s/src/main.lat", name);
    }
    if (realpath(candidate, resolved)) return strdup(resolved);

    /* Strategy 3: Check lattice.toml in the package for custom entry */
    char toml_path[PATH_MAX];
    if (project_dir) {
        snprintf(toml_path, sizeof(toml_path), "%s/lat_modules/%s/lattice.toml", project_dir, name);
    } else {
        snprintf(toml_path, sizeof(toml_path), "lat_modules/%s/lattice.toml", name);
    }
    if (fs_file_exists(toml_path)) {
        char *toml_src = read_file_str(toml_path);
        if (toml_src) {
            PkgManifest dep_m;
            char *err = NULL;
            if (pkg_manifest_parse(toml_src, &dep_m, &err)) {
                if (dep_m.meta.entry) {
                    if (project_dir) {
                        snprintf(candidate, sizeof(candidate), "%s/lat_modules/%s/%s", project_dir, name,
                                 dep_m.meta.entry);
                    } else {
                        snprintf(candidate, sizeof(candidate), "lat_modules/%s/%s", name, dep_m.meta.entry);
                    }
                    pkg_manifest_free(&dep_m);
                    free(toml_src);
                    if (realpath(candidate, resolved)) return strdup(resolved);
                    return NULL;
                }
                pkg_manifest_free(&dep_m);
            } else {
                free(err);
            }
            free(toml_src);
        }
    }

    /* Strategy 4: lat_modules/<name>.lat (single-file package) */
    if (project_dir) {
        snprintf(candidate, sizeof(candidate), "%s/lat_modules/%s.lat", project_dir, name);
    } else {
        snprintf(candidate, sizeof(candidate), "lat_modules/%s.lat", name);
    }
    if (realpath(candidate, resolved)) return strdup(resolved);

    /* Also try CWD-based lat_modules/ if project_dir didn't work */
    if (project_dir) {
        snprintf(candidate, sizeof(candidate), "lat_modules/%s/main.lat", name);
        if (realpath(candidate, resolved)) return strdup(resolved);

        snprintf(candidate, sizeof(candidate), "lat_modules/%s.lat", name);
        if (realpath(candidate, resolved)) return strdup(resolved);
    }

    return NULL;
}
