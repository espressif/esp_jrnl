/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/lock.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "esp_jrnl_internal.h"

#ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE
#include "esp_system.h"
#endif

static const char* TAG = "esp_jrnl";
static _lock_t s_instances_lock;

esp_jrnl_instance_t* s_jrnl_instance_ptrs[JRNL_MAX_HANDLES] = {
        [0 ... JRNL_MAX_HANDLES - 1] = NULL
};

static const char* jrnl_status_to_str(esp_jrnl_trans_status_t status)
{
    switch(status)
    {
        case ESP_JRNL_STATUS_FS_INIT: return "Initialize/FS-direct";
        case ESP_JRNL_STATUS_TRANS_READY: return "Ready";
        case ESP_JRNL_STATUS_TRANS_OPEN: return "Open";
        case ESP_JRNL_STATUS_TRANS_COMMIT: return "Commit";
    }

    return "Unknown";
}

esp_err_t jrnl_check_handle(const esp_jrnl_handle_t handle, const char *func)
{
    if (handle == JRNL_INVALID_HANDLE) {
        ESP_LOGE(TAG, "%s: invalid handle", func);
        return ESP_ERR_INVALID_STATE;
    }

    if (handle >= JRNL_MAX_HANDLES) {
        ESP_LOGE(TAG, "%s: instance[%ld] out of range", func, (int32_t)handle);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_jrnl_instance_ptrs[handle] == NULL) {
        ESP_LOGE(TAG, "%s: instance[%ld] not initialized", func, (int32_t)handle);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

uint32_t jrnl_get_target_disk_sector(const esp_jrnl_instance_t* inst_ptr, const uint32_t jrnl_sector)
{
    return inst_ptr->master.store_volume_offset_sector + jrnl_sector;
}

/* read directly from the instance specific disk device */
esp_err_t jrnl_read_raw(esp_jrnl_instance_t* inst_ptr, size_t src_addr, void *dest, size_t size)
{
    return inst_ptr->diskio.disk_read(inst_ptr->diskio.diskio_ctrl_handle, src_addr, dest, size);
}

/* erase_range directly for the instance specific disk device */
static esp_err_t jrnl_write_raw(esp_jrnl_instance_t* inst_ptr, size_t dest_addr, const void *src, size_t size)
{
    return inst_ptr->diskio.disk_write(inst_ptr->diskio.diskio_ctrl_handle, dest_addr, src, size);
}

/* erase_range directly for the instance specific disk device */
static esp_err_t jrnl_erase_range_raw(esp_jrnl_instance_t* inst_ptr, size_t start_addr, size_t size)
{
    return inst_ptr->diskio.disk_erase_range(inst_ptr->diskio.diskio_ctrl_handle, start_addr, size);
}

esp_err_t jrnl_write_internal(esp_jrnl_instance_t* inst_ptr, const uint8_t *buff, const uint32_t sector, const uint32_t count)
{
    if (inst_ptr == NULL || buff == NULL || sector >= inst_ptr->master.store_size_sectors) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t target_sector = jrnl_get_target_disk_sector(inst_ptr, sector);
    ESP_LOGV(TAG, "jrnl_write_internal - sector=%"PRIu32", target_sector=%"PRIu32", count=%"PRIu32"\n", sector, target_sector, count);

    size_t sector_size = inst_ptr->master.volume.disk_sector_size;

    esp_err_t err = jrnl_erase_range_raw(inst_ptr, target_sector * sector_size, count * sector_size);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "jrnl_erase_range_raw failed (0x%08X)", err);
        return err;
    }

    err = jrnl_write_raw(inst_ptr, target_sector * sector_size, buff, count * sector_size);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "jrnl_write_raw failed (0x%08X)", err);
    }

    return err;
}

/* read 'count' sectors from the journaling store referenced by JRNL index <0, JRNL_SECTOR_COUNT-1>
 * store the data into 'out_buff', which is expected large enough for holding the payload
 */
