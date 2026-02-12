#ifndef FS_OPS_H
#define FS_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Create a directory with mode 0755.
 * Returns true on success; on failure sets *err. */
bool fs_mkdir(const char *path, char **err);

/* Rename/move a file or directory.
 * Returns true on success; on failure sets *err. */
bool fs_rename(const char *oldpath, const char *newpath, char **err);

/* Check if path is a directory. */
bool fs_is_dir(const char *path);

/* Check if path is a regular file. */
bool fs_is_file(const char *path);

/* Remove an empty directory.
 * Returns true on success; on failure sets *err. */
bool fs_rmdir(const char *path, char **err);

/* Glob pattern matching.
 * Returns a heap-allocated array of heap-allocated path strings.
 * Caller must free each string and the array itself.
 * Sets *count to the number of matches. On error, returns NULL and sets *err. */
char **fs_glob(const char *pattern, size_t *count, char **err);

/* Get file metadata.
 * Sets *size_out (bytes), *mtime_out (epoch ms), *mode_out (permission bits),
 * *type_out ("file", "dir", "symlink", "other" - static string, do not free).
 * Returns true on success; on failure sets *err. */
bool fs_stat(const char *path, int64_t *size_out, int64_t *mtime_out,
             int64_t *mode_out, const char **type_out, char **err);

/* Copy a file from src to dst.
 * Returns true on success; on failure sets *err. */
bool fs_copy_file(const char *src, const char *dst, char **err);

/* Resolve to absolute canonical path.
 * Returns heap-allocated string on success; on failure returns NULL and sets *err. */
char *fs_realpath(const char *path, char **err);

/* Create a temporary directory.
 * Returns heap-allocated path string; on failure returns NULL and sets *err. */
char *fs_tempdir(char **err);

/* Create a temporary file.
 * Returns heap-allocated path string; on failure returns NULL and sets *err. */
char *fs_tempfile(char **err);

/* Change file permissions.
 * Returns true on success; on failure sets *err. */
bool fs_chmod(const char *path, int mode, char **err);

/* Get file size in bytes.
 * Returns file size on success; on failure returns -1 and sets *err. */
int64_t fs_file_size(const char *path, char **err);

#endif /* FS_OPS_H */
