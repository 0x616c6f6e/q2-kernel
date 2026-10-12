/* SQLite still requires a registered VFS for :memory:. No host OS/filesystem is
 * imported. */
#include "sqlite3.h"
#include <string.h>
static int open_file(sqlite3_vfs *v, const char *n, sqlite3_file *f, int flags, int *out) {
    (void)v;
    (void)n;
    (void)f;
    (void)flags;
    (void)out;
    return SQLITE_CANTOPEN;
}
static int delete_file(sqlite3_vfs *v, const char *n, int sync) {
    (void)v;
    (void)n;
    (void)sync;
    return SQLITE_IOERR_DELETE;
}
static int access_file(sqlite3_vfs *v, const char *n, int flags, int *out) {
    (void)v;
    (void)n;
    (void)flags;
    *out = 0;
    return SQLITE_OK;
}
static int full_path(sqlite3_vfs *v, const char *n, int cap, char *out) {
    (void)v;
    if ((int)strlen(n) >= cap)
        return SQLITE_CANTOPEN;
    strcpy(out, n);
    return SQLITE_OK;
}
static int random_bytes(sqlite3_vfs *v, int n, char *out) {
    (void)v;
    static unsigned state = 0x51324b31;
    for (int i = 0; i < n; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        out[i] = (char)state;
    }
    return n;
}
static int sleep_us(sqlite3_vfs *v, int us) {
    (void)v;
    (void)us;
    return 0;
}
static int time_now(sqlite3_vfs *v, double *out) {
    (void)v;
    *out = 2440587.5;
    return SQLITE_OK;
}
static int last_error(sqlite3_vfs *v, int n, char *out) {
    (void)v;
    if (n)
        out[0] = 0;
    return 0;
}
int sqlite3_os_init(void) {
    static sqlite3_vfs v = {.iVersion = 1,
                            .szOsFile = sizeof(sqlite3_file),
                            .mxPathname = 256,
                            .zName = "q2-memory-only",
                            .xOpen = open_file,
                            .xDelete = delete_file,
                            .xAccess = access_file,
                            .xFullPathname = full_path,
                            .xRandomness = random_bytes,
                            .xSleep = sleep_us,
                            .xCurrentTime = time_now,
                            .xGetLastError = last_error};
    return sqlite3_vfs_register(&v, 1);
}
int sqlite3_os_end(void) {
    return SQLITE_OK;
}