esp_err_t jrnl_read_internal(esp_jrnl_instance_t* inst_ptr, uint8_t *out_buff, uint32_t sector, uint32_t count)
{
    if (inst_ptr == NULL || out_buff == NULL || sector >= inst_ptr->master.store_size_sectors) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t target_sector = jrnl_get_target_disk_sector(inst_ptr, sector);
    ESP_LOGV(TAG, "jrnl_read_internal - sector=%"PRIu32", target_sector=%"PRIu32", count=%"PRIu32"\n", sector, target_sector, count);

    size_t sector_size = inst_ptr->master.volume.disk_sector_size;

    esp_err_t err = jrnl_read_raw(inst_ptr, target_sector * sector_size, out_buff, count * sector_size);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "jrnl_read_raw failed (0x%08X)", err);
    }

    return err;
}

static void jrnl_delete_instance(esp_jrnl_instance_t* inst_ptr)
{
    if (inst_ptr == NULL) {
        return;
    }
    _lock_close(&inst_ptr->trans_lock);
    free(inst_ptr);
    inst_ptr = NULL;
}

static inline esp_err_t jrnl_update_master(esp_jrnl_instance_t* jrnl, const esp_jrnl_master_t* master)
{
    ESP_LOGD(TAG, "Updating jrnl master record (status: %s)", jrnl_status_to_str(jrnl->master.status));
    return jrnl_write_internal(jrnl, (const uint8_t*)master, jrnl->master.store_size_sectors - 1, 1);
}

/* reset the JRNL master record for given instance
 * the reset applies only to the structure items, the rest of the sector space is expected = 0 */
