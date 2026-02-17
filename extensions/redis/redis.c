/*
 * Lattice Redis Extension
 *
 * Minimal Redis client using raw TCP sockets and the RESP protocol.
 * No external dependencies (no hiredis).
 *
 * Provides: connect, close, command, get, set, del, exists, expire,
 *           keys, incr, lpush, lrange, publish, ping
 */

#include "lattice_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

/* Forward declare the init function (exported symbol) */
void lat_ext_init(LatExtContext *ctx);

/* ── Connection table (indexed by integer handle) ── */

#define MAX_CONNECTIONS 256
#define RESP_BUF_SIZE   65536
#define DEFAULT_PORT    6379

static int connections[MAX_CONNECTIONS];
static int conn_count = 0;

static int conn_alloc(int fd) {
    int i;
    for (i = 0; i < conn_count; i++) {
        if (connections[i] < 0) {
            connections[i] = fd;
            return i;
        }
    }
    if (conn_count >= MAX_CONNECTIONS) return -1;
    connections[conn_count] = fd;
    return conn_count++;
}

static int conn_get(int64_t id) {
    if (id < 0 || id >= conn_count) return -1;
    return connections[id];
}

static void conn_release(int64_t id) {
    if (id >= 0 && id < conn_count) {
        connections[id] = -1;
    }
}

/* ── Dynamic buffer for building RESP commands and reading responses ── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} RespBuf;

static void buf_init(RespBuf *b) {
    b->cap  = 256;
    b->len  = 0;
    b->data = malloc(b->cap);
}

static void buf_free(RespBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void buf_ensure(RespBuf *b, size_t extra) {
    while (b->len + extra > b->cap) {
        b->cap *= 2;
        b->data = realloc(b->data, b->cap);
    }
}

static void buf_append(RespBuf *b, const char *s, size_t n) {
    buf_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void buf_append_str(RespBuf *b, const char *s) {
    buf_append(b, s, strlen(s));
}

static void buf_append_crlf(RespBuf *b) {
    buf_append(b, "\r\n", 2);
}

/* ── RESP protocol: build command ── */

/*
 * Build a RESP array command: *N\r\n$len\r\narg\r\n...
 * args is an array of C strings, argc is the count.
 */
static void resp_build_command(RespBuf *out, const char **args, size_t argc) {
    char numbuf[32];
    size_t i;

    buf_init(out);

    /* *N\r\n */
    snprintf(numbuf, sizeof(numbuf), "*%zu", argc);
    buf_append_str(out, numbuf);
    buf_append_crlf(out);

    for (i = 0; i < argc; i++) {
        size_t slen = strlen(args[i]);
        snprintf(numbuf, sizeof(numbuf), "$%zu", slen);
        buf_append_str(out, numbuf);
        buf_append_crlf(out);
        buf_append(out, args[i], slen);
        buf_append_crlf(out);
    }
}

/* ── RESP protocol: send command ── */

