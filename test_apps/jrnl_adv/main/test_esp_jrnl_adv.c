/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "unity.h"
#include "esp_vfs_jrnl_fat.h"
#include "esp_jrnl_internal.h"

/* ESP_JRNL ADVANCED TESTS
 * -----------------------
 * This test set emulates power-off events at various phases of the journaling process, though it cannot cover all
 * real-world cases. Based on jrnl_instance->test_config flags, given FS operation being journaled gets interrupted
 * at important points during the 'Commit' phase, when all the journaled blocks are being transferred to their target locations.
 * General esp_jrnl approach is the following:
 *  - when journaling transaction is interrupted during 'Open' phase, it is ignored and cleaned on the next mount (== rollback).
 *  - when the transaction is interrupted within 'Commit' phase (after user-invoked call to esp_jrnl_stop()), it is attempted to be finished on the next mount (== FF commit).
 * No matter where exactly in the 'Commit' sequence power-off appears, the journaling store keeps the transaction data until the transfer is successfully finished and confirmed.
 * Any other result is considered incomplete, and the journaled log replay is repeated on the next mount.
 * Above described generally applies to all file-system operations invoking disk_write access, specific modifications are described for each test group separately.
 * Check JRNL_TEST_PRELIMINARY_EXIT macro and ESP_JRNL_TEST* defines (esp_jrnl/private_include/esp_jrnl_internal.h) for the interruption points.
 * Above-described setup is the default, esp_jrnl store behaviour can be widely modified by its configuration.
 */

static const char* TAG = "test_esp_jrnl_adv";
static esp_jrnl_handle_t s_jrnl_handle = JRNL_INVALID_HANDLE;
extern esp_jrnl_instance_t* s_jrnl_instance_ptrs[JRNL_MAX_HANDLES];

const char* s_basepath = "/spiflash";
const char* s_partlabel = "jrnl";

static uint8_t* s_buf_write = NULL;
static uint8_t* s_buf_read = NULL;

static void test_memset_pattern(const uint8_t* pattern, const size_t pattern_size, uint8_t* out_buffer, const size_t out_buffer_size)
{
    assert(pattern_size < out_buffer_size);
    assert(pattern != NULL && out_buffer != NULL);

    for (uint8_t *offset = out_buffer; offset < (out_buffer+out_buffer_size-pattern_size); offset += pattern_size) {
        memcpy((void*)offset, (const void*)pattern, pattern_size);
    }
}

static void test_check_inst_master_ready(esp_jrnl_handle_t handle)
{
    TEST_ASSERT_NOT_EQUAL(JRNL_INVALID_HANDLE, handle);
    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);
    TEST_ASSERT_EQUAL(inst_ptr->master.status, ESP_JRNL_STATUS_TRANS_READY);
    TEST_ASSERT_EQUAL(inst_ptr->master.next_free_sector, 0);
}

static esp_err_t test_read_jrnl_master_sector(esp_jrnl_master_t* master)
{
    const esp_partition_t *jrnl_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, s_partlabel);
    if (jrnl_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition (type='data', subtype='fat', partition_label='%s'). Check the partition table.", s_partlabel);
        return ESP_ERR_NOT_FOUND;
    }

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t err = wl_mount(jrnl_partition, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount wear levelling layer, error: 0x%08X", err);
        return err;
    }

    err = wl_read(wl_handle, wl_size(wl_handle) - wl_sector_size(wl_handle), (void*)master, sizeof(esp_jrnl_master_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read jrnl master record from disk, error: 0x%08X", err);
    }

    wl_unmount(wl_handle);

    return err;
}

static void test_check_master_status_non_empty(esp_jrnl_trans_status_t status)
{
    //raw-read jrnl master record from disk
    esp_jrnl_master_t jrnl_master;
    TEST_ESP_OK(test_read_jrnl_master_sector(&jrnl_master));
#ifdef CONFIG_ESP_JRNL_DEBUG_PRINT
    print_jrnl_master(&jrnl_master);
#endif
    //check saved jrnl store status and journaled data existence
    TEST_ASSERT_EQUAL(jrnl_master.status, status);
    TEST_ASSERT(jrnl_master.next_free_sector > 0);
}