esp_err_t jrnl_reset_master(esp_jrnl_instance_t* jrnl, bool fs_direct)
{
    ESP_LOGV(TAG, "Resetting jrnl master record");
    if (jrnl == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    jrnl->master.jrnl_magic_mark = JRNL_STORE_MARKER;
    jrnl->master.next_free_sector = 0;
    jrnl->master.status = fs_direct ? ESP_JRNL_STATUS_FS_DIRECT : ESP_JRNL_STATUS_TRANS_READY;

    return jrnl_update_master(jrnl, &jrnl->master);
}

#ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE

//power-off emulation: interrupt the transaction only when some data written to the journal
#define JRNL_TEST_PRELIMINARY_EXIT(flags, msg) \
    if (inst_ptr->master.next_free_sector > 0 && inst_ptr->test_config & flags) { \
        ESP_LOGD(TAG, msg); \
        esp_restart(); \
    }

#define JRNL_TEST_TRANSACTION_SUSPENDED(msg) \
    if (inst_ptr->test_config & ESP_JRNL_TEST_SUSPEND_TRANSACTION) { \
        ESP_LOGD(TAG, msg); \
        return ESP_OK; \
    }
#else
#define JRNL_TEST_PRELIMINARY_EXIT(flags, msg)
#define JRNL_TEST_TRANSACTION_SUSPENDED(msg)
#endif

esp_err_t jrnl_replay(esp_jrnl_instance_t* inst_ptr)
{
    ESP_LOGV(TAG, "Replaying journaled log");

    if (inst_ptr == NULL) {
        ESP_LOGE(TAG, "jrnl_replay - NULL journal log instance, operation aborted");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    _lock_acquire(&inst_ptr->trans_lock);

#ifdef CONFIG_ESP_JRNL_DEBUG_PRINT
    print_jrnl_instance(inst_ptr);
#endif

    assert(inst_ptr->master.status != ESP_JRNL_STATUS_FS_INIT && "Attempt to reply uninitialized journaling store!");

    //clean possibly uncommitted transactions
    if (inst_ptr->master.status != ESP_JRNL_STATUS_TRANS_COMMIT) {

        switch (inst_ptr->master.status)
        {
            case ESP_JRNL_STATUS_TRANS_READY:
                ESP_LOGD(TAG, "jrnl_replay - journaling log empty");
                break;
            case ESP_JRNL_STATUS_TRANS_OPEN:
                {
                    ESP_LOGD(TAG, "jrnl_replay - found unfinished transaction, cleaning journaling log");
                    err = jrnl_reset_master(inst_ptr, false);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to reset journaling master record (0x%08X)", err);
                    }
                }
                break;
            default:
                ESP_LOGD(TAG, "jrnl_replay - invalid journaling log status (%s), operation aborted", jrnl_status_to_str(inst_ptr->master.status));
                err = ESP_ERR_INVALID_STATE;
                break;
        }

        _lock_release(&inst_ptr->trans_lock);
        return err;
    }

    //iterate through stored operation records and try to repeat them all
    uint32_t oper_sector_index = 0;
    uint8_t* data = NULL;
    uint32_t sector_size = inst_ptr->master.volume.disk_sector_size;

    uint8_t* header = (uint8_t *)calloc(1, sector_size);
    if (header == NULL) {
        ESP_LOGE(TAG, "jrnl_replay - operation header buffer allocation failed (0x%08X)", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    while (oper_sector_index < inst_ptr->master.next_free_sector) {

        //read the operation header
        err = jrnl_read_internal(inst_ptr, header, oper_sector_index, 1);
        if (err != ESP_OK) {
            break;
        }

        esp_jrnl_operation_t* oper_header = (esp_jrnl_operation_t*)header;
        uint32_t crc32_header = esp_crc32_le(UINT32_MAX, header, sizeof(esp_jrnl_oper_header_t));
        if (crc32_header != oper_header->crc32_header) {
            err = ESP_ERR_INVALID_CRC;
            ESP_LOGE(TAG, "jrnl_replay - operation header checksum mismatch");
            break;
        }

        data = (uint8_t *)calloc(oper_header->header.sector_count, sector_size);
        if (data == NULL) {
            err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "jrnl_replay - operation data buffer allocation failed");
            break;
        }

        //read the data
        err = jrnl_read_internal(inst_ptr, data, oper_sector_index + 1, oper_header->header.sector_count);
        if (err != ESP_OK) {
            break;
        }

        uint32_t crc32_data = esp_crc32_le(UINT32_MAX, data, oper_header->header.sector_count * sector_size);
        if (crc32_data != oper_header->header.crc32_data) {
            err = ESP_ERR_INVALID_CRC;
            ESP_LOGE(TAG, "jrnl_replay - operation data checksum mismatch");
            break;
        }

        //store the data to the original location
        err = jrnl_erase_range_raw(inst_ptr, oper_header->header.target_sector * sector_size, oper_header->header.sector_count * sector_size);
        if (unlikely(err != ESP_OK)) {
            break;
        }

        JRNL_TEST_PRELIMINARY_EXIT(ESP_JRNL_TEST_REPLAY_ERASE_AND_EXIT, "(jrnl_poweroff_test): Erase first target sector on replay and exit");

        err = jrnl_write_raw(inst_ptr, oper_header->header.target_sector * sector_size, data, oper_header->header.sector_count * sector_size);
        if (unlikely(err != ESP_OK)) {
            break;
        }

        JRNL_TEST_PRELIMINARY_EXIT(ESP_JRNL_TEST_REPLAY_WRITE_AND_EXIT, "(jrnl_poweroff_test): Write first target sector on replay and exit");

        //shift the jrnl store pointer
        oper_sector_index += (1 + oper_header->header.sector_count);
        free(data);
        data = NULL;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jrnl_replay failed (0x%08X)", err);
    }
    else {

        JRNL_TEST_PRELIMINARY_EXIT(ESP_JRNL_TEST_REPLAY_EXIT_BEFORE_CLOSE, "(jrnl_poweroff_test): Exit after transferring all the sectors, leave the transaction unfinished");

        err = jrnl_reset_master(inst_ptr, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reset journaling master record (0x%08X)", err);
        }
    }

    free(header);
    free(data);
    _lock_release(&inst_ptr->trans_lock);

    return err;
}

void print_jrnl_config_extended(const esp_jrnl_config_extended_t *config)
{
    esp_rom_printf("\nJRNL configuration:\n");
    esp_rom_printf("  user_cfg:\n");
    esp_rom_printf("    overwrite_existing: %u\n", config->user_cfg.overwrite_existing);
    esp_rom_printf("    store_size_sectors: %u\n", config->user_cfg.store_size_sectors);
    esp_rom_printf("  fs_volume_id: %u\n", config->fs_volume_id);
    esp_rom_printf("  volume_cfg:\n");
    esp_rom_printf("    volume_size: %u\n", config->volume_cfg.volume_size);
    esp_rom_printf("    disk_sector_size: %u\n", config->volume_cfg.disk_sector_size);
    esp_rom_printf("  diskio_cfg:\n");
    esp_rom_printf("    diskio_ctrl_handle: %d\n", config->diskio_cfg.diskio_ctrl_handle);
    esp_rom_printf("    disk_read: Ox%08X\n", (uint32_t)config->diskio_cfg.disk_read);
    esp_rom_printf("    disk_write: Ox%08X\n", (uint32_t)config->diskio_cfg.disk_write);
    esp_rom_printf("    disk_erase_range: Ox%08X\n", (uint32_t)config->diskio_cfg.disk_erase_range);
}

void print_jrnl_master(const esp_jrnl_master_t* jrnl_master)
{
    esp_rom_printf("\nJRNL master record:\n");
    esp_rom_printf("   jrnl_magic_mark: 0x%08" PRIX32 "\n", jrnl_master->jrnl_magic_mark);
    esp_rom_printf("   store_size_sectors: %" PRIu32 "\n", (uint32_t)jrnl_master->store_size_sectors);
    esp_rom_printf("   next_free_sector: %" PRIu32 "\n", jrnl_master->next_free_sector);
    esp_rom_printf("   status: %s\n", jrnl_status_to_str(jrnl_master->status));
    esp_rom_printf("   volume.volume_size: %" PRIu32 "\n", (uint32_t)jrnl_master->volume.volume_size);
    esp_rom_printf("   volume.store_volume_offset_sector: %" PRIu32 "\n", (uint32_t)jrnl_master->store_volume_offset_sector);
    esp_rom_printf("   volume.disk_sector_size: %" PRIu32 "\n", (uint32_t)jrnl_master->volume.disk_sector_size);
}

void print_jrnl_instance(esp_jrnl_instance_t* inst_ptr)
{
    esp_jrnl_master_t* jrnl_master = &inst_ptr->master;

    print_jrnl_master(jrnl_master);

    //iterate through stored operation records and try to repeat them all
    uint32_t oper_sector_index = 0;
    uint8_t* header = (uint8_t *)calloc(1, jrnl_master->volume.disk_sector_size);
    if (header == NULL) {
        ESP_LOGE(TAG, "print_jrnl_instance failed with error (0x%08X)", ESP_ERR_NO_MEM);
        return;
    }

    //journaling store can be empty
    esp_err_t err = ESP_OK;
    size_t record_count = 0;
    while (oper_sector_index < jrnl_master->next_free_sector) {

        //read the operation header
        err = jrnl_read_internal(inst_ptr, header, oper_sector_index, 1);
        if (err != ESP_OK) {
            break;
        }

        esp_jrnl_operation_t* oper_header = (esp_jrnl_operation_t*)header;
        uint32_t crc32_header = esp_crc32_le(UINT32_MAX, header, sizeof(esp_jrnl_oper_header_t));
        if (crc32_header != oper_header->crc32_header) {
            err = ESP_ERR_INVALID_CRC;
            ESP_LOGE(TAG, "print_jrnl_instance - operation header checksum mismatch, aborting");
            break;
        }

        //print the header
        esp_rom_printf("\n   OPER.HEADER %u:\n", record_count);
        esp_rom_printf("      header.target_sector: %" PRIu32 "\n", oper_header->header.target_sector);
        esp_rom_printf("      header.sector_count: %" PRIu32 "\n", oper_header->header.sector_count);
        esp_rom_printf("      header.crc32_data: Ox%08X\n", oper_header->header.crc32_data);
        esp_rom_printf("      crc32_header: Ox%08X\n", oper_header->crc32_header);

        oper_sector_index += (1 + oper_header->header.sector_count);
        record_count++;
    }

    free(header);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "print_jrnl_instance failed with error (0x%08X)", err);
    }
}


