/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_blockdev.h"

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
#define JRNL_STORE_FORMAT_VERSION       2   /* on-disk master / operation-header layout (1.1.0+) */

typedef int32_t esp_jrnl_handle_t;
typedef esp_err_t (*diskio_read) (int32_t handle, size_t src_addr, void *dest, size_t size);
typedef esp_err_t (*diskio_write) (int32_t handle, size_t dest_addr, const void *src, size_t size);
typedef esp_err_t (*diskio_erase_range) (int32_t handle, size_t start_addr, size_t size);

/**
 * @brief Journaling transaction status enumeration
 *
 * Reflects the persistent state of a journal store master record. The states form
 * a simple lifecycle:
 *  - @c ESP_JRNL_STATUS_FS_INIT / @c ESP_JRNL_STATUS_FS_DIRECT: journal bypassed (used during
 *    file-system format/mount and for direct maintenance access).
 *  - @c ESP_JRNL_STATUS_TRANS_READY: empty journal, ready to begin a new transaction.
 *  - @c ESP_JRNL_STATUS_TRANS_OPEN: a transaction is in progress; subsequent writes are logged.
 *  - @c ESP_JRNL_STATUS_TRANS_COMMIT: the journal is being applied to the target media.
 *    A power-loss in this state is recovered by @c jrnl_replay() on the next mount.
 */
typedef enum {
    ESP_JRNL_STATUS_FS_INIT,                                /*!< file-system being mounted/formatted on the journaled volume */
    ESP_JRNL_STATUS_FS_DIRECT = ESP_JRNL_STATUS_FS_INIT,    /*!< alias for better code readability */
    ESP_JRNL_STATUS_TRANS_READY,                            /*!< fresh new log or the last transaction processed completely */
    ESP_JRNL_STATUS_TRANS_OPEN,                             /*!< journaling transaction running */
    ESP_JRNL_STATUS_TRANS_COMMIT                            /*!< journaling transaction being committed to the target disk */
} esp_jrnl_trans_status_t;

/**
 * @brief File system journaling user configuration
 */
typedef struct {
    bool overwrite_existing;                /* create a new journaling store at any rate */
    bool replay_journal_after_mount;        /* true = apply unfinished-commit transaction if found during journal mount */
    bool force_fs_format;                   /* (re)format journaled file-system */
    size_t store_size_sectors;              /* journal store size in sectors (disk space deducted from WL partition end) */
    bool implicit_transactions;             /* standalone write/erase auto START + log + COMMIT when status is TRANS_READY */
} esp_jrnl_config_t;

#define ESP_JRNL_DEFAULT_CONFIG() { \
    .overwrite_existing = false, \
    .replay_journal_after_mount = true, \
    .force_fs_format = false, \
    .store_size_sectors = 32, \
    .implicit_transactions = true \
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
    uint64_t volume_size;                   /* partition space in bytes available for the file-system (eg after WL sectors deduction). JRNL part not included. 64-bit so >4GB media (SD/eMMC) are addressable on 32-bit targets */
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
 * A leftover open or committing transaction is cancelled (the on-disk master record is reset)
 * before the instance is freed. If another task holds the transaction window, this call blocks
 * until that task finishes.
 *
 * @note The caller must stop all file-system access to this mount before unmounting. A task that
 *       is already blocked waiting for the transaction window when the instance is destroyed
 *       cannot be woken safely; unmounting under concurrent I/O is undefined behaviour.
 *
 * @param handle  FS journal instance handle
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_TIMEOUT if the transaction window could not be acquired
 *      - errors from jrnl_check_handle()
 */
esp_err_t esp_jrnl_unmount(const esp_jrnl_handle_t handle);

