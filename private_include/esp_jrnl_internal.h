/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/lock.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_jrnl.h"
#include "sdkconfig.h"

/*
 * File-system journaling internal structures and defines
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Journaling operation record header. Covers one 'disk_write' operation
 * (typical file-system API invokes several 'disk_writes' at various stages)
 *
 * @note @c esp_jrnl_trans_status_t (the journal transaction status enum) lives in the
 * public header @c esp_jrnl.h since the BDL ioctl @c ESP_JRNL_IOCTL_GET_STATUS exposes it.
 */
#define ESP_JRNL_OP_WRITE               0u  /* journaled write (header + data sectors) */
#define ESP_JRNL_OP_ERASE               1u  /* journaled erase (header only) */

typedef struct {
    uint64_t target_sector;                 /* first target sector in the filesystem address space */
    uint32_t sector_count;                  /* number of sectors involved in current operation */
    uint32_t op_type;                       /* ESP_JRNL_OP_WRITE or ESP_JRNL_OP_ERASE */
    uint32_t crc32_data;                    /* sector data checksum (write); 0 for erase */
} esp_jrnl_oper_header_t;

typedef struct {
    esp_jrnl_oper_header_t header;
    uint32_t crc32_header;                  /* operation header checksum (contents of the struct instance) */
} esp_jrnl_operation_t;

/**
 * @brief Journaling store master record, only 1 instance defined per journaled partition
 */
typedef struct {
    uint32_t jrnl_magic_mark;               /* journaling store master record identification stamp */
    uint32_t format_version;                /* JRNL_STORE_FORMAT_VERSION; reject unknown versions on mount */
    uint32_t store_size_sectors;            /* size of journaling store in sectors */
    uint64_t store_volume_offset_sector;    /* index of the first journaling store sector within the volume */
    uint32_t next_free_sector;              /* next free block. Default = 0 (relative offset in the store space) */
    esp_jrnl_trans_status_t status;         /* transaction status. Default = ESP_JRNL_STATUS_TRANS_READY */
    esp_jrnl_volume_t volume;               /* disk volume properties */
} esp_jrnl_master_t;

/**
 * @brief Runtime configuration of a single journaling store instance. Not stored on the target media, memory only
 *
 * The instance always operates on top of a BDL device pointed to by @c bottom_bdl. The two
 * supported creation flows differ only in how that bottom BDL is obtained:
 *  - legacy @ref esp_jrnl_mount path: a tiny synthetic "callback-bridge" BDL is built around
 *    the @c esp_jrnl_diskio_t callbacks and owned by the instance (@c owns_bottom_bdl = true);
 *  - new @ref esp_jrnl_get_blockdev path: @c bottom_bdl points to a real BDL provided by the
 *    caller, who retains ownership (@c owns_bottom_bdl = false).
 */
typedef struct {
    _lock_t trans_lock;
    /* Binary semaphore serializing one transaction window. START takes it, COMMIT/CANCEL
     * give it — give can come from any task, so the closer need not be the opener.
     * Distinct from trans_lock, which only guards short master-record critical sections. */
    SemaphoreHandle_t window_sem;
    TaskHandle_t window_owner;              /* task that last successfully opened the window; used to reject nested START */
    bool implicit_transactions;             /* runtime copy of user_cfg.implicit_transactions */
    uint8_t fs_volume_id;                   /* file-system volume ID (PDRV for FatFS); legacy path only */
    esp_jrnl_diskio_t diskio;               /* disk device access configuration; legacy path only */
    esp_blockdev_handle_t bottom_bdl;       /* underlying BDL device used for all raw I/O */
    bool owns_bottom_bdl;                   /* true if bottom_bdl is the internal callback-bridge BDL and must be released with the instance */
    esp_jrnl_master_t master;               /* journal master record for given instance */
#ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE
    uint32_t test_config;                   /* runtime flags for internal testing, 0x0 by default */
#endif
} esp_jrnl_instance_t;

