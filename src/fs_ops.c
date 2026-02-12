#include "fs_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifndef __EMSCRIPTEN__

#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>

bool fs_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool fs_delete_file(const char *path, char **err) {
    if (unlink(path) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "delete_file: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        return false;
    }
    return true;
}

char **fs_list_dir(const char *path, size_t *count, char **err) {
    DIR *dir = opendir(path);
    if (!dir) {
        char buf[512];
        snprintf(buf, sizeof(buf), "list_dir: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        *count = 0;
        return NULL;
    }

    size_t cap = 16;
    size_t len = 0;
    char **entries = malloc(cap * sizeof(char *));
    if (!entries) {
        closedir(dir);
        *err = strdup("list_dir: out of memory");
        *count = 0;
        return NULL;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (len >= cap) {
            cap *= 2;
            char **tmp = realloc(entries, cap * sizeof(char *));
            if (!tmp) {
                for (size_t i = 0; i < len; i++) free(entries[i]);
                free(entries);
                closedir(dir);
                *err = strdup("list_dir: out of memory");
                *count = 0;
                return NULL;
            }
            entries = tmp;
        }
        entries[len++] = strdup(ent->d_name);
    }
    closedir(dir);

    *count = len;
    return entries;
}

bool fs_append_file(const char *path, const char *data, char **err) {
    FILE *f = fopen(path, "a");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "append_file: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        return false;
    }
    if (fputs(data, f) == EOF) {
        char buf[512];
        snprintf(buf, sizeof(buf), "append_file: write failed: %s", strerror(errno));
        *err = strdup(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

bool fs_mkdir(const char *path, char **err) {
    if (mkdir(path, 0755) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "mkdir: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        return false;
    }
    return true;
}

bool fs_rename(const char *oldpath, const char *newpath, char **err) {
    if (rename(oldpath, newpath) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "rename: %s -> %s: %s", oldpath, newpath, strerror(errno));
        *err = strdup(buf);
        return false;
    }
    return true;
}

bool fs_is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool fs_is_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

bool fs_rmdir(const char *path, char **err) {
    if (rmdir(path) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "rmdir: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        return false;
    }
    return true;
}

char **fs_glob(const char *pattern, size_t *count, char **err) {
    glob_t gl;
    int flags = GLOB_TILDE;
#ifdef GLOB_BRACE
    flags |= GLOB_BRACE;
#endif
    int rc = glob(pattern, flags, NULL, &gl);
    if (rc == GLOB_NOMATCH) {
        globfree(&gl);
        *count = 0;
        return NULL; /* no error, just empty */
    }
    if (rc != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "glob: pattern error: %s", pattern);
        *err = strdup(buf);
        *count = 0;
        return NULL;
    }

    size_t n = gl.gl_pathc;
    char **results = malloc(n * sizeof(char *));
    if (!results) {
        globfree(&gl);
        *err = strdup("glob: out of memory");
        *count = 0;
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        results[i] = strdup(gl.gl_pathv[i]);
    }
    globfree(&gl);
    *count = n;
    return results;
}

bool fs_stat(const char *path, int64_t *size_out, int64_t *mtime_out,
             int64_t *mode_out, const char **type_out, char **err) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "stat: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        return false;
    }
    *size_out = (int64_t)st.st_size;
    *mtime_out = (int64_t)st.st_mtime * 1000;
    *mode_out = (int64_t)(st.st_mode & 07777);
    if (S_ISREG(st.st_mode))       *type_out = "file";
    else if (S_ISDIR(st.st_mode))  *type_out = "dir";
    else if (S_ISLNK(st.st_mode))  *type_out = "symlink";
    else                            *type_out = "other";
    return true;
}

