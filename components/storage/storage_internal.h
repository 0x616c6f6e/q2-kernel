#ifndef Q2_STORAGE_INTERNAL_H
#define Q2_STORAGE_INTERNAL_H
#include "ff.h"
#include "lfs.h"
#include "q2/storage.h"
struct q2_storage {
    q2_kernel_t *kernel;
    q2_block_device_t block;
    bool fat, mounted;
    q2_component_t component;
    lfs_t lfs;
    struct lfs_config config;
    FATFS fs;
};
q2_result_t q2_storage_new(q2_kernel_t *, const q2_block_device_t *, bool, q2_storage_t **);
q2_result_t q2_fatfs_mount(q2_storage_t *);
q2_result_t q2_fatfs_unmount(q2_storage_t *);
q2_result_t q2_fatfs_format(q2_storage_t *);
q2_result_t q2_fatfs_access(q2_storage_t *, const q2_caller_t *, const q2_value_t *, size_t, q2_value_t *,
                            bool);
#endif
