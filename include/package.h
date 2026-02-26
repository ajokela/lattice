#ifndef PACKAGE_H
#define PACKAGE_H

#include "value.h"
#include <stdbool.h>
#include <stddef.h>

/* ── Manifest (lattice.toml) ── */

typedef struct {
    char *name;
    char *version;
    char *description;
    char *license;
    char *entry; /* entry point, default "main.lat" */
} PkgMeta;

typedef struct {
    char *name;
    char *version; /* semver constraint e.g. "0.2.0" */
} PkgDep;

typedef struct {
    PkgMeta meta;
    PkgDep *deps;
    size_t dep_count;
    size_t dep_cap;
} PkgManifest;

/* ── Lock file (lattice.lock) ── */

typedef struct {
    char *name;
    char *version;
    char *source;   /* "registry", "local", "path" */
    char *checksum; /* sha256 hex, or empty */
} PkgLockEntry;

typedef struct {
    PkgLockEntry *entries;
    size_t entry_count;
    size_t entry_cap;
} PkgLock;

/* ── Manifest I/O ── */

/* Parse lattice.toml from string. Sets *err on failure. Caller must free with pkg_manifest_free(). */
bool pkg_manifest_parse(const char *toml_str, PkgManifest *out, char **err);

/* Write a PkgManifest to TOML string. Caller must free returned string. */
char *pkg_manifest_to_toml(const PkgManifest *m);

/* Free a PkgManifest. */
void pkg_manifest_free(PkgManifest *m);

/* ── Lock file I/O ── */

/* Parse lattice.lock from string. Sets *err on failure. */
bool pkg_lock_parse(const char *toml_str, PkgLock *out, char **err);

/* Write a PkgLock to TOML string. Caller must free returned string. */
char *pkg_lock_to_toml(const PkgLock *lock);

/* Free a PkgLock. */
void pkg_lock_free(PkgLock *lock);

/* ── CLI commands ── */

/* `clat init` — create lattice.toml in the current directory. */
int pkg_cmd_init(void);

/* `clat install` — resolve and install deps from lattice.toml. */
int pkg_cmd_install(void);

/* `clat add <package> [version]` — add a dependency. */
int pkg_cmd_add(const char *name, const char *version);

/* `clat remove <package>` — remove a dependency. */
int pkg_cmd_remove(const char *name);

/* ── Semver utilities ── */

/* Parse a semver string "MAJOR.MINOR.PATCH" into components.
 * Returns true if valid (at least major is parsed). */
bool pkg_semver_parse(const char *version, int *major, int *minor, int *patch);

/* Compare two semver strings.
 * Returns <0 if a < b, 0 if a == b, >0 if a > b. */
int pkg_semver_compare(const char *a, const char *b);

/* Check if a version satisfies a constraint.
 * Supports: "*" (any), "1.2.3" (exact), "^1.2.3" (compatible),
 * "~1.2.3" (approximately: >=1.2.3, <1.3.0),
 * ">=1.2.3" (minimum), "<=1.2.3" (maximum). */
bool pkg_semver_satisfies(const char *constraint, const char *version);

/* ── Dependency graph ── */

/* Node in the dependency graph (adjacency list representation). */
typedef struct {
    char *name;    /* package name */
    size_t *edges; /* indices into graph's nodes[] array */
    size_t edge_count;
    size_t edge_cap;
} PkgDepNode;

/* Dependency graph with cycle detection support. */
typedef struct {
    PkgDepNode *nodes;
    size_t node_count;
    size_t node_cap;
} PkgDepGraph;

/* Initialize an empty dependency graph. */
void pkg_dep_graph_init(PkgDepGraph *g);

/* Add a node to the graph. Returns the node index. If a node with the
 * same name already exists, returns its index without adding a duplicate. */
size_t pkg_dep_graph_add_node(PkgDepGraph *g, const char *name);

/* Add a directed edge from `from` to `to` (by node index). */
void pkg_dep_graph_add_edge(PkgDepGraph *g, size_t from, size_t to);

/* Detect circular dependencies using DFS.
 * Returns true if a cycle is found. If cycle_path is non-NULL,
 * stores a heap-allocated human-readable cycle description (caller frees). */
bool pkg_dep_graph_has_cycle(const PkgDepGraph *g, char **cycle_path);

/* Build a dependency graph from a manifest and its transitive dependencies.
 * Reads lattice.toml from each dependency in lat_modules/.
 * Sets *err on failure. Returns true on success. */
bool pkg_dep_graph_build(const PkgManifest *root, const char *project_dir, PkgDepGraph *out, char **err);

/* Free all resources owned by a dependency graph. */
void pkg_dep_graph_free(PkgDepGraph *g);

/* ── Module resolution ── */

/* Try to resolve an import path via lat_modules/.
 * Given a module name (e.g. "json-utils"), checks:
 *   1. ./lat_modules/<name>/main.lat
 *   2. ./lat_modules/<name>/src/main.lat
 *   3. ./lat_modules/<name>/<entry> from lattice.toml
 * Returns a heap-allocated absolute path, or NULL if not found. */
char *pkg_resolve_module(const char *name, const char *project_dir);

#endif /* PACKAGE_H */
