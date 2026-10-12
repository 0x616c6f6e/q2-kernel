#include "storage_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int read_block(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, void *data, lfs_size_t n) {
    q2_storage_t *s = c->context;
    return s->block.read(s->block.context, (uint64_t)b * c->block_size + o, data, n) == Q2_OK ? 0
                                                                                              : LFS_ERR_IO;
}
static int program_block(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, const void *data,
                         lfs_size_t n) {
    q2_storage_t *s = c->context;
    return s->block.program(s->block.context, (uint64_t)b * c->block_size + o, data, n) == Q2_OK ? 0
                                                                                                 : LFS_ERR_IO;
}
static int erase_block(const struct lfs_config *c, lfs_block_t b) {
    q2_storage_t *s = c->context;
    return s->block.erase(s->block.context, (uint64_t)b * c->block_size, c->block_size) == Q2_OK ? 0
                                                                                                 : LFS_ERR_IO;
}
static int sync_block(const struct lfs_config *c) {
    q2_storage_t *s = c->context;
    return s->block.sync(s->block.context) == Q2_OK ? 0 : LFS_ERR_IO;
}
static q2_result_t map_lfs(int e) {
    if (e >= 0)
        return Q2_OK;
    if (e == LFS_ERR_NOENT)
        return Q2_ERR_NOT_FOUND;
    if (e == LFS_ERR_NOSPC || e == LFS_ERR_FBIG)
        return Q2_ERR_LIMIT;
    if (e == LFS_ERR_NOMEM)
        return Q2_ERR_MEMORY;
    if (e == LFS_ERR_CORRUPT)
        return Q2_ERR_FORMAT;
    return Q2_ERR_IO;
}
static q2_result_t mount(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_storage_t *s = ctx;
    q2_result_t e = s->fat ? q2_fatfs_mount(s) : map_lfs(lfs_mount(&s->lfs, &s->config));
    s->mounted = e == Q2_OK;
    return e;
}
static q2_result_t unmount(q2_kernel_t *k, void *ctx) {
    (void)k;
    q2_storage_t *s = ctx;
    if (!s->mounted)
        return Q2_OK;
    q2_result_t e = s->fat ? q2_fatfs_unmount(s) : map_lfs(lfs_unmount(&s->lfs));
    if (e == Q2_OK)
        s->mounted = false;
    return e;
}
static q2_result_t access_file(q2_kernel_t *k, void *ctx, const q2_caller_t *caller, const q2_value_t *args,
                               size_t n, q2_value_t *out, bool write) {
    (void)k;
    q2_storage_t *s = ctx;
    if (n != (write ? 2u : 1u) || args[0].kind != Q2_STRING || (write && args[1].kind != Q2_STRING))
        return Q2_ERR_ARGUMENT;
    const char *key = args[0].as.string;
    if (!*key || strlen(key) > 32 || strlen(caller->app_id) != 36)
        return Q2_ERR_ARGUMENT;
    /* Flat keys and a runtime-issued UUID prevent arbitrary filesystem traversal.
     */
    for (const char *p = key; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return Q2_ERR_ARGUMENT;
    for (const char *p = caller->app_id; *p; p++)
        if (!((*p >= 'a' && *p <= 'f') || (*p >= '0' && *p <= '9') || *p == '-'))
            return Q2_ERR_ARGUMENT;
    if (s->fat)
        return q2_fatfs_access(s, caller, args, n, out, write);
    char dir[40], path[80], temp[80];
    snprintf(dir, sizeof(dir), "/%s", caller->app_id);
    snprintf(path, sizeof(path), "%.39s/%.32s", dir, key);
    snprintf(temp, sizeof(temp), "%s/.pending", dir);
    if (write) {
        int e = lfs_mkdir(&s->lfs, dir);
        if (e < 0 && e != LFS_ERR_EXIST)
            return map_lfs(e);
        lfs_dir_t d;
        e = lfs_dir_open(&s->lfs, &d, dir);
        if (e < 0)
            return map_lfs(e);
        unsigned files = 0;
        struct lfs_info info;
        while ((e = lfs_dir_read(&s->lfs, &d, &info)) > 0)
            if (info.name[0] != '.')
                files++;
        int close = lfs_dir_close(&s->lfs, &d);
        if (e < 0 || close < 0)
            return Q2_ERR_IO;
        if (lfs_stat(&s->lfs, path, &info) == LFS_ERR_NOENT && files >= Q2_STORAGE_PRIVATE_KEYS)
            return Q2_ERR_LIMIT;
    }
    lfs_file_t f;
    int e = lfs_file_open(&s->lfs, &f, write ? temp : path,
                          write ? (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) : LFS_O_RDONLY);
    if (e < 0)
        return map_lfs(e);
    if (write) {
        size_t len = strlen(args[1].as.string);
        e = lfs_file_write(&s->lfs, &f, args[1].as.string, (lfs_size_t)len);
        if (e >= 0 && (size_t)e != len)
            e = LFS_ERR_IO;
    } else {
        lfs_soff_t size = lfs_file_size(&s->lfs, &f);
        if (size < 0 || size >= (lfs_soff_t)Q2_TEXT_MAX)
            e = LFS_ERR_FBIG;
        else {
            e = lfs_file_read(&s->lfs, &f, out->as.string, (lfs_size_t)size);
            if (e != size)
                e = LFS_ERR_IO;
            else {
                out->as.string[size] = 0;
                out->kind = Q2_STRING;
            }
        }
    }
    int close = lfs_file_close(&s->lfs, &f);
    if (e < 0 || close < 0)
        return map_lfs(e < 0 ? e : close);
    /* LittleFS rename commits the new complete value after close/sync. */
    return write ? map_lfs(lfs_rename(&s->lfs, temp, path)) : Q2_OK;
}
static q2_result_t get(q2_kernel_t *k, void *s, const q2_caller_t *c, const q2_value_t *a, size_t n,
                       q2_value_t *o) {
    return access_file(k, s, c, a, n, o, false);
}
static q2_result_t put(q2_kernel_t *k, void *s, const q2_caller_t *c, const q2_value_t *a, size_t n,
                       q2_value_t *o) {
    return access_file(k, s, c, a, n, o, true);
}
static const q2_method_t lfs_methods[] = {{"storage", "get", "storage.private", get},
                                          {"storage", "put", "storage.private", put}};
static const q2_method_t fat_methods[] = {{"fatfs", "get", "storage.fatfs", get},
                                          {"fatfs", "put", "storage.fatfs", put}};
q2_result_t q2_storage_new(q2_kernel_t *k, const q2_block_device_t *b, bool fat, q2_storage_t **out) {
    if (!out)
        return Q2_ERR_ARGUMENT;
    *out = NULL;
    if (!k || !b || b->abi != 1 || b->size < sizeof(*b) || !b->read || !b->program || !b->sync ||
        !b->block_count || !b->block_size)
        return Q2_ERR_ARGUMENT;
    if (!fat && (!b->erase || !b->read_size || !b->program_size || b->block_size < 256 ||
                 b->block_count < 8 || 256 % b->read_size || 256 % b->program_size || b->block_size % 256))
        return Q2_ERR_ARGUMENT;
    if (fat && (b->block_size != 512 || b->block_count < 128))
        return Q2_ERR_ARGUMENT;
    q2_storage_t *s = calloc(1, sizeof(*s));
    if (!s)
        return Q2_ERR_MEMORY;
    s->kernel = k;
    s->block = *b;
    s->fat = fat;
    s->config = (struct lfs_config){.context = s,
                                    .read = read_block,
                                    .prog = program_block,
                                    .erase = erase_block,
                                    .sync = sync_block,
                                    .read_size = b->read_size,
                                    .prog_size = b->program_size,
                                    .block_size = b->block_size,
                                    .block_count = b->block_count,
                                    .block_cycles = 100,
                                    .cache_size = 256,
                                    .lookahead_size = 32,
                                    .name_max = 64,
                                    .file_max = 255};
    s->component = (q2_component_t){.abi = 1,
                                    .size = sizeof(q2_component_t),
                                    .id = fat ? "storage_fatfs" : "storage_littlefs",
                                    .context = s,
                                    .start = mount,
                                    .stop = unmount,
                                    .methods = fat ? fat_methods : lfs_methods,
                                    .method_count = 2};
    *out = s;
    return Q2_OK;
}
q2_result_t q2_littlefs_create(q2_kernel_t *k, const q2_block_device_t *b, q2_storage_t **out) {
    return q2_storage_new(k, b, false, out);
}
q2_result_t q2_fatfs_create(q2_kernel_t *k, const q2_block_device_t *b, q2_storage_t **out) {
    return q2_storage_new(k, b, true, out);
}
q2_result_t q2_storage_format(q2_storage_t *s) {
    if (!s)
        return Q2_ERR_ARGUMENT;
    if (s->mounted)
        return Q2_ERR_BUSY;
    return s->fat ? q2_fatfs_format(s) : map_lfs(lfs_format(&s->lfs, &s->config));
}
q2_result_t q2_storage_component(q2_storage_t *s, const q2_component_t **out) {
    if (!s || !out)
        return Q2_ERR_ARGUMENT;
    *out = &s->component;
    return Q2_OK;
}
q2_result_t q2_storage_destroy(q2_storage_t **ref) {
    if (!ref || !*ref)
        return Q2_ERR_ARGUMENT;
    if ((*ref)->mounted)
        return Q2_ERR_BUSY;
    free(*ref);
    *ref = NULL;
    return Q2_OK;
}
