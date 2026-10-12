#ifndef Q2_STORAGE_H
#define Q2_STORAGE_H
#include "q2/kernel.h"
/** @file storage.h Optional byte-addressed block capability; no vendor
 * filesystem types cross the boundary. */
typedef struct {
    unsigned abi;
    size_t size;
    void *context;
    uint32_t block_size, block_count, read_size, program_size;
    q2_result_t (*read)(void *, uint64_t offset, void *, size_t);
    q2_result_t (*program)(void *, uint64_t offset, const void *, size_t);
    q2_result_t (*erase)(void *, uint64_t offset, size_t);
    q2_result_t (*sync)(void *);
} q2_block_device_t;
typedef struct q2_storage q2_storage_t;
/** Maximum private LittleFS keys per APP UUID; each text value remains <=255 bytes. */
#define Q2_STORAGE_PRIVATE_KEYS 32u
/** Allocate a LittleFS adapter. Does not mount or format the media. */
q2_result_t q2_littlefs_create(q2_kernel_t *kernel, const q2_block_device_t *device,
                               q2_storage_t **out_storage);
/** Allocate a FatFs adapter for one 512-byte-sector volume. Single FatFs owner
 * in this build. */
q2_result_t q2_fatfs_create(q2_kernel_t *kernel, const q2_block_device_t *device, q2_storage_t **out_storage);
/** Explicitly erase/reformat an unmounted volume. Never called automatically by
 * boot. */
q2_result_t q2_storage_format(q2_storage_t *storage);
/** Obtain descriptor owned by storage; storage must outlive kernel
 * registration. */
q2_result_t q2_storage_component(q2_storage_t *storage, const q2_component_t **out_component);
/** Release an unmounted adapter after its kernel is destroyed; never
 * dereferences kernel. */
q2_result_t q2_storage_destroy(q2_storage_t **storage);
#endif