/**
 * @brief Starts a new transaction for FS journal instance given by the handle.
 * The transaction can be started only on empty FS journal store (status must be ESP_JRNL_STATUS_TRANS_READY). Once the transaction is open
 * (status changed to ESP_JRNL_STATUS_TRANS_OPEN), all subsequent disk-write operations originated in the journaled file-system are written to the FS store first,
 * unless esp_jrnl_stop() is called for appropriate FS journal instance handle.
 *
 * @note A per-instance binary semaphore serializes the start..stop window so concurrent file
 *       operations from several tasks on the same mount are queued. COMMIT/CANCEL (or the BDL
 *       ioctl equivalents) may be issued from a different task than START. Nested START from the
 *       task that already owns the window returns @c ESP_ERR_INVALID_STATE. STOP without an open
 *       transaction also returns @c ESP_ERR_INVALID_STATE and does not release the semaphore.
 *       @c esp_jrnl_unmount() / @c esp_jrnl_set_direct_io() wait for any in-flight window.
 *       Destroying a mount while another task is still issuing file operations remains a caller
 *       responsibility.
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
 * @brief Read sectors from the target disk, overlaying any records from an open transaction.
 *
 * When no transaction is open this is a pass-through to the underlying device. During
 * @c ESP_JRNL_STATUS_TRANS_OPEN, later journaled WRITE/ERASE records that overlap the
 * requested range replace the bottom-device bytes so a file-system read-modify-write sees
 * its own uncommitted updates (and cancelled transactions stay invisible after STOP).
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

/**
 * @defgroup esp_jrnl_bdl_ioctl Block-device ioctl commands for journal transaction control
 *
 * These commands are issued through @c esp_blockdev_ops_t::ioctl on the journaled BDL handle
 * returned by @ref esp_jrnl_get_blockdev. They occupy a dedicated sub-range of the BDL
 * system command space starting at @ref ESP_JRNL_IOCTL_BASE. The authoritative map of all
 * reserved BDL command ranges (system, per-component sub-ranges and the user range) lives in
 * @c esp_blockdev.h; refer to it when allocating or relocating command numbers.
 *
 * They can be invoked by any consumer holding the BDL handle: typically the application
 * (manual, user-controlled transaction scope) or an IDF library that wraps a sequence of
 * writes in a transaction internally (e.g. a file-system auto-repair routine).
 *
 * Coordination is cooperative: only one transaction can be open at a time on a given
 * journal instance. A second @c ESP_JRNL_IOCTL_TRANS_START while a transaction is already
 * open returns @c ESP_ERR_INVALID_STATE. Use @ref ESP_JRNL_IOCTL_GET_STATUS to query the
 * current state before attempting to start a new transaction when collisions are possible.
 *
 * @{
 */

/**
 * @brief Base offset of the esp_jrnl ioctl sub-range within the BDL system command space.
 *
 * The five @c ESP_JRNL_IOCTL_* commands are expressed as offsets from this base so the
 * allocation is visible at a single point and easy to relocate as a unit if the BDL system
 * command layout evolves.
 */
#define ESP_JRNL_IOCTL_BASE             (ESP_BLOCKDEV_CMD_SYSTEM_BASE + 30)

/* Inclusive reservation of the five ESP_JRNL_IOCTL_* commands. The marker is a no-op at
 * compile time; CMakeLists.txt appends this header to ESP_BLOCKDEV_IOCTL_DEF_FILES so the
 * esp_blockdev overlap checker picks it up. */
ESP_BLOCKDEV_RESERVE_CMD_RANGE(esp_jrnl, ESP_JRNL_IOCTL_BASE, ESP_JRNL_IOCTL_BASE + 4);

/**
 * @brief Begin a new journaling transaction
 *
 * Equivalent to @ref esp_jrnl_start on the underlying journal instance.
 *
 * Argument: @c NULL.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if a transaction is already open
 */
#define ESP_JRNL_IOCTL_TRANS_START      (ESP_JRNL_IOCTL_BASE + 0)

/**
 * @brief Commit the currently open journaling transaction
 *
 * Equivalent to @ref esp_jrnl_stop with @c commit=true.
 *
 * Argument: @c NULL.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if no transaction is open
 */
#define ESP_JRNL_IOCTL_TRANS_COMMIT     (ESP_JRNL_IOCTL_BASE + 1)

/**
 * @brief Cancel (discard) the currently open journaling transaction
 *
 * Equivalent to @ref esp_jrnl_stop with @c commit=false.
 *
 * Argument: @c NULL.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if no transaction is open
 */
#define ESP_JRNL_IOCTL_TRANS_CANCEL     (ESP_JRNL_IOCTL_BASE + 2)

/**
 * @brief Toggle direct (non-journaled) access on the underlying device
 *
 * Equivalent to @ref esp_jrnl_set_direct_io. Required for file-system format and
 * maintenance operations that produce more I/O than the journal store can hold.
 *
 * Argument: @c const @c bool*. @c true switches the journal to direct mode
 * (@c ESP_JRNL_STATUS_FS_DIRECT); @c false reverts to journaled mode
 * (@c ESP_JRNL_STATUS_TRANS_READY).
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if @c args is NULL
 *      - ESP_ERR_INVALID_STATE if a transaction is open
 */
#define ESP_JRNL_IOCTL_SET_DIRECT_IO    (ESP_JRNL_IOCTL_BASE + 3)

/**
 * @brief Query the current journal transaction status
 *
 * Argument: @c esp_jrnl_trans_status_t*. The current status is written to @c *args.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if @c args is NULL
 */
#define ESP_JRNL_IOCTL_GET_STATUS       (ESP_JRNL_IOCTL_BASE + 4)

/** @} */