/*
 * PUBLIC APIS
 */

esp_err_t esp_jrnl_mount(const esp_jrnl_config_extended_t *config, esp_jrnl_handle_t *jrnl_handle)
{
    ESP_LOGV(TAG, "Mounting journaling store...");

    //sanity check
    if (config == NULL ||
        jrnl_handle == NULL ||
        config->user_cfg.store_size_sectors < JRNL_MIN_STORE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    _lock_acquire(&s_instances_lock);

    //find first available handle
    esp_jrnl_handle_t out_handle = JRNL_INVALID_HANDLE;
    for (size_t i=0; i<JRNL_MAX_HANDLES; i++) {
        if (s_jrnl_instance_ptrs[i] == NULL) {
            out_handle = i;
            break;
        }
    }

    ESP_LOGV(TAG, "jrnl handle: %" PRIu32, out_handle);

    esp_err_t err = ESP_OK;
    esp_jrnl_instance_t* jrnl = NULL;

    do {
        //no more handles
        if (out_handle == JRNL_INVALID_HANDLE) {
            ESP_LOGE(TAG, "JRNL_MAX_HANDLES=%d instances already allocated", JRNL_MAX_HANDLES);
            err = ESP_ERR_NO_MEM;
            break;
        }

        //create a new journaling instance for given volume
        jrnl = (esp_jrnl_instance_t *) calloc(1, sizeof(esp_jrnl_instance_t));
        if (jrnl == NULL) {
            err = ESP_ERR_NO_MEM;
            break;
        }

        //init the instance
        _lock_init(&jrnl->trans_lock);
        jrnl->fs_volume_id = config->fs_volume_id;
        jrnl->diskio = config->diskio_cfg;

        ESP_LOGV(TAG, "jrnl volume ID: %" PRIu8", total volume size: %" PRIu32 ", disk_sector_size: %" PRIu32 ", master record address: %" PRIu32,
                 jrnl->fs_volume_id, (uint32_t)config->volume_cfg.volume_size, (uint32_t)config->volume_cfg.disk_sector_size, (uint32_t)(config->volume_cfg.volume_size - config->volume_cfg.disk_sector_size));

        //check possibly uncommitted transaction stored in the journal, unless configured to ignore all journaled data
        bool need_fresh_journal = config->user_cfg.force_fs_format || config->user_cfg.overwrite_existing;
        if (!need_fresh_journal) {

            //master record == the last sector before WL section
            err = jrnl_read_raw(jrnl, config->volume_cfg.volume_size - config->volume_cfg.disk_sector_size, (void*)&jrnl->master, sizeof(esp_jrnl_master_t));
            if (unlikely(err != ESP_OK)) {
                ESP_LOGE(TAG, "Failed to read journal master record from disk (err 0x%08X)", err);
                break;
            }

            //ensure the record validity and replay the journal, if any (MV!!!: no way to recognise whether the record is corrupted or missing completely - add extra feature?)
            if (jrnl->master.jrnl_magic_mark == JRNL_STORE_MARKER) {

                ESP_LOGV(TAG, "Found valid journal record, verifying consistency...");

                if (!(config->volume_cfg.volume_size == jrnl->master.volume.volume_size &&
                       config->volume_cfg.disk_sector_size == jrnl->master.volume.disk_sector_size &&
                       config->user_cfg.store_size_sectors == jrnl->master.store_size_sectors)) {
                    ESP_LOGE(TAG, "Journaling configuration inconsistent with found jrnl master record (record corrupted?)");
                    err = ESP_ERR_INVALID_STATE;
                    break;
                }

                //repeat open JRNL transaction, if any
                if (config->user_cfg.replay_journal_after_mount) {
                    err = jrnl_replay(jrnl);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to replay stored journal log (0x%08X)", err);
                        break;
                    }
                    ESP_LOGV(TAG, "Journaling store successfully resumed from disk");
                } else {
                    ESP_LOGV(TAG, "Journaling store configured to stay not replayed");
                }
            }
            else {
                ESP_LOGV(TAG, "No valid journaling record found");
            }
        }

        ESP_LOGV(TAG, "Creating fresh journaling store...");

        jrnl->master.store_size_sectors = config->user_cfg.store_size_sectors;
        jrnl->master.store_volume_offset_sector = config->volume_cfg.volume_size/config->volume_cfg.disk_sector_size - config->user_cfg.store_size_sectors;
        jrnl->master.volume = config->volume_cfg;

        //journal instance created with ESP_JRNL_STATUS_FS_INIT status
        err = jrnl_reset_master(jrnl, need_fresh_journal);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reset journaling master record (0x%08X)", err);
            break;
        }

        //add the new instance handle to the list and provide it to the caller
        s_jrnl_instance_ptrs[out_handle] = jrnl;
        *jrnl_handle = out_handle;

    } while(0);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_jrnl_mount failed (0x%08X)", err);
        jrnl_delete_instance(jrnl);
    }
    else {
        ESP_LOGV(TAG, "esp_jrnl_mount succeeded (handle: %ld)", *jrnl_handle);
    }

    _lock_release(&s_instances_lock);

    return err;
}

