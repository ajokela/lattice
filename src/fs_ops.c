#include "fs_ops.h"
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