static void test_teardown_jrnl(void)
{
    free(s_buf_write);
    free(s_buf_read);

    ESP_LOGV(TAG, "test_teardown_jrnl: esp_vfs_fat_spiflash_unmount_rw_wl_jrnl(%s, handle=%ld", s_basepath, s_jrnl_handle);
    TEST_ESP_OK(esp_vfs_fat_spiflash_unmount_jrnl(&s_jrnl_handle, s_basepath));
    TEST_ASSERT(s_jrnl_handle == JRNL_INVALID_HANDLE);
}

/* jrnl_config == NULL => use default test setup */
static void test_setup_jrnl(esp_jrnl_config_t* jrnl_config)
{
    if (s_jrnl_handle != JRNL_INVALID_HANDLE) {
        ESP_LOGV(TAG, "test_setup_jrnl: found mounted volume, dismounting");
        test_teardown_jrnl();
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 5
    };

    esp_jrnl_config_t* jrnl_config_def = NULL;

    if (jrnl_config == NULL) {
        jrnl_config_def = (esp_jrnl_config_t*)malloc(sizeof(esp_jrnl_config_t));
        TEST_ASSERT(jrnl_config_def != NULL);

        jrnl_config_def->store_size_sectors = 16;
        jrnl_config_def->replay_journal_after_mount = false;
        jrnl_config_def->overwrite_existing = true;
        jrnl_config_def->force_fs_format = true;

        jrnl_config = jrnl_config_def;
    }

    ESP_LOGV(TAG, "test_setup_jrnl: esp_vfs_fat_spiflash_mount_jrnl(%s, %s)", s_basepath, s_partlabel);
    esp_err_t err = esp_vfs_fat_spiflash_mount_jrnl(s_basepath, s_partlabel, &mount_config, jrnl_config, &s_jrnl_handle);
    free(jrnl_config_def);

    TEST_ESP_OK(err);
}

/* *******************************************************************************************
 * CREATE/CLOSE FILE
 *
 * Creating empty file on FatFS disk device is not guaranteed until f_close() is called.
 * The following tests check:
 *  - the file is not created if power-off appears before the journal commit phase
 *  - the (empty) file is created after all power-off cases during the journal commit, but fclose() is required
 *  It means that we effectively test either rolling back for the first case, or the fast-forward replaying for f_close()
 *  (as testing fopen() for re-committing on empty file makes no sense)
 */

/* start 'Create file' operation sequence and leave the transaction unfinished at various stages */
static void jrnl_create_file_early_exit(uint32_t flags)
{
    //create fresh FatFS partition
    test_setup_jrnl(NULL);
    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[s_jrnl_handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);

    //1. when testing the fclose() scenario, don't interrupt the first transaction
    if (!(flags & ESP_JRNL_TEST_REQUIRE_FILE_CLOSE)) {
        inst_ptr->test_config = flags;
    }

    //2. create empty file
    char test_file_name[64] = {0};
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, "testfil1.txt");
    FILE* testfile = fopen(test_file_name, "w+");
    TEST_ASSERT_NOT_NULL(testfile);

    //3. set the power-off testing flags for fclose() if !1.
    if (flags & ESP_JRNL_TEST_REQUIRE_FILE_CLOSE) {
        inst_ptr->test_config = flags;
        TEST_ASSERT_EQUAL(0, fclose(testfile));
    }
}

/* remount and check the file exists/doesn't exist */
static void jrnl_create_unfinish_check(bool commit_running)
{
    test_check_master_status_non_empty(commit_running ? ESP_JRNL_STATUS_TRANS_COMMIT : ESP_JRNL_STATUS_TRANS_OPEN);

    //mount journaled FS again
    esp_jrnl_config_t jrnl_cfg = ESP_JRNL_DEFAULT_CONFIG();
    test_setup_jrnl(&jrnl_cfg);

    //check the file system:
    char test_file_name[64] = {0};
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, "testfil1.txt");

    //a) the file is created and is empty
    if (commit_running) {
        TEST_ASSERT(fopen(test_file_name, "r") != NULL);
        struct stat f_stat;
        TEST_ASSERT_EQUAL(0, stat(test_file_name, &f_stat));
        TEST_ASSERT_EQUAL(0, f_stat.st_size);
    }
    //b) the file doesn't exist
    else {
        TEST_ASSERT(fopen(test_file_name, "r") == NULL);
        TEST_ASSERT(errno == ENOENT);
    }

    test_teardown_jrnl();
}

