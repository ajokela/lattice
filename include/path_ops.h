#ifndef PATH_OPS_H
#define PATH_OPS_H

#include <stddef.h>

/* Join path components with '/'. Returns heap-allocated string. */
char *path_join(const char **parts, size_t count);

/* Return directory portion of path. Returns heap-allocated string.
 * "/foo/bar.txt" -> "/foo", "bar.txt" -> "." */
char *path_dir(const char *path);

/* Return base filename. Returns heap-allocated string.
 * "/foo/bar.txt" -> "bar.txt", "/foo/" -> "" */
char *path_base(const char *path);

/* Return file extension including dot. Returns heap-allocated string.
 * "foo.txt" -> ".txt", "foo" -> "", ".hidden" -> "" */
char *path_ext(const char *path);

#endif