static int resp_send(int fd, const RespBuf *cmd) {
    size_t sent = 0;
    while (sent < cmd->len) {
        ssize_t n = write(fd, cmd->data + sent, cmd->len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* ── RESP protocol: read response ── */

/*
 * Buffered reader over a socket fd.
 * We keep a read-ahead buffer and parse RESP incrementally.
 */
typedef struct {
    int    fd;
    char   buf[RESP_BUF_SIZE];
    size_t pos;
    size_t len;
} RespReader;

static void reader_init(RespReader *r, int fd) {
    r->fd  = fd;
    r->pos = 0;
    r->len = 0;
}

/* Read more data into the buffer if needed. Returns 0 on success, -1 on error. */
static int reader_fill(RespReader *r) {
    ssize_t n;
    if (r->pos > 0 && r->pos < r->len) {
        /* Shift remaining data to front */
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
        r->len -= r->pos;
        r->pos = 0;
    } else if (r->pos >= r->len) {
        r->pos = 0;
        r->len = 0;
    }
    n = read(r->fd, r->buf + r->len, RESP_BUF_SIZE - r->len);
    if (n <= 0) return -1;
    r->len += (size_t)n;
    return 0;
}

/* Read a single byte. Returns 0 on success, -1 on EOF/error. */
static int reader_byte(RespReader *r, char *out) {
    if (r->pos >= r->len) {
        if (reader_fill(r) < 0) return -1;
    }
    *out = r->buf[r->pos++];
    return 0;
}

/*
 * Read a line terminated by \r\n.
 * Returns a malloc'd string WITHOUT the \r\n, or NULL on error.
 */
static char *reader_line(RespReader *r) {
    RespBuf line;
    char c;

    buf_init(&line);
    for (;;) {
        if (reader_byte(r, &c) < 0) {
            buf_free(&line);
            return NULL;
        }
        if (c == '\r') {
            /* Expect \n next */
            if (reader_byte(r, &c) < 0 || c != '\n') {
                buf_free(&line);
                return NULL;
            }
            /* Null-terminate */
            buf_ensure(&line, 1);
            line.data[line.len] = '\0';
            return line.data;
        }
        buf_append(&line, &c, 1);
    }
}

/*
 * Read exactly n bytes from the reader. Returns malloc'd buffer or NULL.
 */
static char *reader_bytes(RespReader *r, size_t n) {
    char *out = malloc(n + 1);
    size_t got = 0;
    if (!out) return NULL;

    while (got < n) {
        /* Use buffered data first */
        size_t avail = r->len - r->pos;
        if (avail > 0) {
            size_t take = (avail < n - got) ? avail : (n - got);
            memcpy(out + got, r->buf + r->pos, take);
            r->pos += take;
            got += take;
        } else {
            if (reader_fill(r) < 0) {
                free(out);
                return NULL;
            }
        }
    }
    out[n] = '\0';
    return out;
}

/* Forward declaration */
static LatExtValue *resp_read_value(RespReader *r);

/*
 * Parse a complete RESP value from the reader.
 * Returns a LatExtValue, or an error value on parse failure.
 */
static LatExtValue *resp_read_value(RespReader *r) {
    char *line = reader_line(r);
    if (!line) return lat_ext_error("redis: failed to read response");

    char type = line[0];
    char *payload = line + 1;

    switch (type) {
        case '+': {
            /* Simple String: +OK\r\n */
            LatExtValue *val = lat_ext_string(payload);
            free(line);
            return val;
        }
        case '-': {
            /* Error: -ERR message\r\n */
            char errbuf[512];
            snprintf(errbuf, sizeof(errbuf), "redis: %s", payload);
            free(line);
            return lat_ext_error(errbuf);
        }
        case ':': {
            /* Integer: :42\r\n */
            int64_t ival = strtoll(payload, NULL, 10);
            free(line);
            return lat_ext_int(ival);
        }
        case '$': {
            /* Bulk String: $N\r\n<data>\r\n  or  $-1\r\n (nil) */
            int64_t blen = strtoll(payload, NULL, 10);
            free(line);
            if (blen < 0) return lat_ext_nil();

            char *data = reader_bytes(r, (size_t)blen);
            if (!data) return lat_ext_error("redis: failed to read bulk string");

            /* Consume trailing \r\n */
            char cr, lf;
            if (reader_byte(r, &cr) < 0 || reader_byte(r, &lf) < 0) {
                free(data);
                return lat_ext_error("redis: missing CRLF after bulk string");
            }

            LatExtValue *val = lat_ext_string(data);
            free(data);
            return val;
        }
        case '*': {
            /* Array: *N\r\n<elements...>  or  *-1\r\n (nil) */
            int64_t count = strtoll(payload, NULL, 10);
            free(line);
            if (count < 0) return lat_ext_nil();

            LatExtValue **elems = NULL;
            if (count > 0) {
                elems = malloc((size_t)count * sizeof(LatExtValue *));
                if (!elems) return lat_ext_error("redis: out of memory");
            }

            int64_t i;
            for (i = 0; i < count; i++) {
                elems[i] = resp_read_value(r);
            }

            LatExtValue *arr = lat_ext_array(elems, (size_t)count);
            for (i = 0; i < count; i++) {
                lat_ext_free(elems[i]);
            }
            free(elems);
            return arr;
        }
        default: {
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf),
                     "redis: unknown RESP type '%c'", type);
            free(line);
            return lat_ext_error(errbuf);
        }
    }
}

/* ── Helper: send a command and get response ── */

static LatExtValue *redis_send_command(int fd, const char **argv, size_t argc) {
    RespBuf cmd;
    RespReader reader;

    resp_build_command(&cmd, argv, argc);
    if (resp_send(fd, &cmd) < 0) {
        buf_free(&cmd);
        return lat_ext_error("redis: failed to send command");
    }
    buf_free(&cmd);

    reader_init(&reader, fd);
    return resp_read_value(&reader);
}

/* ── Helper: convert a LatExtValue arg to a C string ── */

/*
 * Convert any scalar arg to a string representation for Redis.
 * For strings, returns the string directly (caller must NOT free).
 * For ints/floats, writes into the provided buffer and returns it.
 */
static const char *arg_to_string(LatExtValue *v, char *buf, size_t bufsz) {
    LatExtType t = lat_ext_type(v);
    switch (t) {
        case LAT_EXT_STRING:
            return lat_ext_as_string(v);
        case LAT_EXT_INT:
            snprintf(buf, bufsz, "%lld", (long long)lat_ext_as_int(v));
            return buf;
        case LAT_EXT_FLOAT:
            snprintf(buf, bufsz, "%g", lat_ext_as_float(v));
            return buf;
        case LAT_EXT_BOOL:
            return lat_ext_as_bool(v) ? "1" : "0";
        default:
            return "";
    }
}

/* ── Extension functions ── */

/* redis.connect(host, port?) -> Int (handle) */
static LatExtValue *redis_connect(LatExtValue **args, size_t argc) {
    const char *host;
    int port = DEFAULT_PORT;
    int fd, rc;
    struct addrinfo hints, *res, *rp;
    char portstr[16];

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_STRING) {
        return lat_ext_error("redis.connect() expects (host: String [, port: Int])");
    }
    host = lat_ext_as_string(args[0]);

    if (argc >= 2 && lat_ext_type(args[1]) == LAT_EXT_INT) {
        port = (int)lat_ext_as_int(args[1]);
    }

    snprintf(portstr, sizeof(portstr), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
                 "redis.connect: %s", gai_strerror(rc));
        return lat_ext_error(errbuf);
    }

    fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
                 "redis.connect: unable to connect to %s:%d", host, port);
        return lat_ext_error(errbuf);
    }

    /* Disable Nagle for lower latency */
    {
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    }

    int id = conn_alloc(fd);
    if (id < 0) {
        close(fd);
        return lat_ext_error("redis.connect: too many connections");
    }
    return lat_ext_int(id);
}

