/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * File-system journaling structures and defines
 */

#ifdef __cplusplus
extern "C" {
#endif

#define JRNL_INVALID_HANDLE            -1   /* invalid handle index */
#define JRNL_MAX_HANDLES                8   /* copies WL logic for now, see MAX_WL_HANDLES */
#define JRNL_MIN_STORE_SIZE             3   /* minimum applicable journaling store size in sectors (master sec + header + data) */
#define JRNL_STORE_MARKER      0x6A6B6C6D   /* journaling store identifier (first 32 bits of master sector) */

typedef int32_t esp_jrnl_handle_t;
typedef esp_err_t (*diskio_read) (int32_t handle, size_t src_addr, void *dest, size_t size);
typedef esp_err_t (*diskio_write) (int32_t handle, size_t dest_addr, const void *src, size_t size);
typedef esp_err_t (*diskio_erase_range) (int32_t handle, size_t start_addr, size_t size);

/**
 * @brief File system journaling user configuration
 */
typedef struct {
    bool overwrite_existing;                /* create a new journaling store at any rate */
    bool replay_journal_after_mount;        /* true = apply unfinished-commit transaction if found during journal mount */
    bool force_fs_format;                   /* (re)format journaled file-system */
    size_t store_size_sectors;              /* journal store size in sectors (disk space deducted from WL partition end) */
} esp_jrnl_config_t;

#define ESP_JRNL_DEFAULT_CONFIG() { \
    .overwrite_existing = false, \
    .replay_journal_after_mount = true, \
    .force_fs_format = false, \
    .store_size_sectors = 32 \
}

#define ESP_JRNL_VOLUME_DEFAULT_CONFIG(wl_hndl) { \
    .volume_size = wl_size(wl_hndl), \
    .disk_sector_size = wl_sector_size(wl_hndl) \
}

#define ESP_JRNL_DISKIO_DEFAULT_CONFIG(wl_hndl) { \
    .diskio_ctrl_handle = wl_hndl, \
    .disk_read = &wl_read, \
    .disk_write = &wl_write, \
    .disk_erase_range = &wl_erase_range \
}

/**
 * @brief Raw access to target disk (journaling "bottom" APIs)
 */
typedef struct {
    int32_t diskio_ctrl_handle;             /* generic handle value holder, used to identify proper disk-controller instance (for unmounting etc, eg wl_handle_t) */
    diskio_read disk_read;                  /* disk read routine of the 'diskio_ctrl_handle' controller interface (eg wl_read). Sector-based addressing */
    diskio_write disk_write;                /* disk write routine of the 'diskio_ctrl_handle' controller interface (eg wl_write). Sector-based addressing */
    diskio_erase_range disk_erase_range;    /* disk erase range routine of the 'diskio_ctrl_handle' controller interface (eg wl_erase_range). Sector-based addressing */
} esp_jrnl_diskio_t;

/**
 * @brief Journaled disk volume configuration
 */
typedef struct {
    size_t volume_size;                     /* partition space in bytes available for the file-system (eg after WL sectors deduction). JRNL part not included */
    size_t disk_sector_size;                /* target disk sector size */
} esp_jrnl_volume_t;

/**
 * @brief File system journaling internal configuration
 */
typedef struct {
    esp_jrnl_config_t user_cfg;
    uint8_t fs_volume_id;                   /* see esp_jrnl_instance_t */
    esp_jrnl_volume_t volume_cfg;
    esp_jrnl_diskio_t diskio_cfg;
} esp_jrnl_config_extended_t;


/**
 * @brief Mounts FS journal store instance to wear-levelled partition, checks existence of previously
 *        created journaling log and possibly applies the operations found in the log. If all the internal steps succeed,
 *        the journal store instance is referenced by the handle returned. The handle remains uninitialized if any error appears
 *
 * @param[in] config  FS journal instance configuration
 * @param[out] jrnl_handle  FS journal instance handle
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG on some input parameter being NULL or store_size_sectors less than JRNL_MIN_STORE_SIZE
 *      - ESP_ERR_NO_MEM no more handles available or no memory left for the FS journal instance record
 *      - ESP_ERR_INVALID_STATE if volume size, sector size or journal size differ between the configuration and the master record found on disk (sanity check)
 *      - errors from jrnl_replay()
 *      - errors from diskio.disk_read, diskio_write or diskio_erase_range function (eg wl_read(), wl_write() and wl_erase_range() for FatFS)
 */
esp_err_t esp_jrnl_mount(const esp_jrnl_config_extended_t *config, esp_jrnl_handle_t* jrnl_handle);

/**
 * @brief Deletes FS journal instance given by the handle and clears the handle afterwards
 *
 * @param handle  FS journal instance handle
 *
 * @return
 *      - ESP_OK on success
 *      - errors from jrnl_check_handle()
 */
esp_err_t esp_jrnl_unmount(const esp_jrnl_handle_t handle);

/**
 * @brief Starts a new transaction for FS journal instance given by the handle.
 * The transaction can be started only on empty FS journal store (status must be ESP_JRNL_STATUS_TRANS_READY). Once the transaction is open
 * (status changed to ESP_JRNL_STATUS_TRANS_OPEN), all subsequent disk-write operations originated in the journaled file-system are written to the FS store first,
 * unless esp_jrnl_stop() is called for appropriate FS journal instance handle.
 *
 * @param handle  FS journal instance handle
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the transaction is already started
 *      - errors from jrnl_check_handle() or jrnl_update_master()
 */
esp_err_t esp_jrnl_start(const esp_jrnl_handle_t handle);