esp_err_t esp_jrnl_unmount(const esp_jrnl_handle_t handle)
{
    ESP_LOGV(TAG, "esp_jrnl_unmount (handle: %ld)", handle);

    _lock_acquire(&s_instances_lock);

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }
    jrnl_delete_instance(s_jrnl_instance_ptrs[handle]);
    s_jrnl_instance_ptrs[handle] = NULL;

    _lock_release(&s_instances_lock);

    return ESP_OK;
}

esp_err_t esp_jrnl_start(const esp_jrnl_handle_t handle)
{
    ESP_LOGD(TAG, "esp_jrnl_start (handle: %ld)", handle);

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[handle];
    JRNL_TEST_TRANSACTION_SUSPENDED("esp_jrnl_start() suspended");

    _lock_acquire(&inst_ptr->trans_lock);

    ESP_LOGD(TAG, "esp_jrnl_start (current status: %s)", jrnl_status_to_str(inst_ptr->master.status));

    if (inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_READY) {

        assert(inst_ptr->master.next_free_sector == 0);
        inst_ptr->master.status = ESP_JRNL_STATUS_TRANS_OPEN;

        //update JRNL status on disk
        ESP_LOGV(TAG, "JRNL transaction open, updating master record");
        err = jrnl_update_master(inst_ptr, &inst_ptr->master);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jrnl_write_internal failed (0x%08X)", err);
        }
    }
    else {
        err = ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG, "Can't open new journaling transaction (status=%s, err=0x%08X)", jrnl_status_to_str(inst_ptr->master.status), err);
    }

    _lock_release(&inst_ptr->trans_lock);

    return err;
}

