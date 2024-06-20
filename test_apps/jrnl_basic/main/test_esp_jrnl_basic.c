/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include "unity.h"
#include "unity_fixture.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs_jrnl_fat.h"
#include "esp_jrnl_internal.h"
#include "sdkconfig.h"
#include <errno.h>
#include "esp_crc.h"

static const char* TAG = "test_esp_jrnl_basic";
static esp_jrnl_handle_t s_jrnl_handle;
extern esp_jrnl_instance_t* s_jrnl_instance_ptrs[JRNL_MAX_HANDLES];

const char* s_basepath = "/spiflash";
const char* s_partlabel = "jrnl";

static uint8_t* s_buf_write = NULL;
static uint8_t* s_buf_read = NULL;

static void test_setup(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 5
    };

    esp_jrnl_config_t jrnl_config = {
        .overwrite_existing = true,
        .force_fs_format = true,
        .replay_journal_after_mount = false,
        .store_size_sectors = 16
    };

    ESP_LOGV(TAG, "test_setup: esp_vfs_fat_spiflash_mount_rw_wl_jrnl(%s, %s)", s_basepath, s_partlabel);
    TEST_ESP_OK(esp_vfs_fat_spiflash_mount_jrnl(s_basepath, s_partlabel, &mount_config, &jrnl_config, &s_jrnl_handle));
}

static void test_teardown(void)
{
    ESP_LOGV(TAG, "test_teardown: esp_vfs_fat_spiflash_unmount_rw_wl_jrnl(%s, handle=%ld", s_basepath, s_jrnl_handle);
    TEST_ESP_OK(esp_vfs_fat_spiflash_unmount_jrnl(&s_jrnl_handle, s_basepath));
}

static esp_err_t test_get_jrnl_master(const esp_jrnl_handle_t handle, esp_jrnl_master_t* jrnl_master)
{
    wl_handle_t wl_handle;
    TEST_ESP_OK(esp_jrnl_get_diskio_handle(handle, &wl_handle));
    TEST_ASSERT(wl_handle != WL_INVALID_HANDLE);
    size_t wlsectorsize = wl_sector_size(wl_handle);
    TEST_ASSERT(wlsectorsize > 0);
    size_t wlpartsize = wl_size(wl_handle);
    TEST_ASSERT(wlpartsize > 0);

    esp_err_t err = wl_read(wl_handle, wlpartsize-wlsectorsize, (void*)jrnl_master, sizeof(esp_jrnl_master_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_read (test_get_jrnl_master) failed with 0x%08X", err);
    }

    return err;
}

static void test_memset_pattern(const uint8_t* pattern, const size_t pattern_size, uint8_t* out_buffer, const size_t out_buffer_size)
{
    assert(pattern_size < out_buffer_size);
    assert(pattern != NULL && out_buffer != NULL);

    for (uint8_t *offset = out_buffer; offset < (out_buffer+out_buffer_size-pattern_size); offset += pattern_size) {
        memcpy((void*)offset, (const void*)pattern, pattern_size);
    }
}


TEST_GROUP(jrnl_basic);

TEST_SETUP(jrnl_basic)
{
    s_buf_write = NULL;
    s_buf_read = NULL;
}

TEST_TEAR_DOWN(jrnl_basic)
{
    free(s_buf_write);
    free(s_buf_read);

    if (s_jrnl_handle != JRNL_INVALID_HANDLE) {
        test_teardown();
    }
}

TEST(jrnl_basic, jrnl_creation)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 5
    };

    esp_jrnl_config_t jrnl_config = ESP_JRNL_DEFAULT_CONFIG();
    jrnl_config.overwrite_existing = true;
    jrnl_config.replay_journal_after_mount = false;

    //mount journaling component to FATFS partition (wear-levelled, mount creates a new jrnl_master record and stores it on the disk)
    TEST_ESP_OK(esp_vfs_fat_spiflash_mount_jrnl(s_basepath, s_partlabel, &mount_config, &jrnl_config, &s_jrnl_handle));
    TEST_ESP_OK(jrnl_check_handle(s_jrnl_handle, __func__));

    const esp_partition_t *jrnl_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, s_partlabel);
    TEST_ASSERT_NOT_NULL(jrnl_partition);

    //read jrnl master record and check all the values for fresh jrnl_master instance
    wl_handle_t wl_handle;
    TEST_ESP_OK(esp_jrnl_get_diskio_handle(s_jrnl_handle, &wl_handle));
    TEST_ASSERT(wl_handle != WL_INVALID_HANDLE);

    size_t wlsectorsize = wl_sector_size(wl_handle);
    TEST_ASSERT(wlsectorsize > 0);
    size_t wlpartsize = wl_size(wl_handle);
    TEST_ASSERT(wlpartsize > 0 && wlpartsize <= jrnl_partition->size);
    size_t wlsectorcount = wlpartsize/wlsectorsize;
    ESP_LOGV(TAG, "target partition size: %lu, wl sector size: %u, wl partition size: %u, wl sector count: %u", jrnl_partition->size, wlsectorsize, wlpartsize, wlsectorcount);

    //jrnl master is stored in the last sector of available area within WL partition
    esp_jrnl_master_t jrnl_master;
    TEST_ESP_OK(wl_read(wl_handle, wlpartsize-wlsectorsize, (void*)&jrnl_master, sizeof(esp_jrnl_master_t)));

