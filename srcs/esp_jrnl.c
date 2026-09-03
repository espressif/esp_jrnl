/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/lock.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "esp_blockdev.h"
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

uint64_t jrnl_get_target_disk_sector(const esp_jrnl_instance_t* inst_ptr, const uint32_t jrnl_sector)
{
    return (uint64_t)inst_ptr->master.store_volume_offset_sector + jrnl_sector;
}

/*
 * Raw I/O helpers route to the BDL device pointed to by inst_ptr->bottom_bdl.
 *
 * jrnl_erase_range_raw() is a no-op for media that does not require an erase before write
 * (bottom BDL flag erase_before_write == 0) so that the same journal core works correctly on
 * SD/MMC, USB MSC and NAND-with-FTL backends where host-driven erase is either unsupported or
 * meaningless. SPI NOR backends (and the legacy callback-bridge BDL, which is initialised with
 * SPI Flash defaults) keep the historical erase-then-write behaviour.
 */
esp_err_t jrnl_read_raw(esp_jrnl_instance_t* inst_ptr, uint64_t src_addr, void *dest, size_t size)
{
    return inst_ptr->bottom_bdl->ops->read(inst_ptr->bottom_bdl, (uint8_t*)dest, size, src_addr, size);
}

static esp_err_t jrnl_write_raw(esp_jrnl_instance_t* inst_ptr, uint64_t dest_addr, const void *src, size_t size)
{
    return inst_ptr->bottom_bdl->ops->write(inst_ptr->bottom_bdl, (const uint8_t*)src, dest_addr, size);
}

static esp_err_t jrnl_erase_range_raw(esp_jrnl_instance_t* inst_ptr, uint64_t start_addr, size_t size)
{
    if (!inst_ptr->bottom_bdl->device_flags.erase_before_write) {
        return ESP_OK;
    }
    return inst_ptr->bottom_bdl->ops->erase(inst_ptr->bottom_bdl, start_addr, size);
}

esp_err_t jrnl_write_internal(esp_jrnl_instance_t* inst_ptr, const uint8_t *buff, const uint32_t sector, const uint32_t count)
{
    if (inst_ptr == NULL || buff == NULL || sector >= inst_ptr->master.store_size_sectors) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t target_sector = jrnl_get_target_disk_sector(inst_ptr, sector);
    ESP_LOGV(TAG, "jrnl_write_internal - sector=%"PRIu32", target_sector=%"PRIu64", count=%"PRIu32"\n", sector, target_sector, count);

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

    uint64_t target_sector = jrnl_get_target_disk_sector(inst_ptr, sector);
    ESP_LOGV(TAG, "jrnl_read_internal - sector=%"PRIu32", target_sector=%"PRIu64", count=%"PRIu32"\n", sector, target_sector, count);

    size_t sector_size = inst_ptr->master.volume.disk_sector_size;

    esp_err_t err = jrnl_read_raw(inst_ptr, target_sector * sector_size, out_buff, count * sector_size);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "jrnl_read_raw failed (0x%08X)", err);
    }

    return err;
}

void jrnl_instance_destroy(esp_jrnl_instance_t* inst_ptr)
{
    if (inst_ptr == NULL) {
        return;
    }
    /* Local-only teardown: do not give window_sem (a waiter would then race a deleted object).
     * The on-disk master is left untouched; a dangling OPEN transaction is discarded by
     * jrnl_replay() on the next mount. */
    if (inst_ptr->window_sem != NULL) {
        vSemaphoreDelete(inst_ptr->window_sem);
        inst_ptr->window_sem = NULL;
    }
    inst_ptr->window_owner = NULL;
    _lock_close(&inst_ptr->trans_lock);

    /* Only release the bottom BDL when we own it (legacy callback-bridge case).
     * In the BDL factory path the caller retains ownership and is responsible for
     * releasing the underlying device. */
    if (inst_ptr->owns_bottom_bdl && inst_ptr->bottom_bdl != ESP_BLOCKDEV_HANDLE_INVALID) {
        if (inst_ptr->bottom_bdl->ops && inst_ptr->bottom_bdl->ops->release) {
            inst_ptr->bottom_bdl->ops->release(inst_ptr->bottom_bdl);
        }
    }
    inst_ptr->bottom_bdl = ESP_BLOCKDEV_HANDLE_INVALID;
    free(inst_ptr);
}