/**
 * @brief Build an internal callback-bridge BDL wrapping the legacy @c esp_jrnl_diskio_t callbacks
 *
 * Used by @ref esp_jrnl_mount to keep a single internal code path (everything goes through BDL
 * ops). The synthetic BDL is initialized with SPI Flash defaults
 * (@c erase_before_write=1, @c and_type_write=1, @c default_val_after_erase=1), which preserves
 * the historical behavior of @c esp_vfs_fat_spiflash_mount_jrnl over wear-levelling exactly.
 *
 * @param[in]  diskio       Legacy disk-I/O callbacks (must all be non-NULL)
 * @param[in]  volume_size  Total disk space exposed by the callbacks, in bytes
 * @param[in]  sector_size  Sector size used by the callbacks (granularity of read/write/erase)
 * @param[out] out_bdl      Receives the synthesized BDL handle on success (must be released with @c ops->release)
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG on invalid inputs
 *      - ESP_ERR_NO_MEM on allocation failure
 */
esp_err_t jrnl_legacy_bridge_bdl_create(const esp_jrnl_diskio_t *diskio,
                                        uint64_t volume_size,
                                        size_t sector_size,
                                        esp_blockdev_handle_t *out_bdl);

/**
 * @brief Internal handler shared by both API paths. Allocates an @c esp_jrnl_instance_t,
 * reads and validates the on-disk master record (if any), optionally replays a pending
 * transaction, and writes a fresh master record when needed.
 *
 * Caller owns @c bottom_bdl; this function does not release it on failure.
 *
 * @param[in]  user_cfg     Journal user configuration
 * @param[in]  bottom_bdl   Underlying BDL (real or synthetic callback-bridge)
 * @param[in]  owns_bdl     Whether the produced instance should release @c bottom_bdl when freed
 * @param[in]  diskio       Legacy disk-I/O config; copied into the instance for backward-compat accessors. May be NULL for the BDL path.
 * @param[in]  fs_volume_id File-system volume id; only meaningful for the legacy path. Use 0 for BDL path.
 * @param[out] out_instance Receives the allocated instance on success
 *
 * @return
 *      - ESP_OK on success; @c *out_instance is non-NULL.
 *      - ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE / ESP_ERR_NO_MEM on failure; @c *out_instance is NULL.
 */
esp_err_t jrnl_instance_create(const esp_jrnl_config_t *user_cfg,
                               esp_blockdev_handle_t bottom_bdl,
                               bool owns_bdl,
                               const esp_jrnl_diskio_t *diskio,
                               uint8_t fs_volume_id,
                               esp_jrnl_instance_t **out_instance);

/**
 * @brief Free an @c esp_jrnl_instance_t allocated by @ref jrnl_instance_create.
 *
 * Releases @c bottom_bdl if and only if the instance was created with @c owns_bottom_bdl = true.
 * Does NOT touch the on-disk master record; pending transactions are resolved by @c jrnl_replay()
 * on the next mount (discarded if OPEN, completed if COMMIT).
 *
 * @param[in] inst_ptr  Journal instance to free (may be NULL)
 */
void jrnl_instance_destroy(esp_jrnl_instance_t *inst_ptr);

/**
 * @brief Instance-pointer transaction control shared by the legacy handle API and the BDL ioctl path.
 *
 * jrnl_inst_trans_start() takes @c window_sem and holds it until the paired
 * jrnl_inst_trans_stop() (any task may stop). Nested START from the owning task returns
 * ESP_ERR_INVALID_STATE without touching the semaphore. STOP without an open transaction
 * returns ESP_ERR_INVALID_STATE and does not give the semaphore.
 */
esp_err_t jrnl_inst_trans_start(esp_jrnl_instance_t *inst_ptr);
esp_err_t jrnl_inst_trans_stop(esp_jrnl_instance_t *inst_ptr, bool commit);
esp_err_t jrnl_inst_set_direct_io(esp_jrnl_instance_t *inst_ptr, bool direct_access);

/**
 * @brief Log a write (@c buff != NULL) or erase (@c buff == NULL) in an open transaction,
 *        or wrap the same operation in an implicit START/COMMIT when the store is READY
 *        and @c implicit_transactions is set. Any other state returns @c ESP_ERR_INVALID_STATE.
 */
esp_err_t jrnl_inst_log_or_implicit(esp_jrnl_instance_t *inst_ptr,
                                    const uint8_t *buff,
                                    uint64_t sector,
                                    uint32_t count);