#ifdef CONFIG_ESP_JRNL_DEBUG_PRINT
    print_jrnl_master(&jrnl_master);
#endif

    //check all important properties stored on the disk
    TEST_ASSERT(jrnl_master.jrnl_magic_mark == JRNL_STORE_MARKER);
    TEST_ASSERT(jrnl_master.store_size_sectors == jrnl_config.store_size_sectors);
    TEST_ASSERT(jrnl_master.store_volume_offset_sector == wlsectorcount - jrnl_config.store_size_sectors);
    TEST_ASSERT(jrnl_master.next_free_sector == 0);
    TEST_ASSERT(jrnl_master.status == ESP_JRNL_STATUS_TRANS_READY);
    TEST_ASSERT(jrnl_master.volume.volume_size == wlpartsize);
    TEST_ASSERT(jrnl_master.volume.disk_sector_size == wlsectorsize);

    //verify calculated FS space (FS = WL - JRNL)
    size_t ff_sector_count;
    TEST_ESP_OK(esp_jrnl_get_sector_count(s_jrnl_handle, &ff_sector_count));
    TEST_ASSERT(ff_sector_count == jrnl_master.store_volume_offset_sector);

    TEST_ESP_OK(esp_vfs_fat_spiflash_unmount_jrnl(&s_jrnl_handle, s_basepath));
    TEST_ASSERT(s_jrnl_handle == JRNL_INVALID_HANDLE);
}

TEST(jrnl_basic, internal_reads_writes)
{
    test_setup();

    esp_jrnl_master_t jrnl_master;
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[s_jrnl_handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);

    TEST_ASSERT(jrnl_get_target_disk_sector(inst_ptr, 1) == jrnl_master.store_volume_offset_sector + 1);

    //test jrnl_erase_range_raw, jrnl_write_raw and jrnl_write_internal
    size_t sector_size = inst_ptr->master.volume.disk_sector_size;
    TEST_ASSERT(sector_size > 0);
    s_buf_write = malloc(sector_size);
    TEST_ASSERT(s_buf_write);
    memset(s_buf_write, 0xAA, sector_size);
    TEST_ESP_OK(jrnl_write_internal(inst_ptr, s_buf_write, 0, 1));

    s_buf_read = (uint8_t*)calloc(1, sector_size);
    TEST_ASSERT(s_buf_read);
    TEST_ESP_OK(jrnl_read_internal(inst_ptr, s_buf_read, 0, 1));

    TEST_ASSERT(memcmp(s_buf_read, s_buf_write, sector_size) == 0);

    test_teardown();
}

TEST(jrnl_basic, reset_master)
{
    test_setup();

    esp_jrnl_master_t jrnl_master;
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[s_jrnl_handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);

    //modify master record data
    jrnl_master.status = ESP_JRNL_STATUS_TRANS_OPEN;
    jrnl_master.next_free_sector = 0xFFFFFFFF;
    jrnl_master.jrnl_magic_mark = 0xFFFFFFFF;
    TEST_ESP_OK(jrnl_write_internal(inst_ptr, (const uint8_t*)&jrnl_master, jrnl_master.store_size_sectors - 1, 1));

    //reset the record and verify
    TEST_ESP_OK(jrnl_reset_master(inst_ptr, false));
    memset((void*)&jrnl_master, 0, sizeof(esp_jrnl_master_t));
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));

    TEST_ASSERT(jrnl_master.jrnl_magic_mark == JRNL_STORE_MARKER);
    TEST_ASSERT(jrnl_master.next_free_sector == 0);
    TEST_ASSERT(jrnl_master.status == ESP_JRNL_STATUS_TRANS_READY);

    TEST_ESP_OK(jrnl_reset_master(inst_ptr, true));
    memset((void*)&jrnl_master, 0, sizeof(esp_jrnl_master_t));
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));

    TEST_ASSERT(jrnl_master.jrnl_magic_mark == JRNL_STORE_MARKER);
    TEST_ASSERT(jrnl_master.next_free_sector == 0);
    TEST_ASSERT(jrnl_master.status == ESP_JRNL_STATUS_FS_INIT);

    test_teardown();
}