/* 1.a: power-off before commit */
static void jrnl_create_unfinish_1(void)
{
    jrnl_create_file_early_exit(ESP_JRNL_TEST_STOP_SKIP_COMMIT | ESP_JRNL_TEST_REQUIRE_FILE_CLOSE);
}

/* 1.b:
 *  - jrnl contains stored data (status Open, data sectors count is non-zero)
 *  - it's reset during mount
 *  - the file is not created in the FS
 */
static void jrnl_create_unfinish_check_not_committing(void)
{
    jrnl_create_unfinish_check(false);
}

/* 2.a: power-off after beginning of 'Commit' phase (master record updated, no data yet transferred)
 */
static void jrnl_create_unfinish_2(void)
{
    jrnl_create_file_early_exit(ESP_JRNL_TEST_STOP_SET_COMMIT_AND_EXIT | ESP_JRNL_TEST_REQUIRE_FILE_CLOSE);
}

/* 2.b: journaled data are successfully conveyed to their target sectors:
 *  - jrnl contains stored data (status Commit, data sectors count is non-zero)
 *  - the journal is replayed (== the transaction is committed)
 *  - jrnl.master gets reset (==jrnl is ready for next transaction)
 *  - the file is created in the FS
 */
static void jrnl_create_unfinish_check_committing(void)
{
    jrnl_create_unfinish_check(true);
}

/* 3.a: power-off after erasing the first target sector in 'Commit' phase
 * 3.b: dtto 2.b
 */
static void jrnl_create_unfinish_3(void)
{
    jrnl_create_file_early_exit(ESP_JRNL_TEST_REPLAY_ERASE_AND_EXIT | ESP_JRNL_TEST_REQUIRE_FILE_CLOSE);
}

/* 4.a: power-off after write of the first target sector in 'Commit' phase
 * 4.b: dtto 2.b
 */
static void jrnl_create_unfinish_4(void)
{
    jrnl_create_file_early_exit(ESP_JRNL_TEST_REPLAY_WRITE_AND_EXIT | ESP_JRNL_TEST_REQUIRE_FILE_CLOSE);
}

/* 5.a: power-off after all the sectors are transferred but before resetting the master record ('Commit' phase stays flagged as unfinished)
 * 5.b: dtto 2.b
 */
static void jrnl_create_unfinish_5(void)
{
    jrnl_create_file_early_exit(ESP_JRNL_TEST_REPLAY_EXIT_BEFORE_CLOSE | ESP_JRNL_TEST_REQUIRE_FILE_CLOSE);
}

TEST_CASE_MULTIPLE_STAGES("CREATE FILE - skip commit", "[jrnl_adv]", jrnl_create_unfinish_1, jrnl_create_unfinish_check_not_committing);
TEST_CASE_MULTIPLE_STAGES("CREATE/CLOSE FILE - start commit and exit", "[jrnl_adv]", jrnl_create_unfinish_2, jrnl_create_unfinish_check_committing);
TEST_CASE_MULTIPLE_STAGES("CREATE/CLOSE FILE - start jrnl replay and exit", "[jrnl_adv]", jrnl_create_unfinish_3, jrnl_create_unfinish_check_committing);
TEST_CASE_MULTIPLE_STAGES("CREATE/CLOSE FILE - continue jrnl replay and exit", "[jrnl_adv]", jrnl_create_unfinish_4, jrnl_create_unfinish_check_committing);
TEST_CASE_MULTIPLE_STAGES("CREATE/CLOSE FILE - finish jrnl replay and exit", "[jrnl_adv]", jrnl_create_unfinish_5, jrnl_create_unfinish_check_committing);


/* *******************************************************************************************
 * WRITE FILE
 *
 * Writing to file is performed by various C API functions:
 * - fwrite(): The operation produces disk writes, however, the file-record in the FatFS is not updated until calling fclose() (ie, the file pointer is not updated).
 *             Thus, fwrite() operation cannot be reasonably tested for end-user relevant results as the data written remain unaccessible after next remonuting.
 *             => the only concern tested here is the file staying open-able after a power off event (both before and during committing phase)
 *
 */

//size = 15+1
static const uint8_t pattern_buff[] = "TESTDATA1234567";