/**
 * @brief Append a journaled-write record (header + data) to an open journal store.
 *
 * Instance-pointer counterpart of @ref esp_jrnl_write's @c TRANS_OPEN branch, factored out so
 * that both the legacy integer-handle API and the BDL ioctl/op path share one implementation.
 * The transaction status is re-validated under @c trans_lock: a caller-side check is racy
 * because COMMIT/CANCEL may be issued from any task, so this function returns
 * @c ESP_ERR_INVALID_STATE if the transaction is no longer open by the time the record
 * would be appended.
 *
 * @param[in] inst_ptr Journal instance
 * @param[in] buff     Source data buffer (length @c count * sector_size bytes)
 * @param[in] sector   Target sector index in the file-system address space
 * @param[in] count    Number of consecutive sectors covered by this write
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if no transaction is open
 *      - ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM / underlying errors otherwise
 */
esp_err_t jrnl_log_write(esp_jrnl_instance_t *inst_ptr,
                         const uint8_t *buff,
                         const uint64_t sector,
                         const uint32_t count);

/**
 * @brief Append a journaled-erase record (header only) to an open journal store.
 *
 * Replay applies the erase to the target range. On media without host-driven erase
 * (@c erase_before_write == 0) the logged erase is a no-op at replay time. Status is
 * re-validated under @c trans_lock the same way as @ref jrnl_log_write.
 */
esp_err_t jrnl_log_erase(esp_jrnl_instance_t *inst_ptr,
                         const uint64_t sector,
                         const uint32_t count);

/**
 * @brief Read a byte range from the target volume, overlaying any open-transaction records.
 *
 * Always starts with a bottom-device read of [@c src_addr, @c src_addr + @c len). When a
 * transaction is open, later journaled WRITE/ERASE records that overlap the range replace
 * that slice (erase fills @c default_val_after_erase). Used by both @c esp_jrnl_read and
 * the journaled BDL read op so FatFS read-modify-write sees its own uncommitted writes.
 */
esp_err_t jrnl_log_read(esp_jrnl_instance_t *inst_ptr,
                        uint8_t *dest,
                        uint64_t src_addr,
                        size_t len);

/* Internal test flags (runtime configuration, not stored in the journal)
 * All the flags cause preliminary exit of esp_jrnl_stop() or jrnl_replay() in various stages, all return with ESP_OK
 * The point is to emulate power-off event at sensitive points of the journaling code workflow */
#ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE
#define ESP_JRNL_TEST_STOP_SKIP_COMMIT              0x00000001  /* don't start committing transaction (== leave it OPEN), and exit */
#define ESP_JRNL_TEST_STOP_SET_COMMIT_AND_EXIT      0x00000002  /* start committing transaction (== update the master record), and exit */
#define ESP_JRNL_TEST_REPLAY_ERASE_AND_EXIT         0x00000004  /* erase the first target sector within journaled data transfer, and exit */
#define ESP_JRNL_TEST_REPLAY_WRITE_AND_EXIT         0x00000008  /* erase and write the first target sector within journaled data transfer, and exit */
#define ESP_JRNL_TEST_REPLAY_EXIT_BEFORE_CLOSE      0x00000010  /* finish transferring all the journaled sectors but don't set the master status to READY, and exit */
#define ESP_JRNL_TEST_REQUIRE_FILE_CLOSE            0x00000020  /* fclose()/close() operation required for given testing procedure */
#define ESP_JRNL_TEST_SUSPEND_TRANSACTION           0x00000040  /* keeps jrnl_start/stop disabled, allows direct FS operations */
#define ESP_JRNL_TEST_LOG_RACE_DELAY                0x00000080  /* delay between the unlocked status dispatch check and the journal append, so a
                                                                 * test can commit from another task in the gap and exercise the append-vs-commit race */
#endif

/**
 * @brief Debug printout of esp_jrnl_instance_t record structure (master + all data headers, if any)
 *
 * @param[in] inst_ptr  jrnl instance record
 */
void print_jrnl_instance(esp_jrnl_instance_t* inst_ptr);

/**
 * @brief Debug printout of esp_jrnl_master_t record structure
 *
 * @param[in] jrnl_master  jrnl master record instance
 */
