#include "package.h"
#include "toml_ops.h"
#include "fs_ops.h"
#include "builtins.h"
#include "value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>

/* ========================================================================
 * Helpers
 * ======================================================================== */

static char *safe_strdup(const char *s) {
    return s ? strdup(s) : NULL;
}

/* Get string value from a TOML map, or NULL. Result is a borrowed pointer. */
static const char *map_get_str(LatValue *map, const char *key) {
    if (!map || map->type != VAL_MAP) return NULL;
    LatValue *v = lat_map_get(map->as.map.map, key);
    if (!v || v->type != VAL_STR) return NULL;
    return v->as.str_val;
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
    if (!out->meta.entry)
        out->meta.entry = strdup("main.lat");

    /* [dependencies] section */
    LatValue *deps = lat_map_get(root.as.map.map, "dependencies");
    if (deps && deps->type == VAL_MAP) {
        size_t cap = 8;
        out->deps = malloc(cap * sizeof(PkgDep));
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
    size_t len = 0;

#define APPEND(...) do { \
    int n = snprintf(buf + len, cap - len, __VA_ARGS__); \
    if (n < 0) break; \
    while (len + (size_t)n >= cap) { cap *= 2; buf = realloc(buf, cap); } \
    n = snprintf(buf + len, cap - len, __VA_ARGS__); \
    if (n > 0) len += (size_t)n; \
} while(0)

    APPEND("[package]\n");
    if (m->meta.name) APPEND("name = \"%s\"\n", m->meta.name);
    if (m->meta.version) APPEND("version = \"%s\"\n", m->meta.version);
    if (m->meta.description) APPEND("description = \"%s\"\n", m->meta.description);
    if (m->meta.license) APPEND("license = \"%s\"\n", m->meta.license);
    if (m->meta.entry && strcmp(m->meta.entry, "main.lat") != 0)
        APPEND("entry = \"%s\"\n", m->meta.entry);

    if (m->dep_count > 0) {
        APPEND("\n[dependencies]\n");
        for (size_t i = 0; i < m->dep_count; i++) {
            APPEND("%s = \"%s\"\n", m->deps[i].name, m->deps[i].version);
        }
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
            le->name     = safe_strdup(map_get_str(entry, "name"));
            le->version  = safe_strdup(map_get_str(entry, "version"));
            le->source   = safe_strdup(map_get_str(entry, "source"));
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
    size_t len = 0;

#define APPEND(...) do { \
    int n = snprintf(buf + len, cap - len, __VA_ARGS__); \
    if (n < 0) break; \
    while (len + (size_t)n >= cap) { cap *= 2; buf = realloc(buf, cap); } \
    n = snprintf(buf + len, cap - len, __VA_ARGS__); \
    if (n > 0) len += (size_t)n; \
} while(0)

    APPEND("# This file is auto-generated by clat. Do not edit manually.\n\n");

    for (size_t i = 0; i < lock->entry_count; i++) {
        const PkgLockEntry *e = &lock->entries[i];
        APPEND("[[package]]\n");
        if (e->name)     APPEND("name = \"%s\"\n", e->name);
        if (e->version)  APPEND("version = \"%s\"\n", e->version);
        if (e->source)   APPEND("source = \"%s\"\n", e->source);
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
    if (flen < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)flen + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)flen, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Simple semver check: exact match or wildcard "*" */
static bool version_matches(const char *constraint, const char *actual) {
    if (!constraint || strcmp(constraint, "*") == 0) return true;
    if (!actual) return false;
    return strcmp(constraint, actual) == 0;
}

/* Attempt to fetch a package from the registry (stubbed for local). */
static bool fetch_package(const char *name, const char *version, char **err) {
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
                        fprintf(stderr, "  warning: %s@%s installed, but %s requested\n",
                                name, dep_manifest.meta.version, version);
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

    /* Try registry URL (env var LATTICE_REGISTRY or default). For now, we only
     * support local file:// URLs or pre-populated lat_modules/. */
    const char *registry = getenv("LATTICE_REGISTRY");
    if (registry) {
        /* file:// URL support: copy from a local registry directory */
        if (strncmp(registry, "file://", 7) == 0) {
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
                return true;
            }
        }
        /* TODO: HTTP registry support */
    }

    if (err) {
        size_t elen = 256;
        *err = malloc(elen);
        snprintf(*err, elen, "package '%s' not found (not in lat_modules/ and no registry configured)", name);
    }
    return false;
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

    /* Create lat_modules/ directory if needed */
    if (!fs_is_dir("lat_modules")) {
        char *mkdir_err = NULL;
        if (!fs_mkdir("lat_modules", &mkdir_err)) {
            fprintf(stderr, "error: cannot create lat_modules/: %s\n", mkdir_err);
            free(mkdir_err);
            pkg_manifest_free(&manifest);
            return 1;
        }
    }

    /* Resolve each dependency */
    PkgLock lock;
    memset(&lock, 0, sizeof(lock));
    lock.entry_cap = manifest.dep_count < 4 ? 4 : manifest.dep_count;
    lock.entries = malloc(lock.entry_cap * sizeof(PkgLockEntry));
    lock.entry_count = 0;

    int failures = 0;
    for (size_t i = 0; i < manifest.dep_count; i++) {
        const char *dep_name = manifest.deps[i].name;
        const char *dep_ver  = manifest.deps[i].version;

        printf("  Installing %s@%s...", dep_name, dep_ver);
        fflush(stdout);

        char *fetch_err = NULL;
        if (fetch_package(dep_name, dep_ver, &fetch_err)) {
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
            le->name     = strdup(dep_name);
            le->version  = resolved_ver;
            le->source   = strdup("local");
            le->checksum = strdup("");
        } else {
            printf(" FAILED\n");
            fprintf(stderr, "  error: %s\n", fetch_err ? fetch_err : "unknown error");
            free(fetch_err);
            failures++;
        }
    }

    /* Write lock file */
    if (lock.entry_count > 0) {
        char *lock_toml = pkg_lock_to_toml(&lock);
        if (!builtin_write_file("lattice.lock", lock_toml)) {
            fprintf(stderr, "warning: cannot write lattice.lock\n");
        }
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

write_manifest: ;
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
            for (size_t j = i; j + 1 < manifest.dep_count; j++)
                manifest.deps[j] = manifest.deps[j + 1];
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
        if (system(cmd) != 0) {
            fprintf(stderr, "warning: failed to remove '%s'\n", mod_path);
        }
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
                        for (size_t j = i; j + 1 < lock.entry_count; j++)
                            lock.entries[j] = lock.entries[j + 1];
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
    if (!name || strchr(name, '/') || strchr(name, '\\') || name[0] == '.')
        return NULL;

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
                        snprintf(candidate, sizeof(candidate), "%s/lat_modules/%s/%s",
                                 project_dir, name, dep_m.meta.entry);
                    } else {
                        snprintf(candidate, sizeof(candidate), "lat_modules/%s/%s",
                                 name, dep_m.meta.entry);
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
