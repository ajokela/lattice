/*
 * win32_compat.h — POSIX compatibility shims for Windows/MinGW builds.
 * Only included when _WIN32 is defined.
 *
 * NOTE: This header does NOT include <windows.h> to avoid namespace
 * pollution (winnt.h defines TokenType, etc.). Source files that need
 * Windows API should include <windows.h> themselves AFTER this header.
 */
#ifndef LATTICE_WIN32_COMPAT_H
#define LATTICE_WIN32_COMPAT_H

#ifdef _WIN32

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <direct.h>

/* ── ssize_t ── */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef ptrdiff_t ssize_t;
#endif

/* ── mkdir compat (MinGW mkdir takes 1 arg) ── */
#define mkdir(path, mode) _mkdir(path)

/* ── strndup (MinGW lacks it) ── */
static inline char *strndup(const char *s, size_t n) {
    size_t len = strlen(s);
    if (len > n) len = n;
    char *dup = (char *)malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

/* ── localtime_r / gmtime_r ── */
static inline struct tm *localtime_r(const time_t *timep, struct tm *result) {
    if (localtime_s(result, timep) != 0) return NULL;
    return result;
}

static inline struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (gmtime_s(result, timep) != 0) return NULL;
    return result;
}

/* ── strptime (minimal) ── */
/* Supports: %Y %m %d %H %M %S %b %B %p %I %% */
static inline int w32_parse_int(const char **s, int digits) {
    int val = 0;
    for (int i = 0; i < digits; i++) {
        if (**s < '0' || **s > '9') return -1;
        val = val * 10 + (**s - '0');
        (*s)++;
    }
    return val;
}

static inline const char *w32_skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static const char *w32_month_abbr[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *w32_month_full[] = {"January", "February", "March",     "April",   "May",      "June",
                                       "July",    "August",   "September", "October", "November", "December"};

static inline char *strptime(const char *s, const char *fmt, struct tm *tm) {
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'Y': {
                    int v = w32_parse_int(&s, 4);
                    if (v < 0) return NULL;
                    tm->tm_year = v - 1900;
                    break;
                }
                case 'm': {
                    s = w32_skip_spaces(s);
                    int v = w32_parse_int(&s, (*s != '0' && s[1] < '0') ? 1 : 2);
                    if (v < 1 || v > 12) return NULL;
                    tm->tm_mon = v - 1;
                    break;
                }
                case 'd': {
                    s = w32_skip_spaces(s);
                    int v = w32_parse_int(&s, (*s != '0' && s[1] < '0') ? 1 : 2);
                    if (v < 1 || v > 31) return NULL;
                    tm->tm_mday = v;
                    break;
                }
                case 'H': {
                    s = w32_skip_spaces(s);
                    int v = w32_parse_int(&s, 2);
                    if (v < 0 || v > 23) return NULL;
                    tm->tm_hour = v;
                    break;
                }
                case 'I': {
                    s = w32_skip_spaces(s);
                    int v = w32_parse_int(&s, 2);
                    if (v < 1 || v > 12) return NULL;
                    tm->tm_hour = v % 12; /* adjusted by %p */
                    break;
                }
                case 'M': {
                    s = w32_skip_spaces(s);
                    int v = w32_parse_int(&s, 2);
                    if (v < 0 || v > 59) return NULL;
                    tm->tm_min = v;
                    break;
                }
                case 'S': {
                    s = w32_skip_spaces(s);
                    int v = w32_parse_int(&s, 2);
                    if (v < 0 || v > 60) return NULL;
                    tm->tm_sec = v;
                    break;
                }
                case 'p': {
                    if (_strnicmp(s, "PM", 2) == 0) {
                        if (tm->tm_hour < 12) tm->tm_hour += 12;
                        s += 2;
                    } else if (_strnicmp(s, "AM", 2) == 0) {
                        if (tm->tm_hour == 12) tm->tm_hour = 0;
                        s += 2;
                    } else {
                        return NULL;
                    }
                    break;
                }
                case 'b': {
                    int found = 0;
                    for (int i = 0; i < 12; i++) {
                        if (_strnicmp(s, w32_month_abbr[i], 3) == 0) {
                            tm->tm_mon = i;
                            s += 3;
                            found = 1;
                            break;
                        }
                    }
                    if (!found) return NULL;
                    break;
                }
                case 'B': {
                    int found = 0;
                    for (int i = 0; i < 12; i++) {
                        size_t len = strlen(w32_month_full[i]);
                        if (_strnicmp(s, w32_month_full[i], len) == 0) {
                            tm->tm_mon = i;
                            s += len;
                            found = 1;
                            break;
                        }
                    }
                    if (!found) return NULL;
                    break;
                }
                case '%': {
                    if (*s != '%') return NULL;
                    s++;
                    break;
                }
                default: return NULL;
            }
            fmt++;
        } else {
            if (*s != *fmt) return NULL;
            s++;
            fmt++;
        }
    }
    return (char *)s;
}