bool fs_copy_file(const char *src, const char *dst, char **err) {
    FILE *fin = fopen(src, "rb");
    if (!fin) {
        char buf[512];
        snprintf(buf, sizeof(buf), "copy_file: cannot open source: %s: %s", src, strerror(errno));
        *err = strdup(buf);
        return false;
    }
    FILE *fout = fopen(dst, "wb");
    if (!fout) {
        char buf[512];
        snprintf(buf, sizeof(buf), "copy_file: cannot open destination: %s: %s", dst, strerror(errno));
        *err = strdup(buf);
        fclose(fin);
        return false;
    }

    char buffer[8192];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), fin)) > 0) {
        if (fwrite(buffer, 1, n, fout) != n) {
            char buf[512];
            snprintf(buf, sizeof(buf), "copy_file: write failed: %s", strerror(errno));
            *err = strdup(buf);
            fclose(fin);
            fclose(fout);
            return false;
        }
    }
    if (ferror(fin)) {
        char buf[512];
        snprintf(buf, sizeof(buf), "copy_file: read failed: %s", strerror(errno));
        *err = strdup(buf);
        fclose(fin);
        fclose(fout);
        return false;
    }
    fclose(fin);
    fclose(fout);
    return true;
}

char *fs_realpath(const char *path, char **err) {
    char *resolved = realpath(path, NULL);
    if (!resolved) {
        char buf[512];
        snprintf(buf, sizeof(buf), "realpath: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        return NULL;
    }
    return resolved;
}

char *fs_tempdir(char **err) {
    char tmpl[] = "/tmp/lattice_XXXXXX";
    char *result = mkdtemp(tmpl);
    if (!result) {
        char buf[512];
        snprintf(buf, sizeof(buf), "tempdir: %s", strerror(errno));
        *err = strdup(buf);
        return NULL;
    }
    return strdup(result);
}

char *fs_tempfile(char **err) {
    char tmpl[] = "/tmp/lattice_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "tempfile: %s", strerror(errno));
        *err = strdup(buf);
        return NULL;
    }
    close(fd);
    return strdup(tmpl);
}

#else /* __EMSCRIPTEN__ */

bool fs_file_exists(const char *path) {
    (void)path;
    return false;
}

bool fs_delete_file(const char *path, char **err) {
    (void)path;
    *err = strdup("delete_file: not available in browser");
    return false;
}

char **fs_list_dir(const char *path, size_t *count, char **err) {
    (void)path;
    *count = 0;
    *err = strdup("list_dir: not available in browser");
    return NULL;
}

bool fs_append_file(const char *path, const char *data, char **err) {
    (void)path; (void)data;
    *err = strdup("append_file: not available in browser");
    return false;
}

bool fs_mkdir(const char *path, char **err) {
    (void)path;
    *err = strdup("mkdir: not available in browser");
    return false;
}

bool fs_rename(const char *oldpath, const char *newpath, char **err) {
    (void)oldpath; (void)newpath;
    *err = strdup("rename: not available in browser");
    return false;
}

bool fs_is_dir(const char *path) {
    (void)path;
    return false;
}

bool fs_is_file(const char *path) {
    (void)path;
    return false;
}

bool fs_rmdir(const char *path, char **err) {
    (void)path;
    *err = strdup("rmdir: not available in browser");
    return false;
}

char **fs_glob(const char *pattern, size_t *count, char **err) {
    (void)pattern;
    *count = 0;
    *err = strdup("glob: not available in browser");
    return NULL;
}

bool fs_stat(const char *path, int64_t *size_out, int64_t *mtime_out,
             int64_t *mode_out, const char **type_out, char **err) {
    (void)path; (void)size_out; (void)mtime_out; (void)mode_out; (void)type_out;
    *err = strdup("stat: not available in browser");
    return false;
}

bool fs_copy_file(const char *src, const char *dst, char **err) {
    (void)src; (void)dst;
    *err = strdup("copy_file: not available in browser");
    return false;
}

char *fs_realpath(const char *path, char **err) {
    (void)path;
    *err = strdup("realpath: not available in browser");
    return NULL;
}

char *fs_tempdir(char **err) {
    *err = strdup("tempdir: not available in browser");
    return NULL;
}

char *fs_tempfile(char **err) {
    *err = strdup("tempfile: not available in browser");
    return NULL;
}

#endif /* __EMSCRIPTEN__ */