/* redis.close(handle) -> Nil */
static LatExtValue *redis_close(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("redis.close() expects a connection handle (Int)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.close: invalid connection handle");

    close(fd);
    conn_release(id);
    return lat_ext_nil();
}

/* redis.command(handle, arg1, arg2, ...) -> result
 * Send an arbitrary Redis command. */
static LatExtValue *redis_command(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;
    size_t i, cmd_argc;
    const char **argv = NULL;
    char **numbufs = NULL;
    LatExtValue *result;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("redis.command() expects (handle: Int, cmd: String, ...)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.command: invalid connection handle");

    cmd_argc = argc - 1;
    argv    = malloc(cmd_argc * sizeof(const char *));
    numbufs = calloc(cmd_argc, sizeof(char *));

    for (i = 0; i < cmd_argc; i++) {
        LatExtType t = lat_ext_type(args[i + 1]);
        if (t == LAT_EXT_STRING) {
            argv[i] = lat_ext_as_string(args[i + 1]);
        } else {
            numbufs[i] = malloc(64);
            arg_to_string(args[i + 1], numbufs[i], 64);
            argv[i] = numbufs[i];
        }
    }

    result = redis_send_command(fd, argv, cmd_argc);

    for (i = 0; i < cmd_argc; i++) {
        free(numbufs[i]);
    }
    free(numbufs);
    free(argv);
    return result;
}

/* redis.get(handle, key) -> String or Nil */
static LatExtValue *redis_get(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("redis.get() expects (handle: Int, key: String)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.get: invalid connection handle");

    const char *argv[] = { "GET", lat_ext_as_string(args[1]) };
    return redis_send_command(fd, argv, 2);
}