/* ── realpath ── */
static inline char *realpath(const char *path, char *resolved) {
    if (resolved) { return _fullpath(resolved, path, _MAX_PATH); }
    char buf[_MAX_PATH];
    if (!_fullpath(buf, path, sizeof(buf))) return NULL;
    return _strdup(buf);
}

/* ── mkdtemp ── */
static inline char *mkdtemp(char *tmpl) {
    size_t len = strlen(tmpl);
    if (len < 6) return NULL;
    char *suffix = tmpl + len - 6;
    for (int attempt = 0; attempt < 100; attempt++) {
        for (int i = 0; i < 6; i++) { suffix[i] = 'a' + (rand() % 26); }
        if (_mkdir(tmpl) == 0) return tmpl;
    }
    return NULL;
}

/* ── basename / dirname ── */
/* Custom implementations that handle both '/' and '\\' */
static inline char *win32_basename_buf(const char *path) {
    static char buf[_MAX_PATH];
    if (!path || !*path) {
        buf[0] = '.';
        buf[1] = '\0';
        return buf;
    }
    size_t len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) len--;
    size_t i = len;
    while (i > 0 && path[i - 1] != '/' && path[i - 1] != '\\') i--;
    size_t copy_len = len - i;
    if (copy_len >= _MAX_PATH) copy_len = _MAX_PATH - 1;
    memcpy(buf, path + i, copy_len);
    buf[copy_len] = '\0';
    return buf;
}

static inline char *win32_dirname_buf(const char *path) {
    static char buf[_MAX_PATH];
    if (!path || !*path) {
        buf[0] = '.';
        buf[1] = '\0';
        return buf;
    }
    strncpy(buf, path, _MAX_PATH - 1);
    buf[_MAX_PATH - 1] = '\0';
    size_t len = strlen(buf);
    while (len > 1 && (buf[len - 1] == '/' || buf[len - 1] == '\\')) len--;
    while (len > 0 && buf[len - 1] != '/' && buf[len - 1] != '\\') len--;
    if (len == 0) {
        buf[0] = '.';
        buf[1] = '\0';
        return buf;
    }
    if (len > 1) len--;
    buf[len] = '\0';
    return buf;
}

#define basename(p) win32_basename_buf(p)
#define dirname(p)  win32_dirname_buf(p)

/* ── Home directory ── */
static inline const char *win32_home_dir(void) {
    const char *h = getenv("HOME");
    if (h) return h;
    return getenv("USERPROFILE");
}

/*
 * ── Winsock init ──
 * Only available after <winsock2.h> has been included.
 * Source files that need this (net.c, process_ops.c) include winsock2.h themselves.
 */
#ifdef _WINSOCK2API_
static inline void win32_net_init(void) {
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = 1;
    }
}
#endif

#endif /* _WIN32 */
#endif /* LATTICE_WIN32_COMPAT_H */