void print_jrnl_master(const esp_jrnl_master_t* jrnl_master);

/**
 * @brief Debug printout of esp_jrnl_config_extended_t record structure
 *
 * @param[in] config  jrnl extended configuration record instance
 */
void print_jrnl_config_extended(const esp_jrnl_config_extended_t *config);

/**
 * @brief Checks validity of FS journal instance handle
 *
 * @param[in] handle  instance handle
 * @param[in] func  name of the caller function for better log readability
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the handle equals to JRNL_INVALID_HANDLE
 *      - ESP_ERR_INVALID_ARG if the handle is out of range (>= JRNL_MAX_HANDLES)
 *      - ESP_ERR_NOT_FOUND if there is no instance assigned to the handle
 */
esp_err_t jrnl_check_handle(const esp_jrnl_handle_t handle, const char *func);

/**
 * @brief Converts FS journal store index to parent partition disk sector index
 *
 * This function is designed for strictly internal use and provides no parameter check
 *
 * @param inst_ptr  FS journal instance pointer
 * @param jrnl_sector  journal store sector index
 *
 * @return parent partition disk sector index
 */
uint64_t jrnl_get_target_disk_sector(const esp_jrnl_instance_t* inst_ptr, const uint32_t jrnl_sector);

/**
 * @brief Reads 'count' sectors from the journaling store instance given by ínst_ptr' and referenced by 'sector' index
 * (must be within <0, JRNL_SECTOR_COUNT-1>), and stores the data into 'out_buff', which is expected large enough for holding the payload
 *
 * @param inst_ptr  FS journal instance pointer
 * @param func  name of the caller function for better log readability
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG some of the arguments are NULL or out of range
 *      - errors returned by underlying diskio.disk_read operation (eg wl_read())
 */
esp_err_t jrnl_read_internal(esp_jrnl_instance_t* inst_ptr, uint8_t *out_buff, uint32_t sector, uint32_t count);

/**
 * @brief Writes 'count' sectors of 'buff' data to the journaling store at 'sector' index (must be <0, JRNL_SECTOR_COUNT-1>)
 * for given 'ínst_ptr' instance
 *
 * @param inst_ptr  FS journal instance pointer
 * @param func  name of the caller function for better log readability
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG some of the arguments are NULL or out of range
 *      - errors returned by underlying diskio.disk_erase_range and diskio.disk_write operations (eg wl_write())
 */
esp_err_t jrnl_write_internal(esp_jrnl_instance_t* inst_ptr, const uint8_t *buff, const uint32_t sector, const uint32_t count);

/**
 * @brief Reset's the journal master record of given instance to its defaults:
 *          - master.jrnl_magic_mark = JRNL_STORE_MARKER;
 *          - master.next_free_sector = 0;
 *          - master.status = init ? ESP_JRNL_STATUS_FS_INIT : ESP_JRNL_STATUS_TRANS_READY;
 * Other fields remain untouched.
 *
 * @param inst_ptr  FS journal instance pointer
 * @param fs_direct  flag to distinguish between (initial) file-system direct access (FS mount, format, etc) and journaling-on state (JRNL transaction ready)
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG on 'jrnl' instance being NULL
 *      - errors returned by jrnl_write_internal()
 */
esp_err_t jrnl_reset_master(esp_jrnl_instance_t* jrnl, bool fs_direct);

/**
 * @brief Applies all the operations stored in the FS journal log in the same order as they appeared.
 * Called on each transaction commit (esp_jrnl_stop()) or during esp_jrnl_mount() if user_cfg.replay_journal_after_mount is on)
 *
 * @param inst_ptr  FS journal instance pointer
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG on 'jrnl_log' instance being NULL
 *      - ESP_ERR_NO_MEM can't allocate memory for operation header or data buffer
 *      - ESP_ERR_INVALID_CRC on CRC32 mismatch for operation header or data buffer contents (stored vs calculated)
 *      - errors returned by jrnl_check_handle(), jrnl_read_internal(), jrnl_erase_range_raw() and jrnl_write_raw() functions
 */
esp_err_t jrnl_replay(esp_jrnl_instance_t* jrnl_log);

#ifdef __cplusplus
}
#endif