//MV2DO: locking logic needs revamping (separate task to support multithreading)
esp_err_t esp_jrnl_stop(const esp_jrnl_handle_t handle, const bool commit)
{
    ESP_LOGD(TAG, "esp_jrnl_stop (handle: %ld, commit: %u)", handle, commit);

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[handle];
    JRNL_TEST_TRANSACTION_SUSPENDED("esp_jrnl_stop() suspended");

    //cancel the transaction
    if (!commit) {
        ESP_LOGV(TAG, "Canceling current JRNL transaction");

        _lock_acquire(&inst_ptr->trans_lock);
        err = jrnl_reset_master(inst_ptr, false);
        _lock_release(&inst_ptr->trans_lock);
        return err;
    }

    JRNL_TEST_PRELIMINARY_EXIT(ESP_JRNL_TEST_STOP_SKIP_COMMIT, "(jrnl_poweroff_test): Skip committing of the current JRNL transaction");

    if (inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_OPEN) {

        //start committing the transaction to the disk
        ESP_LOGV(TAG, "Committing current JRNL transaction");

        _lock_acquire(&inst_ptr->trans_lock);
        inst_ptr->master.status = ESP_JRNL_STATUS_TRANS_COMMIT;
        err = jrnl_update_master(inst_ptr, &inst_ptr->master);
        _lock_release(&inst_ptr->trans_lock);

        JRNL_TEST_PRELIMINARY_EXIT(ESP_JRNL_TEST_STOP_SET_COMMIT_AND_EXIT, "(jrnl_poweroff_test): Set commit status to JRNL header and exit");

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jrnl_write_internal failed (0x%08X)", err);
            return err;
        }

        //transfer the operations from JRNL store to the target disk
        err = jrnl_replay(inst_ptr);
    }
    else {
        err = ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG, "Journaling transaction not open (0x%08X)", err);
    }

    return err;
}

