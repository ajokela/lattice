/*
 * Lattice PostgreSQL Extension
 *
 * Provides connect, close, query, exec, and status functions
 * for interacting with PostgreSQL databases via libpq.
 */

#include "lattice_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>

/* Forward declare the init function (exported symbol) */
void lat_ext_init(LatExtContext *ctx);

/* ── Connection table (indexed by integer handle) ── */

#define MAX_CONNECTIONS 256

static PGconn *connections[MAX_CONNECTIONS];
static int conn_count = 0;

static int conn_alloc(PGconn *conn) {
    int i;
    for (i = 0; i < conn_count; i++) {
        if (!connections[i]) {
            connections[i] = conn;
            return i;
        }
    }
    if (conn_count >= MAX_CONNECTIONS) return -1;
    connections[conn_count] = conn;
    return conn_count++;
}

static PGconn *conn_get(int64_t id) {
    if (id < 0 || id >= conn_count) return NULL;
    return connections[id];
}

static void conn_release(int64_t id) {
    if (id >= 0 && id < conn_count) {
        connections[id] = NULL;
    }
}

/* ── Helper: convert PGresult row to a Map ── */

static LatExtValue *row_to_map(PGresult *res, int row) {
    int ncols = PQnfields(res);
    LatExtValue *map = lat_ext_map_new();
    int col;

    for (col = 0; col < ncols; col++) {
        const char *colname = PQfname(res, col);
        const char *val;
        Oid oid;
        LatExtValue *v = NULL;

        if (PQgetisnull(res, row, col)) {
            LatExtValue *nil = lat_ext_nil();
            lat_ext_map_set(map, colname, nil);
            lat_ext_free(nil);
            continue;
        }

        val = PQgetvalue(res, row, col);
        oid = PQftype(res, col);

        switch (oid) {
            case 20:   /* INT8 */
            case 23:   /* INT4 */
            case 21:   /* INT2 */
            case 26: { /* OID */
                int64_t ival = strtoll(val, NULL, 10);
                v = lat_ext_int(ival);
                break;
            }
            case 700:  /* FLOAT4 */
            case 701: { /* FLOAT8 */
                double dval = strtod(val, NULL);
                v = lat_ext_float(dval);
                break;
            }
            case 16: { /* BOOL */
                v = lat_ext_bool(val[0] == 't' || val[0] == 'T');
                break;
            }
            default:
                v = lat_ext_string(val);
                break;
        }

        lat_ext_map_set(map, colname, v);
        lat_ext_free(v);
    }
    return map;
}

/* ── Helper: extract params from args array ── */

static char **extract_params(LatExtValue *params_val, int *nparams) {
    size_t len = lat_ext_array_len(params_val);
    size_t i;
    char **params;
    *nparams = (int)len;
    if (len == 0) return NULL;

    params = malloc(len * sizeof(char *));
    for (i = 0; i < len; i++) {
        LatExtValue *elem = lat_ext_array_get(params_val, i);
        if (!elem) {
            params[i] = NULL;
        } else if (lat_ext_type(elem) == LAT_EXT_NIL) {
            params[i] = NULL;
            lat_ext_free(elem);
        } else if (lat_ext_type(elem) == LAT_EXT_STRING) {
            params[i] = strdup(lat_ext_as_string(elem));
            lat_ext_free(elem);
        } else if (lat_ext_type(elem) == LAT_EXT_INT) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)lat_ext_as_int(elem));
            params[i] = strdup(buf);
            lat_ext_free(elem);
        } else if (lat_ext_type(elem) == LAT_EXT_FLOAT) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", lat_ext_as_float(elem));
            params[i] = strdup(buf);
            lat_ext_free(elem);
        } else if (lat_ext_type(elem) == LAT_EXT_BOOL) {
            params[i] = strdup(lat_ext_as_bool(elem) ? "t" : "f");
            lat_ext_free(elem);
        } else {
            params[i] = strdup("");
            lat_ext_free(elem);
        }
    }
    return params;
}

static void free_params(char **params, int nparams) {
    int i;
    if (!params) return;
    for (i = 0; i < nparams; i++) free(params[i]);
    free(params);
}

/* ── Extension functions ── */

/* pg.connect(connstr) -> Int (handle) */
static LatExtValue *pg_connect(LatExtValue **args, size_t argc) {
    const char *connstr;
    PGconn *conn;
    int id;
    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_STRING) {
        return lat_ext_error("pg.connect() expects a connection string");
    }
    connstr = lat_ext_as_string(args[0]);
    conn = PQconnectdb(connstr);
    if (PQstatus(conn) != CONNECTION_OK) {
        char errbuf[512];
        size_t elen;
        snprintf(errbuf, sizeof(errbuf), "pg.connect: %s", PQerrorMessage(conn));
        elen = strlen(errbuf);
        if (elen > 0 && errbuf[elen - 1] == '\n') errbuf[elen - 1] = '\0';
        PQfinish(conn);
        return lat_ext_error(errbuf);
    }
    id = conn_alloc(conn);
    if (id < 0) {
        PQfinish(conn);
        return lat_ext_error("pg.connect: too many connections");
    }
    return lat_ext_int(id);
}