//alloc 'factor' * sector size and fill the buffer with 'pattern_buff'
static size_t jrnl_test_prepare_file_pattern(const esp_jrnl_instance_t* inst_ptr, const size_t factor, uint8_t** buff)
{
    size_t sector_size = inst_ptr->master.volume.disk_sector_size;
    TEST_ASSERT(sector_size > 0);
    if (!*buff) *buff = (uint8_t*)calloc(factor, sector_size);
    TEST_ASSERT(buff);

    size_t buffsize = factor*sector_size;
    test_memset_pattern(pattern_buff, sizeof(pattern_buff), *buff, buffsize);

    return buffsize;
}

/* to test fwrite() in journaled environment, these steps are taken:
 * 1. create a file in non-journaled FS, write data to it n close the handle
 *    The data to write is pattern_buff[] * pattern_count, for easy verification
 * 2. enable FS journaling and open the file again for appending
 * 3. add 1 pattern_buff chunk to the file and keep it open
 * 4. leave the transaction unfinished according to the test flags (reboot)
 */
static void jrnl_fwrite_file_early_exit(uint32_t flags)
{
    test_setup_jrnl(NULL);

    char test_file_name[64] = {0};
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, "testfil2.txt");

    struct stat f_stat;
    TEST_ASSERT_EQUAL(-1, stat(test_file_name, &f_stat));

    //get the journal store instance
    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[s_jrnl_handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);
    inst_ptr->test_config = flags;
#ifdef CONFIG_ESP_JRNL_DEBUG_PRINT
    print_jrnl_instance(inst_ptr);
#endif

    //prepare write buffer of 2 * disk_sector size
    size_t filesize = jrnl_test_prepare_file_pattern(inst_ptr, 2, &s_buf_write);

    //1. create a file with contents directly in the FS
    inst_ptr->test_config |= ESP_JRNL_TEST_SUSPEND_TRANSACTION;
    TEST_ESP_OK(esp_jrnl_set_direct_io(s_jrnl_handle, true));

    FILE* testfile = fopen(test_file_name, "w+");
    TEST_ASSERT_NOT_NULL(testfile);
    TEST_ASSERT(fwrite(s_buf_write, filesize, 1, testfile) == 1);
    TEST_ASSERT_EQUAL(0, fclose(testfile));
    TEST_ASSERT_EQUAL(0, stat(test_file_name, &f_stat));
    TEST_ASSERT_EQUAL(filesize, f_stat.st_size);

    //2. open, enable journaling and append data to the file (==double the size)
    testfile = fopen(test_file_name, "a+");
    TEST_ASSERT_NOT_NULL(testfile);

    inst_ptr->test_config &= (~ESP_JRNL_TEST_SUSPEND_TRANSACTION);
    TEST_ESP_OK(esp_jrnl_set_direct_io(s_jrnl_handle, false));

    //fwrite operation gets interrupted according to the testing flags of each test-case
    fwrite(s_buf_write, filesize, 1, testfile);
}

/* check the file exist in (journaled) FS and has expected length and data */
static void jrnl_fwrite_unfinish_check(bool commit_running)
{
    //1. raw-read jrnl master record from disk
    esp_jrnl_master_t jrnl_master;
    TEST_ESP_OK(test_read_jrnl_master_sector(&jrnl_master));

#ifdef CONFIG_ESP_JRNL_DEBUG_PRINT
    print_jrnl_master(&jrnl_master);
#endif

    //2. test the store contains appropriate status
    esp_jrnl_trans_status_t status = commit_running ? ESP_JRNL_STATUS_TRANS_COMMIT : ESP_JRNL_STATUS_TRANS_OPEN;
    TEST_ASSERT_EQUAL(jrnl_master.status, status);

    //3. there should be 2 sectors stored (+1 header each)
    TEST_ASSERT(jrnl_master.next_free_sector == 4);

    //4. the fie should stay unchanged and have the length == 2*disk_sector
    esp_jrnl_config_t jrnl_cfg = ESP_JRNL_DEFAULT_CONFIG();
    test_setup_jrnl(&jrnl_cfg);

    char test_file_name[64] = {0};
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, "testfil2.txt");

    struct stat f_stat;
    TEST_ASSERT_EQUAL(0, stat(test_file_name, &f_stat));

    size_t expected_size = jrnl_master.volume.disk_sector_size * 2;
    ESP_LOGV(TAG, "jrnl_fwrite_unfinish_check: found file with size=%"PRIu32, (uint32_t)f_stat.st_size);
    TEST_ASSERT_EQUAL(expected_size, f_stat.st_size);

    test_teardown_jrnl();
}