/* Convenience wrapper preserved so existing call sites keep their meaning. */
static inline void jrnl_delete_instance(esp_jrnl_instance_t* inst_ptr)
{
    jrnl_instance_destroy(inst_ptr);
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
    jrnl->master.format_version = JRNL_STORE_FORMAT_VERSION;
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
        _lock_release(&inst_ptr->trans_lock);   /* [mass-production fix #2] avoid leaving trans_lock held on OOM */
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

        const uint64_t target_addr = oper_header->header.target_sector * sector_size;
        const size_t op_bytes = (size_t)oper_header->header.sector_count * sector_size;

        if (oper_header->header.op_type == ESP_JRNL_OP_ERASE) {
            err = jrnl_erase_range_raw(inst_ptr, target_addr, op_bytes);
            if (unlikely(err != ESP_OK)) {
                break;
            }
            JRNL_TEST_PRELIMINARY_EXIT(ESP_JRNL_TEST_REPLAY_ERASE_AND_EXIT, "(jrnl_poweroff_test): Erase first target sector on replay and exit");
            oper_sector_index += 1;
            continue;
        }

        if (oper_header->header.op_type != ESP_JRNL_OP_WRITE) {
            err = ESP_ERR_INVALID_STATE;
            ESP_LOGE(TAG, "jrnl_replay - unknown operation type %" PRIu32, oper_header->header.op_type);
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
        err = jrnl_erase_range_raw(inst_ptr, target_addr, op_bytes);
        if (unlikely(err != ESP_OK)) {
            break;
        }

        JRNL_TEST_PRELIMINARY_EXIT(ESP_JRNL_TEST_REPLAY_ERASE_AND_EXIT, "(jrnl_poweroff_test): Erase first target sector on replay and exit");

        err = jrnl_write_raw(inst_ptr, target_addr, data, op_bytes);
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
    esp_rom_printf("    volume_size: %llu\n", (unsigned long long)config->volume_cfg.volume_size);
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
    esp_rom_printf("   format_version: %" PRIu32 "\n", jrnl_master->format_version);
    esp_rom_printf("   store_size_sectors: %" PRIu32 "\n", jrnl_master->store_size_sectors);
    esp_rom_printf("   next_free_sector: %" PRIu32 "\n", jrnl_master->next_free_sector);
    esp_rom_printf("   status: %s\n", jrnl_status_to_str(jrnl_master->status));
    esp_rom_printf("   volume.volume_size: %llu\n", (unsigned long long)jrnl_master->volume.volume_size);
    esp_rom_printf("   volume.store_volume_offset_sector: %llu\n", (unsigned long long)jrnl_master->store_volume_offset_sector);
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
        esp_rom_printf("      header.target_sector: %llu\n", (unsigned long long)oper_header->header.target_sector);
        esp_rom_printf("      header.sector_count: %" PRIu32 "\n", oper_header->header.sector_count);
        esp_rom_printf("      header.op_type: %" PRIu32 "\n", oper_header->header.op_type);
        esp_rom_printf("      header.crc32_data: Ox%08X\n", oper_header->header.crc32_data);
        esp_rom_printf("      crc32_header: Ox%08X\n", oper_header->crc32_header);

        oper_sector_index += (oper_header->header.op_type == ESP_JRNL_OP_ERASE)
                             ? 1 : (1 + oper_header->header.sector_count);
        record_count++;
    }

    free(header);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "print_jrnl_instance failed with error (0x%08X)", err);
    }
}


