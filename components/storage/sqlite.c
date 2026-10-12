#include "q2/sqlite.h"
#include "sqlite3.h"
#include <stdlib.h>
#include <string.h>
struct q2_sqlite {
    sqlite3 *db;
    q2_component_t component;
};
static q2_result_t map(int e) {
    if (e == SQLITE_OK || e == SQLITE_DONE || e == SQLITE_ROW)
        return Q2_OK;
    if (e == SQLITE_NOMEM)
        return Q2_ERR_MEMORY;
    if (e == SQLITE_FULL || e == SQLITE_TOOBIG)
        return Q2_ERR_LIMIT;
    return Q2_ERR_IO;
}
static q2_result_t start(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_sqlite_t *s = ctx;
    int e = sqlite3_open_v2(":memory:", &s->db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY, NULL);
    if (e == SQLITE_OK)
        e = sqlite3_exec(s->db,
                         "PRAGMA max_page_count=64; CREATE TABLE kv(app TEXT,key "
                         "TEXT,value TEXT,PRIMARY KEY(app,key));",
                         NULL, NULL, NULL);
    if (e != SQLITE_OK) {
        sqlite3_close(s->db);
        s->db = NULL;
    } else {
        sqlite3_limit(s->db, SQLITE_LIMIT_LENGTH, 4096);
        sqlite3_limit(s->db, SQLITE_LIMIT_SQL_LENGTH, 1024);
    }
    return map(e);
}
static q2_result_t stop(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_sqlite_t *s = ctx;
    if (!s->db)
        return Q2_OK;
    int e = sqlite3_close(s->db);
    if (e == SQLITE_OK)
        s->db = NULL;
    return map(e);
}
static q2_result_t access_db(q2_kernel_t *k, void *ctx, const q2_caller_t *c, const q2_value_t *a, size_t n,
                             q2_value_t *out, bool write) {
    (void)k;
    q2_sqlite_t *s = ctx;
    if (n != (write ? 2u : 1u) || a[0].kind != Q2_STRING || (write && a[1].kind != Q2_STRING) ||
        !*a[0].as.string || strlen(a[0].as.string) > 32)
        return Q2_ERR_ARGUMENT;
    /* Match the flat-key contract used by both filesystem providers. */
    for (const char *p = a[0].as.string; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return Q2_ERR_ARGUMENT;
    const char *sql = write ? "INSERT INTO kv(app,key,value) SELECT ?1,?2,?3 WHERE "
                              "(SELECT COUNT(*) FROM kv WHERE app=?1)<16 OR "
                              "EXISTS(SELECT 1 FROM kv WHERE app=?1 AND key=?2) ON "
                              "CONFLICT(app,key) DO UPDATE SET value=excluded.value"
                            : "SELECT value FROM kv WHERE app=?1 AND key=?2";
    sqlite3_stmt *stmt = NULL;
    int e = sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL);
    if (e == SQLITE_OK)
        e = sqlite3_bind_text(stmt, 1, c->app_id, -1, SQLITE_TRANSIENT);
    if (e == SQLITE_OK)
        e = sqlite3_bind_text(stmt, 2, a[0].as.string, -1, SQLITE_TRANSIENT);
    if (e == SQLITE_OK && write)
        e = sqlite3_bind_text(stmt, 3, a[1].as.string, -1, SQLITE_TRANSIENT);
    if (e == SQLITE_OK)
        e = sqlite3_step(stmt);
    q2_result_t result = map(e);
    if (!write && e == SQLITE_DONE)
        result = Q2_ERR_NOT_FOUND;
    if (write && e == SQLITE_DONE && sqlite3_changes(s->db) == 0)
        result = Q2_ERR_LIMIT;
    if (!write && e == SQLITE_ROW) {
        int bytes = sqlite3_column_bytes(stmt, 0);
        const unsigned char *value = sqlite3_column_text(stmt, 0);
        if (!value)
            result = Q2_ERR_MEMORY;
        else if (bytes >= (int)Q2_TEXT_MAX)
            result = Q2_ERR_LIMIT;
        else {
            memcpy(out->as.string, value, (size_t)bytes);
            out->as.string[bytes] = 0;
            out->kind = Q2_STRING;
        }
    }
    int close = sqlite3_finalize(stmt);
    return result == Q2_OK ? map(close) : result;
}
static q2_result_t get(q2_kernel_t *k, void *s, const q2_caller_t *c, const q2_value_t *a, size_t n,
                       q2_value_t *o) {
    return access_db(k, s, c, a, n, o, false);
}
static q2_result_t put(q2_kernel_t *k, void *s, const q2_caller_t *c, const q2_value_t *a, size_t n,
                       q2_value_t *o) {
    return access_db(k, s, c, a, n, o, true);
}
static const q2_method_t methods[] = {{"sqlite", "get", "storage.database", get},
                                      {"sqlite", "put", "storage.database", put}};
q2_result_t q2_sqlite_create(q2_sqlite_t **out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    *out = calloc(1, sizeof(**out));
    if (!*out)
        return Q2_ERR_MEMORY;
    (*out)->component = (q2_component_t){.abi = 1,
                                         .size = sizeof(q2_component_t),
                                         .id = "storage_sqlite",
                                         .context = *out,
                                         .start = start,
                                         .stop = stop,
                                         .methods = methods,
                                         .method_count = 2};
    return Q2_OK;
}
q2_result_t q2_sqlite_component(q2_sqlite_t *s, const q2_component_t **out) {
    if (!s || !out)
        return Q2_ERR_ARGUMENT;
    *out = &s->component;
    return Q2_OK;
}
q2_result_t q2_sqlite_destroy(q2_sqlite_t **s) {
    if (!s || !*s)
        return Q2_ERR_ARGUMENT;
    if ((*s)->db)
        return Q2_ERR_BUSY;
    free(*s);
    *s = NULL;
    return Q2_OK;
}