/* 1.a: power-off before commit */
static void jrnl_fwrite_unfinish_1(void)
{
    jrnl_fwrite_file_early_exit(ESP_JRNL_TEST_STOP_SKIP_COMMIT);
}

/* 1.b:
 *  - jrnl contains stored data (status Open, data sectors count is non-zero)
 *  - it's reset during mount
 *  - the file is not changed in the FS (has the original length)
 */
static void jrnl_fwrite_unfinish_check_not_committing(void)
{
    jrnl_fwrite_unfinish_check(false);
}

/* 2.a: power-off after beginning of 'Commit' phase (master record updated, no data yet transferred)
 */
static void jrnl_fwrite_unfinish_2(void)
{
    jrnl_fwrite_file_early_exit(ESP_JRNL_TEST_STOP_SET_COMMIT_AND_EXIT);
}

/* 2.b: journaled data are successfully conveyed to their target sectors:
 *  - jrnl contains stored data (status Commit, data sectors count is non-zero)
 *  - the journal is replayed (== the transaction is committed)
 *  - jrnl.master gets reset (==jrnl is ready for next transaction)
 *  - the file is extended in the FS by exact data size
 */
static void jrnl_fwrite_unfinish_check_committing(void)
{
    jrnl_fwrite_unfinish_check(true);
}

TEST_CASE_MULTIPLE_STAGES("WRITE FILE (fwrite) - skip commit", "[jrnl_adv]", jrnl_fwrite_unfinish_1, jrnl_fwrite_unfinish_check_not_committing);
TEST_CASE_MULTIPLE_STAGES("WRITE FILE (fwrite) - start commit and exit", "[jrnl_adv]", jrnl_fwrite_unfinish_2, jrnl_fwrite_unfinish_check_committing);


/* *******************************************************************************************
 * MKDIR
 */

/* start 'Create directory' operation and leave the transaction unfinished at various stages */
static void jrnl_mkdir_early_exit(uint32_t flags)
{
    //create clean FatFS partition
    test_setup_jrnl(NULL);
    esp_jrnl_instance_t* inst_ptr = s_jrnl_instance_ptrs[s_jrnl_handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);

    //configure the jrnl component to exit transactions preliminary
    inst_ptr->test_config = flags;

    //create a directory
    char test_dir_name[64] = {0};
    snprintf(test_dir_name, sizeof(test_dir_name), "%s/%s", s_basepath, "testdir");

    //run the file system operation to be "randomly" interrupted
    mkdir(test_dir_name, 0777);
}

/* 1.a: power-off before commit */
static void jrnl_mkdir_unfinish_1(void)
{
    jrnl_mkdir_early_exit(ESP_JRNL_TEST_STOP_SKIP_COMMIT);
}

/* 1.b:
 *  - jrnl contains stored data (status Open, data sectors count is non-zero)
 *  - it's reset during mount
 *  - the directory is not created in the FS
 */
static void jrnl_mkdir_unfinish_check_no_dir(void)
{
    test_check_master_status_non_empty(ESP_JRNL_STATUS_TRANS_OPEN);

    //remount with the default setup to reset the journaling store
    esp_jrnl_config_t jrnl_cfg = ESP_JRNL_DEFAULT_CONFIG();
    test_setup_jrnl(&jrnl_cfg);
    test_check_inst_master_ready(s_jrnl_handle);

    //finally, check there is no directory in the file-system
    char test_dir_name[64] = {0};
    snprintf(test_dir_name, sizeof(test_dir_name), "%s/%s", s_basepath, "testdir");
    struct stat d_stat;
    TEST_ASSERT_EQUAL(-1, stat(test_dir_name, &d_stat));

    test_teardown_jrnl();
}

/* 2.a: power-off after beginning of 'Commit' phase (master record updated, no data yet transferred)
 */
static void jrnl_mkdir_unfinish_2(void)
{
    jrnl_mkdir_early_exit(ESP_JRNL_TEST_STOP_SET_COMMIT_AND_EXIT);
}