/**
 * @brief Create a journaled Block Device Layer (BDL) device on top of an existing BDL device
 *
 * The journaled BDL exposes the same address space as @c bottom_bdl minus the journal store
 * (the last @c cfg->store_size_sectors journal-record units of @c bottom_bdl), which it uses for
 * transaction logging. The journal store layout and replay semantics match the legacy
 * @ref esp_jrnl_mount API.
 *
 * The journal records and replays data at one fixed internal unit derived from @c bottom_bdl:
 *  - erase-before-write media (e.g. SPI NOR, the legacy callback-bridge): the unit is
 *    @c erase_size. The bottom @c write_size only has to be a non-zero divisor of @c erase_size
 *    (the usual NOR case of @c write_size 1/16 B with a 4 kB erase block is fine); otherwise
 *    @c ESP_ERR_NOT_SUPPORTED is returned.
 *  - media without host-driven erase (SD/MMC, USB MSC, NAND+FTL): the unit is @c write_size.
 *
 * Geometry of the returned BDL relative to @c bottom_bdl:
 *  - @c disk_size  = @c bottom_bdl->geometry.disk_size minus the journal store size
 *  - @c read_size  = @c bottom_bdl->geometry.read_size  (pass-through; open-transaction records overlay the bottom read)
 *  - @c erase_size = @c bottom_bdl->geometry.erase_size (pass-through)
 *  - @c write_size = the journal record unit above (= @c erase_size on erase-before-write media,
 *    = @c bottom_bdl->geometry.write_size otherwise). The layer above can only write at record
 *    granularity, since the journal logs and replays whole records.
 * esp_jrnl is BDL middleware: it does not invent geometry beyond this journaling constraint and
 * otherwise trusts the values reported by @c bottom_bdl.
 *
 * @c device_flags are copied verbatim from @c bottom_bdl. The journal honours
 * @c device_flags.erase_before_write to decide whether to issue erase calls on
 * @c bottom_bdl internally, which makes the implementation usable on media that
 * does not expose host-driven erase (SD/MMC, USB MSC, NAND with FTL).
 *
 * Transactions are driven through the @c esp_blockdev_ops_t::ioctl entry point using the
 * @c ESP_JRNL_IOCTL_TRANS_* commands. Reads overlay any open-transaction records onto the
 * bottom device. Writes and host-driven erases (when @c erase_before_write) are accepted
 * while a transaction is open, while direct access is enabled, or — when
 * @c cfg->implicit_transactions is true (the default) — as a single-op implicit transaction
 * in @c ESP_JRNL_STATUS_TRANS_READY. Explicit ioctl still groups multi-op atomicity.
 * Writes outside these states return @c ESP_ERR_INVALID_STATE.
 *
 * Initial state on success:
 *  - @c cfg->force_fs_format @c == @c false (the default): the journal is left in
 *    @c ESP_JRNL_STATUS_TRANS_READY and the caller can immediately issue
 *    @c ESP_JRNL_IOCTL_TRANS_START. Any on-disk @c TRANS_COMMIT record is replayed
 *    before the call returns; any on-disk @c TRANS_OPEN record is discarded.
 *  - @c cfg->force_fs_format @c == @c true: the journal is left in
 *    @c ESP_JRNL_STATUS_FS_DIRECT so the caller can perform a one-shot direct
 *    (non-journaled) pass over the device — typically an upper file-system format.
 *    Once the direct pass is done, switch back to journaled mode by issuing
 *    @c ESP_JRNL_IOCTL_SET_DIRECT_IO with @c *(const @c bool@c *)args @c == @c false.
 *
 * Ownership of @c bottom_bdl stays with the caller. Releasing the returned journaled BDL
 * (via @c out_bdl->ops->release) does NOT release @c bottom_bdl.
 *
 * On release with an open transaction, the in-memory journal instance is freed but the
 * on-disk master record is left untouched. The transaction is resolved by @c jrnl_replay()
 * on the next @ref esp_jrnl_get_blockdev or @ref esp_jrnl_mount of the same store
 * (open transactions are discarded; committing transactions are completed at any rate).
 *
 * @param[in]  bottom_bdl   Underlying BDL device (must be writable and provide @c erase_size > 0)
 * @param[in]  cfg          Journal configuration; @c store_size_sectors must be at least @ref JRNL_MIN_STORE_SIZE
 * @param[out] out_bdl_handle  Receives the journaled BDL handle on success
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is NULL, @c store_size_sectors is too small,
 *        or @c bottom_bdl geometry is not usable for journaling
 *      - ESP_ERR_NO_MEM if instance/buffer allocation fails
 *      - ESP_ERR_INVALID_STATE if the existing on-disk master record does not match
 *        the supplied geometry
 *      - errors propagated from @c jrnl_replay() and from @c bottom_bdl->ops
 */
esp_err_t esp_jrnl_get_blockdev(esp_blockdev_handle_t bottom_bdl,
                                const esp_jrnl_config_t *cfg,
                                esp_blockdev_handle_t *out_bdl_handle);

#ifdef __cplusplus
}
#endif
