#ifndef FS_OPS_H
#define FS_OPS_H

#include <stdbool.h>
#include <stddef.h>

/* Check whether a file (or directory) exists at the given path. */
bool fs_file_exists(const char *path);

/* Delete (unlink) a file. Returns true on success; on failure sets *err. */
bool fs_delete_file(const char *path, char **err);

/* List directory entries (skipping "." and "..").
 * Returns a heap-allocated array of heap-allocated strings.
 * Caller must free each string and the array itself.
 * Sets *count to the number of entries. On error, returns NULL and sets *err. */
char **fs_list_dir(const char *path, size_t *count, char **err);

/* Append data to a file (opens with "a" mode).
 * Returns true on success; on failure sets *err. */
bool fs_append_file(const char *path, const char *data, char **err);

#endif /* FS_OPS_H */