/* 2.b: journaled data are successfully conveyed to their target sectors:
 *  - jrnl contains stored data (status Commit, data sectors count is non-zero)
 *  - the journal is replayed (== the transaction is committed)
 *  - jrnl.master gets reset (==jrnl is ready for next transaction)
 *  - the directory is created in the FS
 */
static void jrnl_mkdir_unfinish_check_commit_dir_exits(void)
{
    test_check_master_status_non_empty(ESP_JRNL_STATUS_TRANS_COMMIT);

    //remount with the default setup to replay the unfinished transaction
    esp_jrnl_config_t jrnl_cfg = ESP_JRNL_DEFAULT_CONFIG();
    test_setup_jrnl(&jrnl_cfg);
    test_check_inst_master_ready(s_jrnl_handle);

    //finally, check the directory was properly created in the file-system
    char test_dir_name[64] = {0};
    snprintf(test_dir_name, sizeof(test_dir_name), "%s/%s", s_basepath, "testdir");

    struct stat d_stat;
    TEST_ASSERT_EQUAL(0, stat(test_dir_name, &d_stat));
    TEST_ASSERT_EQUAL(0777, 0777 & d_stat.st_mode);

    test_teardown_jrnl();
}

/* 3.a: power-off after erasing the first target sector in 'Commit' phase
 * 3.b: dtto 2.b
 */
static void jrnl_mkdir_unfinish_3(void)
{
    jrnl_mkdir_early_exit(ESP_JRNL_TEST_REPLAY_ERASE_AND_EXIT);
}

/* 4.a: power-off after write of the first target sector in 'Commit' phase
 * 4.b: dtto 2.b
 */
static void jrnl_mkdir_unfinish_4(void)
{
    jrnl_mkdir_early_exit(ESP_JRNL_TEST_REPLAY_WRITE_AND_EXIT);
}

/* 5.a: power-off after all the sectors are transferred but before resetting the master record ('Commit' phase stays flagged as unfinished)
 * 5.b: dtto 2.b
 */
static void jrnl_mkdir_unfinish_5(void)
{
    jrnl_mkdir_early_exit(ESP_JRNL_TEST_REPLAY_EXIT_BEFORE_CLOSE);
}

TEST_CASE_MULTIPLE_STAGES("MKDIR - skip commit", "[jrnl_adv]", jrnl_mkdir_unfinish_1, jrnl_mkdir_unfinish_check_no_dir);
TEST_CASE_MULTIPLE_STAGES("MKDIR - start commit and exit", "[jrnl_adv]", jrnl_mkdir_unfinish_2, jrnl_mkdir_unfinish_check_commit_dir_exits);
TEST_CASE_MULTIPLE_STAGES("MKDIR - start replay and exit", "[jrnl_adv]", jrnl_mkdir_unfinish_3, jrnl_mkdir_unfinish_check_commit_dir_exits);
TEST_CASE_MULTIPLE_STAGES("MKDIR - continue replay and exit", "[jrnl_adv]", jrnl_mkdir_unfinish_4, jrnl_mkdir_unfinish_check_commit_dir_exits);
TEST_CASE_MULTIPLE_STAGES("MKDIR - finish replay and exit", "[jrnl_adv]", jrnl_mkdir_unfinish_5, jrnl_mkdir_unfinish_check_commit_dir_exits);


/* *******************************************************************************************
 * RENAME FILE:
 *              - create a file with some contents
 *              - rename the file (committed or not)
 *              - check the filename if committed
 *              - check expected file size (and possibly contents
 */
static const char* s_rename_filename = "testfil3.txt";
static const char* s_rename_filename_new = "newfile3.txt";

static void jrnl_rename_early_exit(uint32_t flags)
{
    test_setup_jrnl(NULL);

    char test_file_name[64] = {0};
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, s_rename_filename);

    struct stat f_stat;
    TEST_ASSERT_EQUAL(-1, stat(test_file_name, &f_stat));

    //get the journal store instance
    esp_jrnl_instance_t *inst_ptr = s_jrnl_instance_ptrs[s_jrnl_handle];
    TEST_ASSERT_NOT_NULL(inst_ptr);
    inst_ptr->test_config = flags;
#ifdef CONFIG_ESP_JRNL_DEBUG_PRINT
    print_jrnl_instance(inst_ptr);