esp_err_t esp_jrnl_get_diskio_handle(const esp_jrnl_handle_t handle, int32_t* diskio_ctrl_handle)
{
    if (diskio_ctrl_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }

    *diskio_ctrl_handle = s_jrnl_instance_ptrs[handle]->diskio.diskio_ctrl_handle;

    return ESP_OK;
}

esp_err_t esp_jrnl_get_sector_count(const esp_jrnl_handle_t handle, size_t* fs_part_sector_count)
{
    if (fs_part_sector_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[handle];
    *fs_part_sector_count = inst_ptr->master.volume.volume_size / inst_ptr->master.volume.disk_sector_size - inst_ptr->master.store_size_sectors;

    return ESP_OK;
}

esp_err_t esp_jrnl_get_sector_size(const esp_jrnl_handle_t handle, size_t* sector_size)
{
    if (sector_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[handle];
    *sector_size = inst_ptr->master.volume.disk_sector_size;

    return ESP_OK;
}

esp_err_t esp_jrnl_set_direct_io(const esp_jrnl_handle_t handle, bool direct_access)
{
    ESP_LOGV(TAG, "esp_jrnl_set_direct_io (handle: %ld, on: %u)", handle, direct_access);

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[handle];
    _lock_acquire(&inst_ptr->trans_lock);

    //direct FS access switching cannot be required during a transaction lifetime
    if (inst_ptr->master.status != ESP_JRNL_STATUS_FS_DIRECT && inst_ptr->master.status != ESP_JRNL_STATUS_TRANS_READY) {
        err = ESP_ERR_INVALID_STATE;
    }
    else {
        inst_ptr->master.status = direct_access ? ESP_JRNL_STATUS_FS_DIRECT : ESP_JRNL_STATUS_TRANS_READY;
        err = jrnl_update_master(inst_ptr, &inst_ptr->master);
    }
    _lock_release(&inst_ptr->trans_lock);

    return err;
}

esp_err_t esp_jrnl_write(const esp_jrnl_handle_t handle, const uint8_t *buff, const uint32_t sector, const uint32_t count)
{
    ESP_LOGV(TAG, "esp_jrnl_write (handle: %ld)", handle);

    if (buff == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[handle];
    uint32_t sector_size = inst_ptr->master.volume.disk_sector_size;

    //allow direct disk access when FS is being formatted or for testing reasons
    if (inst_ptr->master.status == ESP_JRNL_STATUS_FS_DIRECT) {
        ESP_LOGV(TAG, "esp_jrnl_write (handle: %ld) - direct write", handle);
        err = jrnl_erase_range_raw(inst_ptr, sector * sector_size, count * sector_size);
        if (err == ESP_OK) {
            err = jrnl_write_raw(inst_ptr, sector * sector_size, buff, count * sector_size);
        }
        return err;
    }

    //write to the journaling store only if a transaction is open
    if (inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_OPEN) {

        //operation: header sector + count*[data sector]
        if ((inst_ptr->master.next_free_sector + 1 + count) < (inst_ptr->master.store_size_sectors - 1)) {

            esp_jrnl_operation_t *oper_header = NULL;
            _lock_acquire(&inst_ptr->trans_lock);

            do {
                //create header
                oper_header = (esp_jrnl_operation_t *) calloc(1, sector_size);
                if (oper_header == NULL) {
                    err = ESP_ERR_NO_MEM;
                    ESP_LOGE(TAG, "esp_jrnl_write failed (can't allocate the operation header, 0x%08X)", err);
                    break;
                }
                oper_header->header.target_sector = sector;
                oper_header->header.sector_count = count;
                oper_header->header.crc32_data = esp_crc32_le(UINT32_MAX, buff, count * sector_size);
                oper_header->crc32_header = esp_crc32_le(UINT32_MAX, (uint8_t *) &oper_header->header, sizeof(esp_jrnl_oper_header_t));

                size_t oper_addr =
                        jrnl_get_target_disk_sector(inst_ptr, inst_ptr->master.next_free_sector) * sector_size;
                size_t oper_size = (count + 1) * sector_size;

                ESP_LOGV(TAG, "Writing jrnl oper header+data at sector %" PRIu32 " (size %" PRIu32 ")", sector, count);

                //clean 1 + count
                err = jrnl_erase_range_raw(inst_ptr, oper_addr, oper_size);
                if (unlikely(err != ESP_OK)) {
                    ESP_LOGE(TAG, "esp_jrnl_write failed (jrnl_erase_range_raw(): 0x%08X)", err);
                    break;
                }

                //write header
                err = jrnl_write_raw(inst_ptr, oper_addr, (void *) oper_header, sector_size);
                if (unlikely(err != ESP_OK)) {
                    ESP_LOGE(TAG, "esp_jrnl_write failed (jrnl_write_raw(): 0x%08X)", err);
                    break;
                }

                //write data
                err = jrnl_write_raw(inst_ptr, oper_addr + sector_size, (void *) buff, oper_size - sector_size);
                if (unlikely(err != ESP_OK)) {
                    ESP_LOGE(TAG, "esp_jrnl_write failed (jrnl_write_raw(): 0x%08X)", err);
                    break;
                }

                //update jrnl record
                inst_ptr->master.next_free_sector += (1 + count);
                err = jrnl_update_master(inst_ptr, &inst_ptr->master);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "jrnl_write_internal() failed (0x%08X)", err);
                }
            } while(false);

            _lock_release(&inst_ptr->trans_lock);
            free(oper_header);

            if (err != ESP_OK) {
                return err;
            }
        }
        else {
            ESP_LOGE(TAG, "esp_jrnl_write failed (not enough space to complete the operation, 0x%08X)", ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }
    }
    else {
        //any other case must fail to avoid JRNL corruption
        ESP_LOGE(TAG, "esp_jrnl_write() failed due to invalid transaction status (0x%08X)", inst_ptr->master.status);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

//public reading API (redirection to wl_read)
esp_err_t esp_jrnl_read(const esp_jrnl_handle_t handle, uint32_t sector, uint8_t *dest, uint32_t count)
{
    if (dest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[handle];
    uint32_t sector_size = inst_ptr->master.volume.disk_sector_size;

    //boundary check
    if ((sector + count) >= inst_ptr->master.store_volume_offset_sector) {
        return ESP_ERR_INVALID_SIZE;
    }

    return jrnl_read_raw(inst_ptr, sector * sector_size, dest, count * sector_size);
}