TEST(jrnl_basic, jrnl_start)
{
    test_setup();

    //open transaction
    TEST_ESP_OK(esp_jrnl_start(s_jrnl_handle));

    esp_jrnl_master_t jrnl_master;
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));
    TEST_ASSERT(jrnl_master.status == ESP_JRNL_STATUS_TRANS_OPEN);

    //can't run multiple transactions
    TEST_ASSERT(esp_jrnl_start(s_jrnl_handle) == ESP_ERR_INVALID_STATE);

    test_teardown();
}

TEST(jrnl_basic, jrnl_mount_unmount)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 5
    };

    esp_jrnl_config_t jrnl_config = {
            .overwrite_existing = true,
            .force_fs_format = true,
            .replay_journal_after_mount = false,
            .store_size_sectors = 16
    };

    TEST_ESP_OK(esp_vfs_fat_spiflash_mount_jrnl(s_basepath, s_partlabel, &mount_config, &jrnl_config, &s_jrnl_handle));
    TEST_ESP_OK(esp_vfs_fat_spiflash_unmount_jrnl(&s_jrnl_handle, s_basepath));
    TEST_ASSERT(s_jrnl_handle == JRNL_INVALID_HANDLE);

    jrnl_config.overwrite_existing = false;
    jrnl_config.force_fs_format = false;
    TEST_ESP_OK(esp_vfs_fat_spiflash_mount_jrnl(s_basepath, s_partlabel, &mount_config, &jrnl_config, &s_jrnl_handle));
    TEST_ESP_OK(esp_vfs_fat_spiflash_unmount_jrnl(&s_jrnl_handle, s_basepath));
    TEST_ASSERT(s_jrnl_handle == JRNL_INVALID_HANDLE);
}

TEST(jrnl_basic, direct_read_write)
{
    test_setup();

    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[s_jrnl_handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);

    //set INIT status to get direct read/write access to the target disk
    esp_jrnl_master_t jrnl_master;
    TEST_ESP_OK(jrnl_reset_master(inst_ptr, true));
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));
    TEST_ASSERT(jrnl_master.status == ESP_JRNL_STATUS_FS_INIT);

    //write data to desired sector
    size_t sector_size = inst_ptr->master.volume.disk_sector_size;
    TEST_ASSERT(sector_size > 0);
    s_buf_write = (uint8_t*)calloc(1, sector_size);
    TEST_ASSERT(s_buf_write);

    const uint8_t buff[] = "ABCDEFGHABCDEFGH";
    test_memset_pattern(buff, sizeof(buff), s_buf_write, sector_size);
    size_t test_target_sector = 15;

    TEST_ESP_OK(esp_jrnl_write(s_jrnl_handle, s_buf_write, test_target_sector, 1));

    //read data back from the disk
    s_buf_read = (uint8_t*)calloc(1, sector_size);
    TEST_ASSERT(s_buf_read);
    TEST_ESP_OK(esp_jrnl_read(s_jrnl_handle, test_target_sector, s_buf_read, 1));

    //match
    TEST_ASSERT(memcmp(s_buf_read, s_buf_write, sector_size) == 0);

    test_teardown();
}

TEST(jrnl_basic, jrnl_start_write)
{
    test_setup();

    //start journaling transaction
    TEST_ESP_OK(esp_jrnl_start(s_jrnl_handle));

    //prepare testing data
    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[s_jrnl_handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);

    esp_jrnl_master_t jrnl_master;
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));

    size_t sector_size = inst_ptr->master.volume.disk_sector_size;
    TEST_ASSERT(sector_size > 0);

    s_buf_write = (uint8_t*)calloc(1, sector_size);
    TEST_ASSERT(s_buf_write);

    //write data (2 sector in jrnl store: header + data)
    const uint8_t buff_pattern[] = "ABCDEFGHABCDEFGH";
    size_t test_target_sector = 20;
    test_memset_pattern(buff_pattern, sizeof(buff_pattern), s_buf_write, sector_size);
    TEST_ESP_OK(esp_jrnl_write(s_jrnl_handle, s_buf_write, test_target_sector, 1));

    //check written header
    size_t jrnl_sector_header = 0;
    size_t jrnl_sector_data = 1;

    s_buf_read = (uint8_t*)calloc(1, sector_size);
    TEST_ASSERT(s_buf_read);
    TEST_ESP_OK(jrnl_read_internal(inst_ptr, s_buf_read, jrnl_sector_header, 1));
    esp_jrnl_operation_t *oper_header = (esp_jrnl_operation_t*)s_buf_read;

    TEST_ASSERT(oper_header->header.target_sector == test_target_sector);
    TEST_ASSERT(oper_header->header.sector_count == 1);
    TEST_ASSERT(oper_header->header.crc32_data == esp_crc32_le(UINT32_MAX, s_buf_write, sector_size));
    TEST_ASSERT(oper_header->crc32_header == esp_crc32_le(UINT32_MAX, (uint8_t *) s_buf_read, sizeof(esp_jrnl_oper_header_t)));

    //check written data
    TEST_ESP_OK(jrnl_read_internal(inst_ptr, s_buf_read, jrnl_sector_data, 1));
    TEST_ASSERT(memcmp(s_buf_read, s_buf_write, sector_size) == 0);

    //check jrnl master is properly updated
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));
    TEST_ASSERT(inst_ptr->master.next_free_sector == 2);

    test_teardown();
}

