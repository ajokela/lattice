#include "fs_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifndef __EMSCRIPTEN__

#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#include "win32_compat.h"
#else
#include <unistd.h>
#include <glob.h>
#endif
#include <dirent.h>

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
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
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
#ifdef _WIN32
    if (_mkdir(path) != 0) {
#else
    if (mkdir(path, 0755) != 0) {
#endif
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

#ifdef _WIN32
char **fs_glob(const char *pattern, size_t *count, char **err) {
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        *count = 0;
        return NULL; /* no matches, not an error */
    }

    /* Extract directory prefix from pattern */
    const char *last_sep = strrchr(pattern, '/');
    const char *last_bsep = strrchr(pattern, '\\');
    if (last_bsep && (!last_sep || last_bsep > last_sep)) last_sep = last_bsep;
    size_t prefix_len = last_sep ? (size_t)(last_sep - pattern + 1) : 0;

    size_t cap = 16, len = 0;
    char **results = malloc(cap * sizeof(char *));
    if (!results) {
        FindClose(hFind);
        *err = strdup("glob: out of memory");
        *count = 0;
        return NULL;
    }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (len >= cap) {
            cap *= 2;
            char **tmp = realloc(results, cap * sizeof(char *));
            if (!tmp) {
                for (size_t i = 0; i < len; i++) free(results[i]);
                free(results);
                FindClose(hFind);
                *err = strdup("glob: out of memory");
                *count = 0;
                return NULL;
            }
            results = tmp;
        }
        size_t name_len = strlen(fd.cFileName);
        results[len] = malloc(prefix_len + name_len + 1);
        if (prefix_len > 0) memcpy(results[len], pattern, prefix_len);
        memcpy(results[len] + prefix_len, fd.cFileName, name_len + 1);
        len++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    *count = len;
    return results;
}
#else
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
    for (size_t i = 0; i < n; i++) { results[i] = strdup(gl.gl_pathv[i]); }
    globfree(&gl);
    *count = n;
    return results;
}
#endif

bool fs_stat(const char *path, int64_t *size_out, int64_t *mtime_out, int64_t *mode_out, const char **type_out,
             char **err) {
    struct stat st;
#ifdef _WIN32
    if (stat(path, &st) != 0) {
#else
    if (lstat(path, &st) != 0) {
#endif
        char buf[512];
        snprintf(buf, sizeof(buf), "stat: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        return false;
    }
    *size_out = (int64_t)st.st_size;
    *mtime_out = (int64_t)st.st_mtime * 1000;
    *mode_out = (int64_t)(st.st_mode & 07777);
    if (S_ISREG(st.st_mode)) *type_out = "file";
    else if (S_ISDIR(st.st_mode)) *type_out = "dir";
#ifndef _WIN32
    else if (S_ISLNK(st.st_mode)) *type_out = "symlink";
#endif
    else *type_out = "other";
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
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char tmpl[MAX_PATH + 32];
    snprintf(tmpl, sizeof(tmpl), "%slattice_XXXXXX", tmp);
#else
    char tmpl[] = "/tmp/lattice_XXXXXX";
#endif
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
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char tmpl[MAX_PATH + 32];
    snprintf(tmpl, sizeof(tmpl), "%slattice_XXXXXX", tmp);
#else
    char tmpl[] = "/tmp/lattice_XXXXXX";
#endif
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "tempfile: %s", strerror(errno));
        *err = strdup(buf);
        return NULL;
    }
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
    return strdup(tmpl);
}

bool fs_chmod(const char *path, int mode, char **err) {
#ifdef _WIN32
    if (_chmod(path, mode) != 0) {
#else
    if (chmod(path, (mode_t)mode) != 0) {
#endif
        char buf[512];
        snprintf(buf, sizeof(buf), "chmod: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        return false;
    }
    return true;
}

int64_t fs_file_size(const char *path, char **err) {
    struct stat st;
    if (stat(path, &st) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "file_size: %s: %s", path, strerror(errno));
        *err = strdup(buf);
        return -1;
    }
    return (int64_t)st.st_size;
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
    (void)path;
    (void)data;
    *err = strdup("append_file: not available in browser");
    return false;
}

bool fs_mkdir(const char *path, char **err) {
    (void)path;
    *err = strdup("mkdir: not available in browser");
    return false;
}

bool fs_rename(const char *oldpath, const char *newpath, char **err) {
    (void)oldpath;
    (void)newpath;
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

bool fs_stat(const char *path, int64_t *size_out, int64_t *mtime_out, int64_t *mode_out, const char **type_out,
             char **err) {
    (void)path;
    (void)size_out;
    (void)mtime_out;
    (void)mode_out;
    (void)type_out;
    *err = strdup("stat: not available in browser");
    return false;
}

bool fs_copy_file(const char *src, const char *dst, char **err) {
    (void)src;
    (void)dst;
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

bool fs_chmod(const char *path, int mode, char **err) {
    (void)path;
    (void)mode;
    *err = strdup("chmod: not available in browser");
    return false;
}

int64_t fs_file_size(const char *path, char **err) {
    (void)path;
    *err = strdup("file_size: not available in browser");
    return -1;
}

#endif /* __EMSCRIPTEN__ */