#endif

    //1. create a file with contents directly in the FS
    inst_ptr->test_config |= ESP_JRNL_TEST_SUSPEND_TRANSACTION;
    TEST_ESP_OK(esp_jrnl_set_direct_io(s_jrnl_handle, true));

    FILE *testfile = fopen(test_file_name, "w+");
    TEST_ASSERT_NOT_NULL(testfile);
    TEST_ASSERT(fwrite(pattern_buff, sizeof(pattern_buff), 1, testfile) == 1);
    TEST_ASSERT_EQUAL(0, fclose(testfile));
    TEST_ASSERT_EQUAL(0, stat(test_file_name, &f_stat));
    TEST_ASSERT_EQUAL(sizeof(pattern_buff), f_stat.st_size);

    inst_ptr->test_config &= (~ESP_JRNL_TEST_SUSPEND_TRANSACTION);
    TEST_ESP_OK(esp_jrnl_set_direct_io(s_jrnl_handle, false));

    //rename the file (interrupted)
    char new_file_name[64] = {0};
    snprintf(new_file_name, sizeof(new_file_name), "%s/%s", s_basepath, s_rename_filename_new);
    rename(test_file_name, new_file_name);
}

//check only the old file exists
static void jrnl_rename_unfinish_check(bool commit_running)
{
    //mount journaled FS again
    esp_jrnl_config_t jrnl_cfg = ESP_JRNL_DEFAULT_CONFIG();
    test_setup_jrnl(&jrnl_cfg);

    const char* required_filename = commit_running ? s_rename_filename_new : s_rename_filename;
    const char* unwanted_filename = commit_running ? s_rename_filename : s_rename_filename_new;
    char test_file_name[64] = {0};
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, required_filename);

    //one file must exist...
    FILE* testfile = fopen(test_file_name, "r");
    TEST_ASSERT_NOT_NULL(testfile);
    TEST_ASSERT(fclose(testfile) == 0);

    //... the other cannot
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, unwanted_filename);
    testfile = fopen(test_file_name, "r");
    TEST_ASSERT_EQUAL(NULL, testfile);

    test_teardown_jrnl();
}

//all tests below are analogue to the previous 1-5 steps for CREATE or MKDIR scenarios
static void jrnl_rename_unfinish_1(void)
{
    jrnl_rename_early_exit(ESP_JRNL_TEST_STOP_SKIP_COMMIT);
}

static void jrnl_rename_unfinish_check_not_committing(void)
{
    jrnl_rename_unfinish_check(false);
}

static void jrnl_rename_unfinish_2(void)
{
    jrnl_rename_early_exit(ESP_JRNL_TEST_STOP_SET_COMMIT_AND_EXIT);
}

static void jrnl_rename_unfinish_check_committing(void)
{
    jrnl_rename_unfinish_check(true);
}

static void jrnl_rename_unfinish_3(void)
{
    jrnl_rename_early_exit(ESP_JRNL_TEST_REPLAY_ERASE_AND_EXIT);
}

static void jrnl_rename_unfinish_4(void)
{
    jrnl_rename_early_exit(ESP_JRNL_TEST_REPLAY_WRITE_AND_EXIT);
}

static void jrnl_rename_unfinish_5(void)
{
    jrnl_rename_early_exit(ESP_JRNL_TEST_REPLAY_EXIT_BEFORE_CLOSE);
}

TEST_CASE_MULTIPLE_STAGES("RENAME FILE - skip commit", "[jrnl_adv]", jrnl_rename_unfinish_1, jrnl_rename_unfinish_check_not_committing);
TEST_CASE_MULTIPLE_STAGES("RENAME FILE - start commit and exit", "[jrnl_adv]", jrnl_rename_unfinish_2, jrnl_rename_unfinish_check_committing);
TEST_CASE_MULTIPLE_STAGES("RENAME FILE - start replay and exit", "[jrnl_adv]", jrnl_rename_unfinish_3, jrnl_rename_unfinish_check_committing);
TEST_CASE_MULTIPLE_STAGES("RENAME FILE - continue replay and exit", "[jrnl_adv]", jrnl_rename_unfinish_4, jrnl_rename_unfinish_check_committing);
TEST_CASE_MULTIPLE_STAGES("RENAME FILE - finish replay and exit", "[jrnl_adv]", jrnl_rename_unfinish_5, jrnl_rename_unfinish_check_committing);


void app_main(void)
{
    unity_run_menu();
}