TEST(jrnl_basic, jrnl_stop_replay)
{
    test_setup();

    //start journaling transaction
    TEST_ESP_OK(esp_jrnl_start(s_jrnl_handle));

    //prepare testing data
    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[s_jrnl_handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);

    esp_jrnl_master_t jrnl_master;
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));

    size_t sector_size = inst_ptr->master.volume.disk_sector_size;
    TEST_ASSERT(sector_size > 0);

    s_buf_write = (uint8_t*)calloc(1, sector_size);
    TEST_ASSERT(s_buf_write);

    //write data (2 sectors in jrnl store: header + data)
    const uint8_t buff_pattern[] = "ABCDEFGHABCDEFGH";
    size_t test_target_sector = 8;
    test_memset_pattern(buff_pattern, sizeof(buff_pattern), s_buf_write, sector_size);
    TEST_ESP_OK(esp_jrnl_write(s_jrnl_handle, s_buf_write, test_target_sector, 1));

    //1. test transaction CANCEL (no data written to the target, jrnl store gets reset)
    TEST_ESP_OK(esp_jrnl_stop(s_jrnl_handle, false));

    memset((void*)&jrnl_master, 0, sizeof(esp_jrnl_master_t));
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));

    TEST_ASSERT(jrnl_master.jrnl_magic_mark == JRNL_STORE_MARKER);
    TEST_ASSERT(jrnl_master.next_free_sector == 0);
    TEST_ASSERT(jrnl_master.status == ESP_JRNL_STATUS_TRANS_READY);

    //check target disk sector (must be untouched)
    s_buf_read = (uint8_t*)calloc(1, sector_size);
    TEST_ASSERT(s_buf_read);
    TEST_ESP_OK(esp_jrnl_read(s_jrnl_handle, test_target_sector, s_buf_read, 1));
    TEST_ASSERT(memcmp(s_buf_read, s_buf_write, sector_size) != 0);

    //2. test transaction COMMIT (jrnl_replay)
    test_target_sector = 10;
    TEST_ESP_OK(esp_jrnl_start(s_jrnl_handle));
    TEST_ESP_OK(esp_jrnl_write(s_jrnl_handle, s_buf_write, test_target_sector, 1));
    TEST_ESP_OK(esp_jrnl_stop(s_jrnl_handle, true));
    memset(s_buf_read, 0, sector_size);
    TEST_ESP_OK(esp_jrnl_read(s_jrnl_handle, test_target_sector, s_buf_read, 1));

    //journaled data must be applied to the target sector
    TEST_ASSERT(memcmp(s_buf_read, s_buf_write, sector_size) == 0);

    //either way the jrml master must be reset
    memset((void*)&jrnl_master, 0, sizeof(esp_jrnl_master_t));
    TEST_ESP_OK(test_get_jrnl_master(s_jrnl_handle, &jrnl_master));
    TEST_ASSERT(jrnl_master.jrnl_magic_mark == JRNL_STORE_MARKER);
    TEST_ASSERT(jrnl_master.next_free_sector == 0);
    TEST_ASSERT(jrnl_master.status == ESP_JRNL_STATUS_TRANS_READY);

    test_teardown();
}

TEST_GROUP_RUNNER(fs_journaling_basic)
{
    RUN_TEST_CASE(jrnl_basic, jrnl_creation);
    RUN_TEST_CASE(jrnl_basic, internal_reads_writes);
    RUN_TEST_CASE(jrnl_basic, reset_master);
    RUN_TEST_CASE(jrnl_basic, jrnl_start);
    RUN_TEST_CASE(jrnl_basic, jrnl_mount_unmount);
    RUN_TEST_CASE(jrnl_basic, direct_read_write);
    RUN_TEST_CASE(jrnl_basic, jrnl_start_write);
    RUN_TEST_CASE(jrnl_basic, jrnl_stop_replay);
}

void app_main(void)
{
    UNITY_MAIN(fs_journaling_basic);
}
