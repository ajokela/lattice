/*
 * Lattice SQLite Extension
 *
 * Provides open, close, query, exec, and status functions
 * for interacting with SQLite databases.
 */

#include "lattice_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

/* Forward declare the init function (exported symbol) */
void lat_ext_init(LatExtContext *ctx);

/* ── Connection table (indexed by integer handle) ── */

#define MAX_CONNECTIONS 256

static sqlite3 *connections[MAX_CONNECTIONS];
static int conn_count = 0;

static int conn_alloc(sqlite3 *db) {
    int i;
    for (i = 0; i < conn_count; i++) {
        if (!connections[i]) {
            connections[i] = db;
            return i;
        }
    }
    if (conn_count >= MAX_CONNECTIONS) return -1;
    connections[conn_count] = db;
    return conn_count++;
}

static sqlite3 *conn_get(int64_t id) {
    if (id < 0 || id >= conn_count) return NULL;
    return connections[id];
}

static void conn_release(int64_t id) {
    if (id >= 0 && id < conn_count) {
        connections[id] = NULL;
    }
}

/* ── Bind parameter helper ── */

static int bind_params(sqlite3_stmt *stmt, LatExtValue *params_arr) {
    size_t n = lat_ext_array_len(params_arr);
    size_t i;
    for (i = 0; i < n; i++) {
        LatExtValue *val = lat_ext_array_get(params_arr, i);
        int idx = (int)(i + 1);  /* SQLite uses 1-based indices */
        LatExtType t = lat_ext_type(val);
        switch (t) {
            case LAT_EXT_INT:
                sqlite3_bind_int64(stmt, idx, lat_ext_as_int(val));
                break;
            case LAT_EXT_FLOAT:
                sqlite3_bind_double(stmt, idx, lat_ext_as_float(val));
                break;
            case LAT_EXT_STRING:
                sqlite3_bind_text(stmt, idx, lat_ext_as_string(val), -1, SQLITE_TRANSIENT);
                break;
            case LAT_EXT_BOOL:
                sqlite3_bind_int(stmt, idx, lat_ext_as_bool(val) ? 1 : 0);
                break;
            case LAT_EXT_NIL:
            default:
                sqlite3_bind_null(stmt, idx);
                break;
        }
        lat_ext_free(val);
    }
    return 0;
}

/* ── Extension functions ── */

/* sqlite.open(path) -> Int (handle) */
static LatExtValue *sqlite_open(LatExtValue **args, size_t argc) {
    const char *path;
    sqlite3 *db;
    int rc, id;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_STRING) {
        return lat_ext_error("sqlite.open() expects a file path (String)");
    }
    path = lat_ext_as_string(args[0]);
    rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "sqlite.open: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return lat_ext_error(errbuf);
    }
    id = conn_alloc(db);
    if (id < 0) {
        sqlite3_close(db);
        return lat_ext_error("sqlite.open: too many connections");
    }
    return lat_ext_int(id);
}

/* sqlite.close(conn) -> Nil */
static LatExtValue *sqlite_close(LatExtValue **args, size_t argc) {
    int64_t id;
    sqlite3 *db;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("sqlite.close() expects a connection handle (Int)");
    }
    id = lat_ext_as_int(args[0]);
    db = conn_get(id);
    if (!db) return lat_ext_error("sqlite.close: invalid connection handle");
    sqlite3_close(db);
    conn_release(id);
    return lat_ext_nil();
}

