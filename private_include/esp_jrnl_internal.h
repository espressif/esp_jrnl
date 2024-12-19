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
#include "esp_jrnl.h"
#include "sdkconfig.h"

/*
 * File-system journaling internal structures and defines
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Journaling transaction status enumeration
 */
typedef enum {
    ESP_JRNL_STATUS_FS_INIT,                /* file-system is being mounted/formatted on the journaled volume */
    ESP_JRNL_STATUS_FS_DIRECT = ESP_JRNL_STATUS_FS_INIT, /* alias for better code readability */
    ESP_JRNL_STATUS_TRANS_READY,            /* fresh new log or the last transaction processed completely */
    ESP_JRNL_STATUS_TRANS_OPEN,             /* journaling transaction running */
    ESP_JRNL_STATUS_TRANS_COMMIT            /* journaling transaction being committed to the target disk */
} esp_jrnl_trans_status_t;

/**
 * @brief Journaling operation record header. Covers one 'disk_write' operation
 * (typical file-system API invokes several 'disk_writes' at various stages)
 */
typedef struct {
    uint32_t target_sector;                 /* target sector number in the filesystem (first sector of the sequence) */
    size_t sector_count;                    /* number of sectors involved in current operation */
    uint32_t crc32_data;                    /* sector data checksum (all sectors in the sequence) */
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
    size_t store_size_sectors;              /* size of journaling store in sectors */
    size_t store_volume_offset_sector;      /* index of the first journaling store sector within the volume */
    uint32_t next_free_sector;              /* next free block. Default = 0 (relative offset in the store space) */
    esp_jrnl_trans_status_t status;         /* transaction status. Default = ESP_JRNL_STATUS_TRANS_READY */
    esp_jrnl_volume_t volume;               /* disk volume properties */
} esp_jrnl_master_t;

/**
 * @brief Runtime configuration of a single journaling store instance. Not stored on the target media, memory only
 */
typedef struct {
    _lock_t trans_lock;
    uint8_t fs_volume_id;                   /* file-system volume ID (PDRV for FatFS) */
    esp_jrnl_diskio_t diskio;               /* disk device access configuration */
    esp_jrnl_master_t master;               /* journal master record for given instance */
#ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE
    uint32_t test_config;                   /* runtime flags for internal testing, 0x0 by default */
#endif
} esp_jrnl_instance_t;

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
uint32_t jrnl_get_target_disk_sector(const esp_jrnl_instance_t* inst_ptr, const uint32_t jrnl_sector);

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