static esp_err_t jrnl_window_take(esp_jrnl_instance_t *inst_ptr)
{
    if (inst_ptr->window_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(inst_ptr->window_sem, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void jrnl_window_give(esp_jrnl_instance_t *inst_ptr)
{
    if (inst_ptr->window_sem != NULL) {
        xSemaphoreGive(inst_ptr->window_sem);
    }
}

esp_err_t jrnl_inst_trans_start(esp_jrnl_instance_t *inst_ptr)
{
    if (inst_ptr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    JRNL_TEST_TRANSACTION_SUSPENDED("esp_jrnl_start() suspended");

    /* Nested START from the task that already owns the window would deadlock on take. */
    if (inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_OPEN &&
        inst_ptr->window_owner == xTaskGetCurrentTaskHandle()) {
        ESP_LOGE(TAG, "Can't open new journaling transaction (status=%s, err=0x%08X)",
                 jrnl_status_to_str(inst_ptr->master.status), ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = jrnl_window_take(inst_ptr);
    if (err != ESP_OK) {
        return err;
    }

    _lock_acquire(&inst_ptr->trans_lock);

    ESP_LOGD(TAG, "esp_jrnl_start (current status: %s)", jrnl_status_to_str(inst_ptr->master.status));

    if (inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_READY) {
        assert(inst_ptr->master.next_free_sector == 0);
        inst_ptr->master.status = ESP_JRNL_STATUS_TRANS_OPEN;
        ESP_LOGV(TAG, "JRNL transaction open, updating master record");
        err = jrnl_update_master(inst_ptr, &inst_ptr->master);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jrnl_write_internal failed (0x%08X)", err);
            inst_ptr->master.status = ESP_JRNL_STATUS_TRANS_READY;
        } else {
            inst_ptr->window_owner = xTaskGetCurrentTaskHandle();
        }
    } else {
        err = ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG, "Can't open new journaling transaction (status=%s, err=0x%08X)",
                 jrnl_status_to_str(inst_ptr->master.status), err);
    }

    _lock_release(&inst_ptr->trans_lock);

    if (err != ESP_OK) {
        jrnl_window_give(inst_ptr);
    }

    return err;
}

esp_err_t jrnl_inst_trans_stop(esp_jrnl_instance_t *inst_ptr, bool commit)
{
    if (inst_ptr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    JRNL_TEST_TRANSACTION_SUSPENDED("esp_jrnl_stop() suspended");

    /* Claim the open window under trans_lock so a concurrent COMMIT/CANCEL cannot
     * both give window_sem. The second observer sees a non-OPEN status and returns
     * INVALID_STATE without touching the semaphore. */
    _lock_acquire(&inst_ptr->trans_lock);
    if (inst_ptr->master.status != ESP_JRNL_STATUS_TRANS_OPEN) {
        _lock_release(&inst_ptr->trans_lock);
        ESP_LOGE(TAG, "Journaling transaction not open (0x%08X)", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    if (!commit) {
        ESP_LOGV(TAG, "Canceling current JRNL transaction");
        esp_err_t err = jrnl_reset_master(inst_ptr, false);
        _lock_release(&inst_ptr->trans_lock);
        inst_ptr->window_owner = NULL;
        jrnl_window_give(inst_ptr);
        return err;
    }

    JRNL_TEST_PRELIMINARY_EXIT(ESP_JRNL_TEST_STOP_SKIP_COMMIT, "(jrnl_poweroff_test): Skip committing of the current JRNL transaction");

    ESP_LOGV(TAG, "Committing current JRNL transaction");
    inst_ptr->master.status = ESP_JRNL_STATUS_TRANS_COMMIT;
    esp_err_t err = jrnl_update_master(inst_ptr, &inst_ptr->master);
    _lock_release(&inst_ptr->trans_lock);

    JRNL_TEST_PRELIMINARY_EXIT(ESP_JRNL_TEST_STOP_SET_COMMIT_AND_EXIT, "(jrnl_poweroff_test): Set commit status to JRNL header and exit");

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jrnl_write_internal failed (0x%08X)", err);
        inst_ptr->window_owner = NULL;
        jrnl_window_give(inst_ptr);
        return err;
    }

    err = jrnl_replay(inst_ptr);
    inst_ptr->window_owner = NULL;
    jrnl_window_give(inst_ptr);
    return err;
}

esp_err_t jrnl_inst_set_direct_io(esp_jrnl_instance_t *inst_ptr, bool direct_access)
{
    if (inst_ptr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Same-task call during our own window would deadlock on take. */
    if (inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_OPEN &&
        inst_ptr->window_owner == xTaskGetCurrentTaskHandle()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = jrnl_window_take(inst_ptr);
    if (err != ESP_OK) {
        return err;
    }

    _lock_acquire(&inst_ptr->trans_lock);

    if (inst_ptr->master.status != ESP_JRNL_STATUS_FS_DIRECT && inst_ptr->master.status != ESP_JRNL_STATUS_TRANS_READY) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        inst_ptr->master.status = direct_access ? ESP_JRNL_STATUS_FS_DIRECT : ESP_JRNL_STATUS_TRANS_READY;
        err = jrnl_update_master(inst_ptr, &inst_ptr->master);
    }

    _lock_release(&inst_ptr->trans_lock);
    jrnl_window_give(inst_ptr);
    return err;
}

esp_err_t jrnl_inst_log_or_implicit(esp_jrnl_instance_t *inst_ptr,
                                    const uint8_t *buff,
                                    uint64_t sector,
                                    uint32_t count)
{
    if (inst_ptr == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool is_erase = (buff == NULL);

    if (inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_OPEN) {
        return is_erase ? jrnl_log_erase(inst_ptr, sector, count)
                        : jrnl_log_write(inst_ptr, buff, sector, count);
    }

    if (inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_READY && inst_ptr->implicit_transactions) {
        esp_err_t err = jrnl_inst_trans_start(inst_ptr);
        if (err != ESP_OK) {
            return err;
        }
        err = is_erase ? jrnl_log_erase(inst_ptr, sector, count)
                       : jrnl_log_write(inst_ptr, buff, sector, count);
        esp_err_t stop_err = jrnl_inst_trans_stop(inst_ptr, err == ESP_OK);
        return (err != ESP_OK) ? err : stop_err;
    }

    ESP_LOGE(TAG, "journaled %s rejected: status=%s",
             is_erase ? "erase" : "write", jrnl_status_to_str(inst_ptr->master.status));
    return ESP_ERR_INVALID_STATE;
}

/*
 * PUBLIC APIS
 */

esp_err_t jrnl_instance_create(const esp_jrnl_config_t *user_cfg,
                               esp_blockdev_handle_t bottom_bdl,
                               bool owns_bdl,
                               const esp_jrnl_diskio_t *diskio,
                               uint8_t fs_volume_id,
                               esp_jrnl_instance_t **out_instance)
{
    if (user_cfg == NULL || bottom_bdl == ESP_BLOCKDEV_HANDLE_INVALID || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (user_cfg->store_size_sectors < JRNL_MIN_STORE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Derive the journal's internal record unit ("sector") from the bottom BDL geometry.
     * esp_jrnl is pure BDL middleware and trusts the values the bottom device reports:
     *  - erase-before-write media (SPI NOR, the legacy callback-bridge): a journal record must
     *    be erasable and rewritable independently, so the record unit is the erase block. The
     *    device's (possibly finer) write_size must divide the erase block so the journal can
     *    still emit erase-block-sized writes; sub-multiple write granularity (the usual NOR
     *    case, write_size 1 or 16 with a 4 kB erase block) is fully supported.
     *  - media without host-driven erase (SD/MMC, USB MSC, NAND+FTL): there is no erase
     *    constraint, so the record unit is simply the write granularity. */
    size_t sector_size;
    if (bottom_bdl->device_flags.erase_before_write) {
        sector_size = bottom_bdl->geometry.erase_size;
        if (sector_size == 0) {
            ESP_LOGE(TAG, "%s: bottom_bdl has zero erase_size on erase-before-write media", __func__);
            return ESP_ERR_INVALID_ARG;
        }
        if (bottom_bdl->geometry.write_size == 0 || (sector_size % bottom_bdl->geometry.write_size) != 0) {
            ESP_LOGE(TAG, "%s: erase-before-write media write_size (%u) must be a non-zero divisor of erase_size (%u)",
                     __func__, (unsigned)bottom_bdl->geometry.write_size, (unsigned)sector_size);
            return ESP_ERR_NOT_SUPPORTED;
        }
    } else {
        sector_size = bottom_bdl->geometry.write_size;
        if (sector_size == 0) {
            ESP_LOGE(TAG, "%s: bottom_bdl has zero write_size", __func__);
            return ESP_ERR_INVALID_ARG;
        }
    }

    const uint64_t volume_size = bottom_bdl->geometry.disk_size;
    if (volume_size == 0 || (volume_size % sector_size) != 0) {
        ESP_LOGE(TAG, "%s: bottom_bdl disk_size (%llu) is not a positive multiple of the journal unit (%u)",
                 __func__, (unsigned long long)volume_size, (unsigned)sector_size);
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t total_sectors = volume_size / sector_size;
    if (user_cfg->store_size_sectors >= total_sectors) {
        ESP_LOGE(TAG, "%s: store_size_sectors (%u) >= total sectors on bottom_bdl (%llu)",
                 __func__, (unsigned)user_cfg->store_size_sectors, (unsigned long long)total_sectors);
        return ESP_ERR_INVALID_ARG;
    }

    *out_instance = NULL;

    esp_jrnl_instance_t *jrnl = (esp_jrnl_instance_t *)calloc(1, sizeof(esp_jrnl_instance_t));
    if (jrnl == NULL) {
        return ESP_ERR_NO_MEM;
    }

    _lock_init(&jrnl->trans_lock);
    jrnl->window_sem = xSemaphoreCreateBinary();
    if (jrnl->window_sem == NULL) {
        _lock_close(&jrnl->trans_lock);
        free(jrnl);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(jrnl->window_sem);
    jrnl->window_owner = NULL;
    jrnl->implicit_transactions = user_cfg->implicit_transactions;
    jrnl->bottom_bdl = bottom_bdl;
    jrnl->owns_bottom_bdl = owns_bdl;
    jrnl->fs_volume_id = fs_volume_id;
    if (diskio != NULL) {
        jrnl->diskio = *diskio;
    }

    ESP_LOGV(TAG, "Mounting journaling store: volume ID %u, volume size %llu, sector size %u, master record address %llu",
             (unsigned)fs_volume_id, (unsigned long long)volume_size, (unsigned)sector_size,
             (unsigned long long)(volume_size - sector_size));

    esp_err_t err = ESP_OK;

    do {
        bool need_fresh_journal = user_cfg->force_fs_format || user_cfg->overwrite_existing;

        if (!need_fresh_journal) {
            /* master record is the last sector of the volume */
            err = jrnl_read_raw(jrnl, volume_size - sector_size, (void*)&jrnl->master, sizeof(esp_jrnl_master_t));
            if (unlikely(err != ESP_OK)) {
                ESP_LOGE(TAG, "Failed to read journal master record from disk (err 0x%08X)", err);
                break;
            }

            if (jrnl->master.jrnl_magic_mark == JRNL_STORE_MARKER) {
                ESP_LOGV(TAG, "Found valid journal record, verifying consistency...");

                if (jrnl->master.format_version != JRNL_STORE_FORMAT_VERSION) {
                    ESP_LOGE(TAG, "Unsupported journal store format version %" PRIu32 " (expected %u)",
                             jrnl->master.format_version, (unsigned)JRNL_STORE_FORMAT_VERSION);
                    err = ESP_ERR_INVALID_STATE;
                    break;
                }

                if (!(volume_size == jrnl->master.volume.volume_size &&
                      sector_size == jrnl->master.volume.disk_sector_size &&
                      user_cfg->store_size_sectors == jrnl->master.store_size_sectors)) {
                    ESP_LOGE(TAG, "Journaling configuration inconsistent with found jrnl master record (record corrupted?)");
                    err = ESP_ERR_INVALID_STATE;
                    break;
                }

                if (user_cfg->replay_journal_after_mount) {
                    err = jrnl_replay(jrnl);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to replay stored journal log (0x%08X)", err);
                        break;
                    }
                    ESP_LOGV(TAG, "Journaling store successfully resumed from disk");
                } else {
                    ESP_LOGV(TAG, "Journaling store configured to stay not replayed");
                }
            } else {
                ESP_LOGV(TAG, "No valid journaling record found");
            }
        }

        ESP_LOGV(TAG, "Creating fresh journaling store...");

        jrnl->master.store_size_sectors = user_cfg->store_size_sectors;
        jrnl->master.store_volume_offset_sector = total_sectors - user_cfg->store_size_sectors;
        jrnl->master.volume.volume_size = volume_size;
        jrnl->master.volume.disk_sector_size = sector_size;

        err = jrnl_reset_master(jrnl, need_fresh_journal);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reset journaling master record (0x%08X)", err);
            break;
        }
    } while (0);

    if (err != ESP_OK) {
        /* Release the instance only; bottom_bdl ownership stays with the caller on failure
         * (regardless of owns_bdl) so the cleanup path in the calling factory/mount is
         * unambiguous. */
        jrnl->owns_bottom_bdl = false;
        jrnl_instance_destroy(jrnl);
        return err;
    }

    *out_instance = jrnl;
    return ESP_OK;
}

esp_err_t esp_jrnl_mount(const esp_jrnl_config_extended_t *config, esp_jrnl_handle_t *jrnl_handle)
{
    ESP_LOGV(TAG, "Mounting journaling store...");

    if (config == NULL ||
        jrnl_handle == NULL ||
        config->user_cfg.store_size_sectors < JRNL_MIN_STORE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build the internal callback-bridge BDL that wraps the legacy disk_read/write/erase
     * callbacks so that the core engine operates uniformly through BDL ops. */
    esp_blockdev_handle_t bridge_bdl = ESP_BLOCKDEV_HANDLE_INVALID;
    esp_err_t err = jrnl_legacy_bridge_bdl_create(&config->diskio_cfg,
                                                  config->volume_cfg.volume_size,
                                                  config->volume_cfg.disk_sector_size,
                                                  &bridge_bdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create legacy bridge BDL (0x%08X)", err);
        return err;
    }

    _lock_acquire(&s_instances_lock);

    esp_jrnl_handle_t out_handle = JRNL_INVALID_HANDLE;
    for (size_t i = 0; i < JRNL_MAX_HANDLES; i++) {
        if (s_jrnl_instance_ptrs[i] == NULL) {
            out_handle = i;
            break;
        }
    }
    ESP_LOGV(TAG, "jrnl handle: %" PRIu32, out_handle);

    if (out_handle == JRNL_INVALID_HANDLE) {
        ESP_LOGE(TAG, "JRNL_MAX_HANDLES=%d instances already allocated", JRNL_MAX_HANDLES);
        bridge_bdl->ops->release(bridge_bdl);
        _lock_release(&s_instances_lock);
        return ESP_ERR_NO_MEM;
    }

    esp_jrnl_instance_t *jrnl = NULL;
    err = jrnl_instance_create(&config->user_cfg, bridge_bdl, true,
                               &config->diskio_cfg, config->fs_volume_id, &jrnl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_jrnl_mount failed (0x%08X)", err);
        /* Caller always owns bottom_bdl on failure (see jrnl_instance_create contract). */
        bridge_bdl->ops->release(bridge_bdl);
        _lock_release(&s_instances_lock);
        return err;
    }

    s_jrnl_instance_ptrs[out_handle] = jrnl;
    *jrnl_handle = out_handle;
    ESP_LOGV(TAG, "esp_jrnl_mount succeeded (handle: %ld)", *jrnl_handle);

    _lock_release(&s_instances_lock);

    return ESP_OK;
}

esp_err_t esp_jrnl_unmount(const esp_jrnl_handle_t handle)
{
    ESP_LOGV(TAG, "esp_jrnl_unmount (handle: %ld)", handle);

    _lock_acquire(&s_instances_lock);

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        _lock_release(&s_instances_lock);   /* [mass-production fix #1] avoid leaving s_instances_lock held on bad handle */
        return err;
    }

    /* Wait out any other task's transaction window. Unmount from the owning task must not
     * take (would deadlock); destroy then deletes the still-taken semaphore. */
    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[handle];
    const bool self_owned = (inst_ptr->window_owner == xTaskGetCurrentTaskHandle());
    if (!self_owned) {
        if (xSemaphoreTake(inst_ptr->window_sem, portMAX_DELAY) != pdTRUE) {
            _lock_release(&s_instances_lock);
            return ESP_ERR_TIMEOUT;
        }
    }

    /* Cancel a leftover OPEN/COMMIT so unmount does not leave an in-flight store
     * (main: cancel open tx on unmount). */
    if (inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_OPEN ||
            inst_ptr->master.status == ESP_JRNL_STATUS_TRANS_COMMIT) {
        _lock_acquire(&inst_ptr->trans_lock);
        (void)jrnl_reset_master(inst_ptr, false);
        _lock_release(&inst_ptr->trans_lock);
        inst_ptr->window_owner = NULL;
    }

    s_jrnl_instance_ptrs[handle] = NULL;
    if (!self_owned) {
        xSemaphoreGive(inst_ptr->window_sem);
    }

    jrnl_delete_instance(inst_ptr);

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

    return jrnl_inst_trans_start(s_jrnl_instance_ptrs[handle]);
}

esp_err_t esp_jrnl_stop(const esp_jrnl_handle_t handle, const bool commit)
{
    ESP_LOGD(TAG, "esp_jrnl_stop (handle: %ld, commit: %u)", handle, commit);

    esp_err_t err = jrnl_check_handle(handle, __func__);
    if (err != ESP_OK) {
        return err;
    }

    return jrnl_inst_trans_stop(s_jrnl_instance_ptrs[handle], commit);
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

    return jrnl_inst_set_direct_io(s_jrnl_instance_ptrs[handle], direct_access);
}

/*
 * Append one journaled-write record (header + data) to an open journal store. This is the core
 * of the "log a write" operation and is shared between esp_jrnl_write (legacy integer-handle
 * API) and the BDL op-table's write entry point. The caller has already verified that the
 * instance is in ESP_JRNL_STATUS_TRANS_OPEN.
 */
esp_err_t jrnl_log_write(esp_jrnl_instance_t *inst_ptr,
                         const uint8_t *buff,
                         const uint64_t sector,
                         const uint32_t count)
{
    if (inst_ptr == NULL || buff == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t sector_size = inst_ptr->master.volume.disk_sector_size;

    /* Must fit: 1 header sector + count data sectors must leave the master sector untouched. */
    if ((inst_ptr->master.next_free_sector + 1 + count) >= (inst_ptr->master.store_size_sectors - 1)) {
        ESP_LOGE(TAG, "jrnl_log_write: not enough space in the store (0x%08X)", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    esp_jrnl_operation_t *oper_header = NULL;
    esp_err_t err = ESP_OK;

    _lock_acquire(&inst_ptr->trans_lock);
    do {
        oper_header = (esp_jrnl_operation_t *)calloc(1, sector_size);
        if (oper_header == NULL) {
            err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "jrnl_log_write: header allocation failed (0x%08X)", err);
            break;
        }
        oper_header->header.target_sector = sector;
        oper_header->header.sector_count  = count;
        oper_header->header.op_type       = ESP_JRNL_OP_WRITE;
        oper_header->header.crc32_data    = esp_crc32_le(UINT32_MAX, buff, count * sector_size);
        oper_header->crc32_header         = esp_crc32_le(UINT32_MAX,
                                                         (uint8_t *)&oper_header->header,
                                                         sizeof(esp_jrnl_oper_header_t));

        uint64_t oper_addr = jrnl_get_target_disk_sector(inst_ptr, inst_ptr->master.next_free_sector) * sector_size;
        size_t oper_size = (count + 1) * sector_size;

        ESP_LOGV(TAG, "Writing jrnl oper header+data at sector %llu (size %" PRIu32 ")",
                 (unsigned long long)sector, count);

        err = jrnl_erase_range_raw(inst_ptr, oper_addr, oper_size);
        if (unlikely(err != ESP_OK)) {
            ESP_LOGE(TAG, "jrnl_log_write failed at erase (0x%08X)", err);
            break;
        }

        err = jrnl_write_raw(inst_ptr, oper_addr, (void *)oper_header, sector_size);
        if (unlikely(err != ESP_OK)) {
            ESP_LOGE(TAG, "jrnl_log_write failed at header write (0x%08X)", err);
            break;
        }

        err = jrnl_write_raw(inst_ptr, oper_addr + sector_size, (void *)buff, oper_size - sector_size);
        if (unlikely(err != ESP_OK)) {
            ESP_LOGE(TAG, "jrnl_log_write failed at data write (0x%08X)", err);
            break;
        }

        inst_ptr->master.next_free_sector += (1 + count);
        err = jrnl_update_master(inst_ptr, &inst_ptr->master);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jrnl_log_write: master update failed (0x%08X)", err);
        }
    } while (false);

    _lock_release(&inst_ptr->trans_lock);
    free(oper_header);
    return err;
}

esp_err_t jrnl_log_erase(esp_jrnl_instance_t *inst_ptr,
                         const uint64_t sector,
                         const uint32_t count)
{
    if (inst_ptr == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t sector_size = inst_ptr->master.volume.disk_sector_size;

    /* Header-only record; must leave the master sector untouched. */
    if ((inst_ptr->master.next_free_sector + 1) >= (inst_ptr->master.store_size_sectors - 1)) {
        ESP_LOGE(TAG, "jrnl_log_erase: not enough space in the store (0x%08X)", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    esp_jrnl_operation_t *oper_header = NULL;
    esp_err_t err = ESP_OK;

    _lock_acquire(&inst_ptr->trans_lock);
    do {
        oper_header = (esp_jrnl_operation_t *)calloc(1, sector_size);
        if (oper_header == NULL) {
            err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "jrnl_log_erase: header allocation failed (0x%08X)", err);
            break;
        }
        oper_header->header.target_sector = sector;
        oper_header->header.sector_count  = count;
        oper_header->header.op_type       = ESP_JRNL_OP_ERASE;
        oper_header->header.crc32_data    = 0;
        oper_header->crc32_header         = esp_crc32_le(UINT32_MAX,
                                                         (uint8_t *)&oper_header->header,
                                                         sizeof(esp_jrnl_oper_header_t));

        uint64_t oper_addr = jrnl_get_target_disk_sector(inst_ptr, inst_ptr->master.next_free_sector) * sector_size;

        err = jrnl_erase_range_raw(inst_ptr, oper_addr, sector_size);
        if (unlikely(err != ESP_OK)) {
            ESP_LOGE(TAG, "jrnl_log_erase failed at erase (0x%08X)", err);
            break;
        }

        err = jrnl_write_raw(inst_ptr, oper_addr, (void *)oper_header, sector_size);
        if (unlikely(err != ESP_OK)) {
            ESP_LOGE(TAG, "jrnl_log_erase failed at header write (0x%08X)", err);
            break;
        }

        inst_ptr->master.next_free_sector += 1;
        err = jrnl_update_master(inst_ptr, &inst_ptr->master);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jrnl_log_erase: master update failed (0x%08X)", err);
        }
    } while (false);

    _lock_release(&inst_ptr->trans_lock);
    free(oper_header);
    return err;
}

esp_err_t jrnl_log_read(esp_jrnl_instance_t *inst_ptr,
                        uint8_t *dest,
                        uint64_t src_addr,
                        size_t len)
{
    if (inst_ptr == NULL || dest == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = jrnl_read_raw(inst_ptr, src_addr, dest, len);
    if (err != ESP_OK) {
        return err;
    }

    if (inst_ptr->master.status != ESP_JRNL_STATUS_TRANS_OPEN ||
        inst_ptr->master.next_free_sector == 0) {
        return ESP_OK;
    }

    const uint32_t sector_size = inst_ptr->master.volume.disk_sector_size;
    const uint64_t req_end = src_addr + len;
    const uint8_t erase_val = inst_ptr->bottom_bdl->device_flags.default_val_after_erase ? 0xFF : 0x00;

    uint8_t *header = (uint8_t *)calloc(1, sector_size);
    if (header == NULL) {
        return ESP_ERR_NO_MEM;
    }

    _lock_acquire(&inst_ptr->trans_lock);

    uint32_t oper_sector_index = 0;
    uint8_t *data = NULL;

    while (oper_sector_index < inst_ptr->master.next_free_sector) {
        err = jrnl_read_internal(inst_ptr, header, oper_sector_index, 1);
        if (err != ESP_OK) {
            break;
        }

        esp_jrnl_operation_t *oper_header = (esp_jrnl_operation_t *)header;
        uint32_t crc32_header = esp_crc32_le(UINT32_MAX, header, sizeof(esp_jrnl_oper_header_t));
        if (crc32_header != oper_header->crc32_header) {
            err = ESP_ERR_INVALID_CRC;
            ESP_LOGE(TAG, "jrnl_log_read - operation header checksum mismatch");
            break;
        }

        const uint64_t op_start = oper_header->header.target_sector * sector_size;
        const uint64_t op_bytes = (uint64_t)oper_header->header.sector_count * sector_size;
        const uint64_t op_end = op_start + op_bytes;
        const bool overlap = (op_end > src_addr) && (op_start < req_end);
        const uint32_t step = (oper_header->header.op_type == ESP_JRNL_OP_ERASE)
                              ? 1 : (1 + oper_header->header.sector_count);

        if (!overlap) {
            oper_sector_index += step;
            continue;
        }

        const uint64_t ov_start = (op_start > src_addr) ? op_start : src_addr;
        const uint64_t ov_end = (op_end < req_end) ? op_end : req_end;
        const size_t dest_off = (size_t)(ov_start - src_addr);
        const size_t op_off = (size_t)(ov_start - op_start);
        const size_t ov_len = (size_t)(ov_end - ov_start);

        if (oper_header->header.op_type == ESP_JRNL_OP_ERASE) {
            memset(dest + dest_off, erase_val, ov_len);
            oper_sector_index += 1;
            continue;
        }

        if (oper_header->header.op_type != ESP_JRNL_OP_WRITE) {
            err = ESP_ERR_INVALID_STATE;
            ESP_LOGE(TAG, "jrnl_log_read - unknown operation type %" PRIu32, oper_header->header.op_type);
            break;
        }

        if (op_bytes > SIZE_MAX) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        data = (uint8_t *)malloc((size_t)op_bytes);
        if (data == NULL) {
            err = ESP_ERR_NO_MEM;
            break;
        }

        err = jrnl_read_internal(inst_ptr, data, oper_sector_index + 1, oper_header->header.sector_count);
        if (err != ESP_OK) {
            break;
        }

        uint32_t crc32_data = esp_crc32_le(UINT32_MAX, data, (size_t)op_bytes);
        if (crc32_data != oper_header->header.crc32_data) {
            err = ESP_ERR_INVALID_CRC;
            ESP_LOGE(TAG, "jrnl_log_read - operation data checksum mismatch");
            break;
        }

        memcpy(dest + dest_off, data + op_off, ov_len);
        free(data);
        data = NULL;
        oper_sector_index += step;
    }

    _lock_release(&inst_ptr->trans_lock);
    free(data);
    free(header);
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
        const uint64_t direct_addr = (uint64_t)sector * sector_size;
        err = jrnl_erase_range_raw(inst_ptr, direct_addr, count * sector_size);
        if (err == ESP_OK) {
            err = jrnl_write_raw(inst_ptr, direct_addr, buff, count * sector_size);
        }
        return err;
    }

    return jrnl_inst_log_or_implicit(inst_ptr, buff, sector, count);
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

    return jrnl_log_read(inst_ptr, dest, (uint64_t)sector * sector_size, count * sector_size);
}
