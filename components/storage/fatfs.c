#include "ff.h"

#include "diskio.h"
#include "storage_internal.h"
#include <stdio.h>
#include <string.h>
/* FatFs diskio uses a drive number, so this profile deliberately binds one
 * global drive. */
static q2_storage_t *owner;
DSTATUS disk_initialize(BYTE drive) {
    return !drive && owner ? 0 : STA_NOINIT;
}
DSTATUS disk_status(BYTE drive) {
    return disk_initialize(drive);
}
DRESULT disk_read(BYTE drive, BYTE *data, LBA_t sector, UINT count) {
    if (drive || !owner || !data || !count || sector >= owner->block.block_count ||
        count > owner->block.block_count - sector)
        return RES_PARERR;
    return owner->block.read(owner->block.context, (uint64_t)sector * 512, data, (size_t)count * 512) == Q2_OK
               ? RES_OK
               : RES_ERROR;
}
DRESULT disk_write(BYTE drive, const BYTE *data, LBA_t sector, UINT count) {
    if (drive || !owner || !data || !count || sector >= owner->block.block_count ||
        count > owner->block.block_count - sector)
        return RES_PARERR;
    return owner->block.program(owner->block.context, (uint64_t)sector * 512, data, (size_t)count * 512) ==
                   Q2_OK
               ? RES_OK
               : RES_ERROR;
}
DRESULT disk_ioctl(BYTE drive, BYTE cmd, void *data) {
    if (drive || !owner)
        return RES_NOTRDY;
    if (cmd == CTRL_SYNC)
        return owner->block.sync(owner->block.context) == Q2_OK ? RES_OK : RES_ERROR;
    if (!data)
        return RES_PARERR;
    if (cmd == GET_SECTOR_COUNT)
        *(LBA_t *)data = owner->block.block_count;
    else if (cmd == GET_SECTOR_SIZE)
        *(WORD *)data = 512;
    else if (cmd == GET_BLOCK_SIZE)
        *(DWORD *)data = 1;
    else
        return RES_PARERR;
    return RES_OK;
}
static q2_result_t map(FRESULT r) {
    if (r == FR_OK)
        return Q2_OK;
    if (r == FR_NO_FILE || r == FR_NO_PATH)
        return Q2_ERR_NOT_FOUND;
    if (r == FR_NO_FILESYSTEM)
        return Q2_ERR_FORMAT;
    if (r == FR_NOT_ENOUGH_CORE)
        return Q2_ERR_MEMORY;
    return Q2_ERR_IO;
}
q2_result_t q2_fatfs_mount(q2_storage_t *s) {
    if (owner && owner != s)
        return Q2_ERR_BUSY;
    owner = s;
    q2_result_t e = map(f_mount(&s->fs, "0:", 1));
    if (e != Q2_OK) {
        f_mount(NULL, "0:", 0);
        owner = NULL;
    }
    return e;
}
q2_result_t q2_fatfs_unmount(q2_storage_t *s) {
    if (owner != s)
        return Q2_ERR_STATE;
    q2_result_t e = s->block.sync(s->block.context);
    if (e != Q2_OK)
        return e;
    e = map(f_mount(NULL, "0:", 0));
    if (e == Q2_OK)
        owner = NULL;
    return e;
}
q2_result_t q2_fatfs_format(q2_storage_t *s) {
    if (owner)
        return Q2_ERR_BUSY;
    owner = s;
    BYTE scratch[4096];
    MKFS_PARM params = {.fmt = FM_FAT | FM_SFD, .n_fat = 1, .align = 1, .n_root = 128, .au_size = 0};
    q2_result_t e = map(f_mkfs("0:", &params, scratch, sizeof(scratch)));
    owner = NULL;
    return e;
}
q2_result_t q2_fatfs_access(q2_storage_t *s, const q2_caller_t *c, const q2_value_t *a, size_t n,
                            q2_value_t *out, bool write) {
    (void)n;
    if (owner != s)
        return Q2_ERR_STATE;
    char dir[42], path[80];
    snprintf(dir, sizeof(dir), "0:/%s", c->app_id);
    snprintf(path, sizeof(path), "%.41s/%.32s", dir, a[0].as.string);
    if (write) {
        FRESULT e = f_mkdir(dir);
        if (e != FR_OK && e != FR_EXIST)
            return map(e);
        DIR d;
        FILINFO info;
        e = f_opendir(&d, dir);
        if (e != FR_OK)
            return map(e);
        unsigned files = 0;
        while ((e = f_readdir(&d, &info)) == FR_OK && info.fname[0])
            files++;
        FRESULT close = f_closedir(&d);
        if (e != FR_OK || close != FR_OK)
            return Q2_ERR_IO;
        if (f_stat(path, &info) == FR_NO_FILE && files >= 16)
            return Q2_ERR_LIMIT;
    }
    FIL file;
    FRESULT e = f_open(&file, path, write ? (FA_WRITE | FA_CREATE_ALWAYS) : FA_READ);
    if (e != FR_OK)
        return map(e);
    UINT count = 0;
    if (write) {
        size_t bytes = strlen(a[1].as.string);
        e = f_write(&file, a[1].as.string, (UINT)bytes, &count);
        if (e == FR_OK && count != bytes)
            e = FR_DISK_ERR;
    } else if (f_size(&file) >= Q2_TEXT_MAX)
        e = FR_INVALID_PARAMETER;
    else {
        e = f_read(&file, out->as.string, (UINT)f_size(&file), &count);
        if (e == FR_OK && count != f_size(&file))
            e = FR_DISK_ERR;
        out->as.string[count] = 0;
        out->kind = Q2_STRING;
    }
    FRESULT close = f_close(&file);
    return map(e == FR_OK ? close : e);
}