/* sqlite.query(conn, sql) -> Array of Maps */
static LatExtValue *sqlite_query(LatExtValue **args, size_t argc) {
    int64_t id;
    sqlite3 *db;
    const char *sql;
    sqlite3_stmt *stmt;
    int rc, ncols;
    LatExtValue **rows = NULL;
    size_t row_count = 0, row_cap = 0;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("sqlite.query() expects (conn: Int, sql: String)");
    }
    id = lat_ext_as_int(args[0]);
    db = conn_get(id);
    if (!db) return lat_ext_error("sqlite.query: invalid connection handle");

    sql = lat_ext_as_string(args[1]);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "sqlite.query: %s", sqlite3_errmsg(db));
        return lat_ext_error(errbuf);
    }

    /* Bind parameters if provided */
    if (argc >= 3 && lat_ext_type(args[2]) == LAT_EXT_ARRAY) {
        bind_params(stmt, args[2]);
    }

    ncols = sqlite3_column_count(stmt);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        LatExtValue *map = lat_ext_map_new();
        int col;
        for (col = 0; col < ncols; col++) {
            const char *colname = sqlite3_column_name(stmt, col);
            int coltype = sqlite3_column_type(stmt, col);
            LatExtValue *v = NULL;

            switch (coltype) {
                case SQLITE_INTEGER: {
                    int64_t ival = sqlite3_column_int64(stmt, col);
                    v = lat_ext_int(ival);
                    break;
                }
                case SQLITE_FLOAT: {
                    double dval = sqlite3_column_double(stmt, col);
                    v = lat_ext_float(dval);
                    break;
                }
                case SQLITE_TEXT: {
                    const char *text = (const char *)sqlite3_column_text(stmt, col);
                    v = lat_ext_string(text);
                    break;
                }
                case SQLITE_NULL:
                default:
                    v = lat_ext_nil();
                    break;
            }

            lat_ext_map_set(map, colname, v);
            lat_ext_free(v);
        }

        /* Grow rows array if needed */
        if (row_count >= row_cap) {
            row_cap = row_cap ? row_cap * 2 : 16;
            rows = realloc(rows, row_cap * sizeof(LatExtValue *));
        }
        rows[row_count++] = map;
    }

    if (rc != SQLITE_DONE) {
        char errbuf[512];
        size_t i;
        snprintf(errbuf, sizeof(errbuf), "sqlite.query: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        for (i = 0; i < row_count; i++) lat_ext_free(rows[i]);
        free(rows);
        return lat_ext_error(errbuf);
    }

    sqlite3_finalize(stmt);

    LatExtValue *arr = lat_ext_array(rows, row_count);
    {
        size_t i;
        for (i = 0; i < row_count; i++) lat_ext_free(rows[i]);
    }
    free(rows);
    return arr;
}

/* sqlite_exec(conn, sql [, params]) -> Int (affected rows) */
static LatExtValue *sqlite_exec(LatExtValue **args, size_t argc) {
    int64_t id;
    sqlite3 *db;
    const char *sql;
    int rc, changes;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_INT ||
        lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("sqlite.exec() expects (conn: Int, sql: String)");
    }
    id = lat_ext_as_int(args[0]);
    db = conn_get(id);
    if (!db) return lat_ext_error("sqlite.exec: invalid connection handle");

    sql = lat_ext_as_string(args[1]);

    /* Parameterized path: prepare/bind/step/finalize */
    if (argc >= 3 && lat_ext_type(args[2]) == LAT_EXT_ARRAY) {
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            char errbuf[512];
            snprintf(errbuf, sizeof(errbuf), "sqlite.exec: %s", sqlite3_errmsg(db));
            return lat_ext_error(errbuf);
        }
        bind_params(stmt, args[2]);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            char errbuf[512];
            snprintf(errbuf, sizeof(errbuf), "sqlite.exec: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return lat_ext_error(errbuf);
        }
        sqlite3_finalize(stmt);
        changes = sqlite3_changes(db);
        return lat_ext_int(changes);
    }

    /* Non-parameterized path */
    {
        char *errmsg = NULL;
        rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            char errbuf[512];
            snprintf(errbuf, sizeof(errbuf), "sqlite.exec: %s", errmsg ? errmsg : "unknown error");
            sqlite3_free(errmsg);
            return lat_ext_error(errbuf);
        }
    }

    changes = sqlite3_changes(db);
    return lat_ext_int(changes);
}

/* sqlite.status(conn) -> String */
static LatExtValue *sqlite_status(LatExtValue **args, size_t argc) {
    int64_t id;
    sqlite3 *db;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("sqlite.status() expects a connection handle (Int)");
    }
    id = lat_ext_as_int(args[0]);
    db = conn_get(id);
    if (!db) return lat_ext_string("closed");
    return lat_ext_string("ok");
}

/* sqlite.last_insert_rowid(conn) -> Int */
static LatExtValue *sqlite_last_insert_rowid(LatExtValue **args, size_t argc) {
    int64_t id;
    sqlite3 *db;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_INT) {
        return lat_ext_error("sqlite.last_insert_rowid() expects a connection handle (Int)");
    }
    id = lat_ext_as_int(args[0]);
    db = conn_get(id);
    if (!db) return lat_ext_error("sqlite.last_insert_rowid: invalid connection handle");
    return lat_ext_int(sqlite3_last_insert_rowid(db));
}

/* ── Extension init ── */

void lat_ext_init(LatExtContext *ctx) {
    lat_ext_register(ctx, "open",               sqlite_open);
    lat_ext_register(ctx, "close",              sqlite_close);
    lat_ext_register(ctx, "query",              sqlite_query);
    lat_ext_register(ctx, "exec",               sqlite_exec);
    lat_ext_register(ctx, "status",             sqlite_status);
    lat_ext_register(ctx, "last_insert_rowid",  sqlite_last_insert_rowid);
}