/**
 * @brief Stops on-going transaction for FS journal instance given by the handle.
 * The transaction can be either canceled (commit = false) or transcribed from the journaling store to the target disk (commit = true).
 * Once the transaction is being transferred to the target disk, the FS journal store status is set to ESP_JRNL_STATUS_TRANS_COMMIT,
 * unless all the journaled operations are successfully written to the target. The store FS journal gets reset to ESP_JRNL_STATUS_TRANS_READY afterwards.
 *
 * @param[in] handle  FS journal instance handle
 * @param[in] commit  boolean flag to cancel (false) or commit (true) currently open transaction
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if no transaction is open
 *      - errors from jrnl_check_handle(), jrnl_replay() and jrnl_update_master()
 */
esp_err_t esp_jrnl_stop(const esp_jrnl_handle_t handle, const bool commit);

/**
 * @brief Updates journal master record status to switch between direct disk access and the journaled one
 *
 * Sets master.status record of journal instance given by 'handle' to ESP_JRNL_STATUS_FS_DIRECT for 'direct_access' = true,
 * and to ESP_JRNL_STATUS_TRANS_READY on false, applicable only when the status is one of the two states.
 * This status allows direct I/O access to the target disk, ie it bypasses the journaling mechanism.
 * Needed for file-system mounting, formatting or similar operations.
 * Should never by called directly as it brings high risk of the FS journal instance corruption and/or data loss.
 *
 * @note So far, direct disk access is applied only within jrnl_write API
 *
 * @param handle  FS journal instance handle
 * @param direct_access  'true' - use direct access, 'false'- use journaled access
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if vfs argument is NULL
 *      - ESP_ERR_INVALID_STATE if transaction is in progress
 */
esp_err_t esp_jrnl_set_direct_io(const esp_jrnl_handle_t handle, bool direct_access);

/**
 * @brief Gets handle to the target disk operation driver instance (eg wl_handle for WL-partition).
 * Designed for internal use, mostly by the VFS implementations
 *
 * @param[in] handle  FS journal instance handle
 * @param[out] diskio_ctrl_handle  output parameter to receive the target disk handle
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the diskio_ctrl_handle is NULL
 *      - errors from jrnl_check_handle()
 */
esp_err_t esp_jrnl_get_diskio_handle(const esp_jrnl_handle_t handle, int32_t* diskio_ctrl_handle);

/**
 * @brief Gets target disk sector count for given FS journal instance handle (ie the sectors available for the file-system journaled).
 * Designed for internal use, mostly by the VFS implementations
 *
 * @param[in] handle  FS journal instance handle
 * @param[out] fs_part_size  output parameter to receive the target disk sector count, ie FS partition size
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the sector_size is NULL
 *      - errors from jrnl_check_handle()
 */
esp_err_t esp_jrnl_get_sector_count(const esp_jrnl_handle_t handle, size_t* fs_part_size);

/**
 * @brief Gets target disk sector size for given FS journal instance handle.
 * Designed for internal use, mostly by the VFS implementations
 *
 * @param[in] handle  FS journal instance handle
 * @param[out] sector_size  output parameter to receive the target disk sector size
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the sector_size is NULL
 *      - errors from jrnl_check_handle()
 */
esp_err_t esp_jrnl_get_sector_size(const esp_jrnl_handle_t handle, size_t* sector_size);

/**
 * @brief Writes 'count' of sectors starting at 'sector' index with data from 'buff' to the target disk.
 * If there is journaling transaction open (status = ESP_JRNL_STATUS_TRANS_OPEN), the data is written to FS
 * journal store instance given by 'handle'. Each such a journaled chunk occupies one extra sector filled with
 * related 'header' data (CRC32s of the data and the header's items, original target sector and sector count).
 *
 * If no transaction is available and the FS journal store status is ESP_JRNL_STATUS_FS_DIRECT (or ESP_JRNL_STATUS_FS_INIT),
 * the operation is propagated directly to the target disk with original parameters (useful for testing, mounting, formatting
 * and other file-system maintenance operations).
 *
 * In all other cases esp_jrnl_write() fails.
 *
 * @param[in] handle  FS journal instance handle
 * @param[in] buff  input data buffer
 * @param[in] sector  index of the target disk sector
 * @param[in] count  number of sectors to write (ie buff length in multiples of sector size)
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if 'buff' is NULL
 *      - ESP_ERR_NO_MEM not enough memory to complete the operation
 *      - ESP_ERR_INVALID_STATE on attempt to write under invalid journal stored state
 *      - errors from jrnl_check_handle(), jrnl_erase_range_raw(), jrnl_write_raw(), jrnl_write_raw() or jrnl_update_master()
 */
esp_err_t esp_jrnl_write(const esp_jrnl_handle_t handle, const uint8_t *buff, const uint32_t sector, const uint32_t count);

/**
 * @brief Essentially redirection to underlying partition read operation (eg to wl_read()). This operation is designed
 * for the journaled file-system to access its sectors and thus there is no need to involve the journaling mechanisms.
 * The only point is to check the range and other parameters.
 *
 * esp_jrnl_read() reads 'count' of sectors into 'dest' buffer, starting at 'sector' index of the target disk. 'handle' of
 * FS journal instance is provided for the sanity checks mentioned above.
 *
 * @param[in] handle  FS journal instance handle
 * @param[out] dest  destination data buffer
 * @param[in] sector  index of the target disk sector
 * @param[in] count  number of sectors to read (ie minimum dest length in multiples of sector size)
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if 'dest' is NULL
 *      - ESP_ERR_INVALID_SIZE if the operation is out of bounds
 *      - errors from jrnl_check_handle() or jrnl_read_raw()
 */
esp_err_t esp_jrnl_read(const esp_jrnl_handle_t handle, const uint32_t sector, uint8_t *dest, const uint32_t count);

#ifdef __cplusplus
}
#endif