/* redis.set(handle, key, value) -> String "OK" */
static LatExtValue *redis_set(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;
    char vbuf[64];

    if (argc < 3 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("redis.set() expects (handle: Int, key: String, value)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.set: invalid connection handle");

    const char *val = arg_to_string(args[2], vbuf, sizeof(vbuf));
    const char *argv[] = { "SET", lat_ext_as_string(args[1]), val };
    return redis_send_command(fd, argv, 3);
}

/* redis.del(handle, key) -> Int (number of keys deleted) */
static LatExtValue *redis_del(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("redis.del() expects (handle: Int, key: String)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.del: invalid connection handle");

    const char *argv[] = { "DEL", lat_ext_as_string(args[1]) };
    return redis_send_command(fd, argv, 2);
}

/* redis.exists(handle, key) -> Bool */
static LatExtValue *redis_exists(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;
    LatExtValue *result;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("redis.exists() expects (handle: Int, key: String)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.exists: invalid connection handle");

    const char *argv[] = { "EXISTS", lat_ext_as_string(args[1]) };
    result = redis_send_command(fd, argv, 2);

    /* EXISTS returns integer 0 or 1; convert to Bool */
    if (lat_ext_type(result) == LAT_EXT_INT) {
        bool exists = (lat_ext_as_int(result) > 0);
        lat_ext_free(result);
        return lat_ext_bool(exists);
    }
    return result; /* error passthrough */
}

/* redis.expire(handle, key, seconds) -> Bool */
static LatExtValue *redis_expire(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;
    char secbuf[32];
    LatExtValue *result;

    if (argc < 3 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING ||
        lat_ext_type(args[2]) != LAT_EXT_INT) {
        return lat_ext_error("redis.expire() expects (handle: Int, key: String, seconds: Int)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.expire: invalid connection handle");

    snprintf(secbuf, sizeof(secbuf), "%lld", (long long)lat_ext_as_int(args[2]));
    const char *argv[] = { "EXPIRE", lat_ext_as_string(args[1]), secbuf };
    result = redis_send_command(fd, argv, 3);

    /* EXPIRE returns integer 0 or 1; convert to Bool */
    if (lat_ext_type(result) == LAT_EXT_INT) {
        bool ok = (lat_ext_as_int(result) > 0);
        lat_ext_free(result);
        return lat_ext_bool(ok);
    }
    return result;
}

/* redis.keys(handle, pattern) -> Array of Strings */
static LatExtValue *redis_keys(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("redis.keys() expects (handle: Int, pattern: String)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.keys: invalid connection handle");

    const char *argv[] = { "KEYS", lat_ext_as_string(args[1]) };
    return redis_send_command(fd, argv, 2);
}

/* redis.incr(handle, key) -> Int */
static LatExtValue *redis_incr(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("redis.incr() expects (handle: Int, key: String)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.incr: invalid connection handle");

    const char *argv[] = { "INCR", lat_ext_as_string(args[1]) };
    return redis_send_command(fd, argv, 2);
}

/* redis.lpush(handle, key, value) -> Int (list length) */
static LatExtValue *redis_lpush(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;
    char vbuf[64];

    if (argc < 3 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("redis.lpush() expects (handle: Int, key: String, value)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.lpush: invalid connection handle");

    const char *val = arg_to_string(args[2], vbuf, sizeof(vbuf));
    const char *argv[] = { "LPUSH", lat_ext_as_string(args[1]), val };
    return redis_send_command(fd, argv, 3);
}

/* redis.lrange(handle, key, start, stop) -> Array of Strings */
static LatExtValue *redis_lrange(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;
    char startbuf[32], stopbuf[32];

    if (argc < 4 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING ||
        lat_ext_type(args[2]) != LAT_EXT_INT ||
        lat_ext_type(args[3]) != LAT_EXT_INT) {
        return lat_ext_error("redis.lrange() expects (handle: Int, key: String, start: Int, stop: Int)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.lrange: invalid connection handle");

    snprintf(startbuf, sizeof(startbuf), "%lld", (long long)lat_ext_as_int(args[2]));
    snprintf(stopbuf, sizeof(stopbuf), "%lld", (long long)lat_ext_as_int(args[3]));
    const char *argv[] = { "LRANGE", lat_ext_as_string(args[1]), startbuf, stopbuf };
    return redis_send_command(fd, argv, 4);
}

/* redis.publish(handle, channel, message) -> Int (receivers) */
static LatExtValue *redis_publish(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;

    if (argc < 3 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING ||
        lat_ext_type(args[2]) != LAT_EXT_STRING) {
        return lat_ext_error("redis.publish() expects (handle: Int, channel: String, message: String)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.publish: invalid connection handle");

    const char *argv[] = { "PUBLISH", lat_ext_as_string(args[1]), lat_ext_as_string(args[2]) };
    return redis_send_command(fd, argv, 3);
}

/* redis.ping(handle) -> String "PONG" */
static LatExtValue *redis_ping(LatExtValue **args, size_t argc) {
    int64_t id;
    int fd;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("redis.ping() expects a connection handle (Int)");
    }
    id = lat_ext_as_int(args[0]);
    fd = conn_get(id);
    if (fd < 0) return lat_ext_error("redis.ping: invalid connection handle");

    const char *argv[] = { "PING" };
    return redis_send_command(fd, argv, 1);
}

/* ── Extension init ── */

void lat_ext_init(LatExtContext *ctx) {
    /* Initialize connection table */
    int i;
    for (i = 0; i < MAX_CONNECTIONS; i++) {
        connections[i] = -1;
    }

    lat_ext_register(ctx, "connect",  redis_connect);
    lat_ext_register(ctx, "close",    redis_close);
    lat_ext_register(ctx, "command",  redis_command);
    lat_ext_register(ctx, "get",      redis_get);
    lat_ext_register(ctx, "set",      redis_set);
    lat_ext_register(ctx, "del",      redis_del);
    lat_ext_register(ctx, "exists",   redis_exists);
    lat_ext_register(ctx, "expire",   redis_expire);
    lat_ext_register(ctx, "keys",     redis_keys);
    lat_ext_register(ctx, "incr",     redis_incr);
    lat_ext_register(ctx, "lpush",    redis_lpush);
    lat_ext_register(ctx, "lrange",   redis_lrange);
    lat_ext_register(ctx, "publish",  redis_publish);
    lat_ext_register(ctx, "ping",     redis_ping);
}