/* pg.close(conn) -> Nil */
static LatExtValue *pg_close(LatExtValue **args, size_t argc) {
    int64_t id;
    PGconn *conn;
    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("pg.close() expects a connection handle (Int)");
    }
    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("pg.close: invalid connection handle");
    PQfinish(conn);
    conn_release(id);
    return lat_ext_nil();
}

/* pg.query(conn, sql, params?) -> Array of Maps */
static LatExtValue *pg_query(LatExtValue **args, size_t argc) {
    int64_t id;
    PGconn *conn;
    const char *sql;
    PGresult *res;
    int nrows, i;
    LatExtValue **rows;
    LatExtValue *arr;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("pg.query() expects (conn: Int, sql: String, params?: Array)");
    }
    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("pg.query: invalid connection handle");

    sql = lat_ext_as_string(args[1]);

    if (argc >= 3 && lat_ext_type(args[2]) == LAT_EXT_ARRAY) {
        int nparams = 0;
        char **params = extract_params(args[2], &nparams);
        res = PQexecParams(conn, sql, nparams,
                          NULL, (const char *const *)params, NULL, NULL, 0);
        free_params(params, nparams);
    } else {
        res = PQexec(conn, sql);
    }

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        char errbuf[512];
        size_t elen;
        snprintf(errbuf, sizeof(errbuf), "pg.query: %s", PQresultErrorMessage(res));
        elen = strlen(errbuf);
        if (elen > 0 && errbuf[elen - 1] == '\n') errbuf[elen - 1] = '\0';
        PQclear(res);
        return lat_ext_error(errbuf);
    }

    nrows = PQntuples(res);
    rows = malloc((size_t)nrows * sizeof(LatExtValue *));
    for (i = 0; i < nrows; i++) {
        rows[i] = row_to_map(res, i);
    }

    arr = lat_ext_array(rows, (size_t)nrows);
    for (i = 0; i < nrows; i++) lat_ext_free(rows[i]);
    free(rows);
    PQclear(res);
    return arr;
}

/* pg.run(conn, sql, params?) -> Int (affected rows) */
static LatExtValue *pg_run(LatExtValue **args, size_t argc) {
    int64_t id;
    PGconn *conn;
    const char *sql;
    PGresult *res;
    ExecStatusType status;
    const char *affected;
    int64_t count;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("pg.exec() expects (conn: Int, sql: String, params?: Array)");
    }
    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_error("pg.exec: invalid connection handle");

    sql = lat_ext_as_string(args[1]);

    if (argc >= 3 && lat_ext_type(args[2]) == LAT_EXT_ARRAY) {
        int nparams = 0;
        char **params = extract_params(args[2], &nparams);
        res = PQexecParams(conn, sql, nparams,
                          NULL, (const char *const *)params, NULL, NULL, 0);
        free_params(params, nparams);
    } else {
        res = PQexec(conn, sql);
    }

    status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        char errbuf[512];
        size_t elen;
        snprintf(errbuf, sizeof(errbuf), "pg.exec: %s", PQresultErrorMessage(res));
        elen = strlen(errbuf);
        if (elen > 0 && errbuf[elen - 1] == '\n') errbuf[elen - 1] = '\0';
        PQclear(res);
        return lat_ext_error(errbuf);
    }

    affected = PQcmdTuples(res);
    count = 0;
    if (affected && affected[0]) count = strtoll(affected, NULL, 10);
    PQclear(res);
    return lat_ext_int(count);
}

/* pg.status(conn) -> String */
static LatExtValue *pg_status(LatExtValue **args, size_t argc) {
    int64_t id;
    PGconn *conn;
    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("pg.status() expects a connection handle (Int)");
    }
    id = lat_ext_as_int(args[0]);
    conn = conn_get(id);
    if (!conn) return lat_ext_string("invalid");

    switch (PQstatus(conn)) {
        case CONNECTION_OK:               return lat_ext_string("ok");
        case CONNECTION_BAD:              return lat_ext_string("bad");
        default:                          return lat_ext_string("unknown");
    }
}

/* ── Extension init ── */

void lat_ext_init(LatExtContext *ctx) {
    lat_ext_register(ctx, "connect", pg_connect);
    lat_ext_register(ctx, "close",   pg_close);
    lat_ext_register(ctx, "query",   pg_query);
    lat_ext_register(ctx, "exec",    pg_run);
    lat_ext_register(ctx, "status",  pg_status);
}
