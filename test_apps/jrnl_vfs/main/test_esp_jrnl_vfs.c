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
#include <fcntl.h>
#include <errno.h>
#include <utime.h>
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
#include "esp_crc.h"


static const char* TAG = "test_esp_jrnl_vfs";
static esp_jrnl_handle_t s_jrnl_handle;
extern esp_jrnl_instance_t* s_jrnl_instance_ptrs[JRNL_MAX_HANDLES];
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

const char* s_basepath = "/spiflash";
const char* s_partlabel = "jrnl";

static uint8_t* s_buf_write = NULL;
static uint8_t* s_buf_read = NULL;


/* jrnl_config == NULL => use default test setup */
static void test_setup_jrnl(esp_jrnl_config_t* jrnl_config)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 5
    };

    esp_jrnl_config_t* jrnl_config_def = NULL;

    if (jrnl_config == NULL) {
        jrnl_config_def = (esp_jrnl_config_t*)malloc(sizeof(esp_jrnl_config_t));
        TEST_ASSERT(jrnl_config_def != NULL);

        jrnl_config_def->store_size_sectors = 32;
        jrnl_config_def->replay_journal_after_mount = false;
        jrnl_config_def->overwrite_existing = true;
        jrnl_config_def->force_fs_format = true;

        jrnl_config = jrnl_config_def;
    }

    ESP_LOGV(TAG, "test_setup_jrnl: esp_vfs_fat_spiflash_mount_rw_wl_jrnl(%s, %s)", s_basepath, s_partlabel);
    TEST_ESP_OK(esp_vfs_fat_spiflash_mount_jrnl(s_basepath, s_partlabel, &mount_config, jrnl_config, &s_jrnl_handle));

    free(jrnl_config_def);
}

static void test_teardown_jrnl(void)
{
    ESP_LOGV(TAG, "test_teardown_jrnl: esp_vfs_fat_spiflash_unmount_rw_wl_jrnl(%s, handle=%ld", s_basepath, s_jrnl_handle);
    TEST_ESP_OK(esp_vfs_fat_spiflash_unmount_jrnl(&s_jrnl_handle, s_basepath));
    TEST_ASSERT(s_jrnl_handle == JRNL_INVALID_HANDLE);
}

/* standard mount, no reformat */
static void test_setup_no_jrnl(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5
    };
    ESP_LOGV(TAG, "test_setup_no_jrnl: esp_vfs_fat_spiflash_mount_rw_wl(%s, %s)", s_basepath, s_partlabel);
    TEST_ESP_OK(esp_vfs_fat_spiflash_mount_rw_wl(s_basepath, s_partlabel, &mount_config, &s_wl_handle));
}

static void test_teardown_no_jrnl(void)
{
    ESP_LOGV(TAG, "test_teardown_jrnl: esp_vfs_fat_spiflash_unmount_rw_wl_jrnl(%s, handle=%ld", s_basepath, s_jrnl_handle);
    TEST_ESP_OK(esp_vfs_fat_spiflash_unmount_rw_wl(s_basepath, s_wl_handle));
}


TEST_GROUP(jrnl_vfs_fat);

TEST_SETUP(jrnl_vfs_fat)
{
    s_buf_write = NULL;
    s_buf_read = NULL;
}

TEST_TEAR_DOWN(jrnl_vfs_fat)
{
    if (s_jrnl_handle != JRNL_INVALID_HANDLE) {
        test_teardown_jrnl();
    }
    free(s_buf_write);
    free(s_buf_read);
}

//fopen, open, fclose, close
TEST(jrnl_vfs_fat, jrnl_create_file)
{
    //1. create new files in journaled FS
    test_setup_jrnl(NULL);

    // - ISO C
    char test_file_name_c[64] = {0};
    snprintf(test_file_name_c, sizeof(test_file_name_c), "%s/%s", s_basepath, "test_c.txt");
    FILE* testfile = fopen(test_file_name_c, "w+");
    TEST_ASSERT_NOT_NULL(testfile);
    TEST_ASSERT(fclose(testfile) == 0);
    testfile = NULL;

    // - Posix
    char test_file_name_p[64] = {0};
    snprintf(test_file_name_p, sizeof(test_file_name_p), "%s/%s", s_basepath, "test_p.txt");
    int fd = open(test_file_name_p, O_WRONLY | O_CREAT | O_TRUNC);
    TEST_ASSERT_NOT_EQUAL(fd, -1);
    TEST_ASSERT_EQUAL(0, close(fd));
    fd = -1;

    test_teardown_jrnl();

    //3. check the files exist in non-journaled FS
    test_setup_no_jrnl();

    testfile = fopen(test_file_name_c, "r");
    TEST_ASSERT_NOT_NULL(testfile);
    TEST_ASSERT(fclose(testfile) == 0);
    fd = open(test_file_name_p, O_RDONLY);
    TEST_ASSERT_NOT_EQUAL(fd, -1);
    TEST_ASSERT_EQUAL(0, close(fd));

    test_teardown_no_jrnl();
}

//write, pwrite, fwrite
TEST(jrnl_vfs_fat, jrnl_write_file)
{
    //1. create new files in journaled FS and write testing contents into them
    test_setup_jrnl(NULL);

    // - ISO C
    char test_file_name_c[64] = {0};
    snprintf(test_file_name_c, sizeof(test_file_name_c), "%s/%s", s_basepath, "test_c.txt");
    FILE* testfile = fopen(test_file_name_c, "w+");
    TEST_ASSERT_NOT_NULL(testfile);

    uint8_t buff[] = "AABBCCDDEEFF";
    TEST_ASSERT(fwrite(buff, sizeof(buff), 1, testfile) > 0);
    TEST_ASSERT(fclose(testfile) == 0);
    testfile = NULL;

    // - Posix
    off_t offset = 0xFF;
    char test_file_name_p[64] = {0};
    snprintf(test_file_name_p, sizeof(test_file_name_p), "%s/%s", s_basepath, "test_p.txt");
    int fd = open(test_file_name_p, O_WRONLY | O_CREAT | O_TRUNC);
    TEST_ASSERT_NOT_EQUAL(fd, -1);
    TEST_ASSERT_NOT_EQUAL(write(fd, buff, sizeof(buff)), -1);
    TEST_ASSERT_NOT_EQUAL(pwrite(fd, buff, sizeof(buff), offset), -1);
    TEST_ASSERT_EQUAL(0, close(fd));
    fd = -1;

    test_teardown_jrnl();

    //3. check the files and their contents in non-journaled FS
    test_setup_no_jrnl();

    s_buf_read = calloc(1, sizeof(buff));
    TEST_ASSERT_NOT_NULL(s_buf_read);

    testfile = fopen(test_file_name_c, "r");
    TEST_ASSERT_NOT_NULL(testfile);
    TEST_ASSERT(fread(s_buf_read, sizeof(buff), 1, testfile) > 0);
    TEST_ASSERT(memcmp(s_buf_read, buff, sizeof(buff)) == 0);
    TEST_ASSERT(fclose(testfile) == 0);

    memset(s_buf_read, 0, sizeof(buff));

    fd = open(test_file_name_p, O_RDONLY);
    TEST_ASSERT_NOT_EQUAL(fd, -1);

    TEST_ASSERT_NOT_EQUAL(read(fd, s_buf_read, sizeof(buff)), -1);
    TEST_ASSERT(memcmp(s_buf_read, buff, sizeof(buff)) == 0);
    memset(s_buf_read, 0, sizeof(buff));
    TEST_ASSERT_NOT_EQUAL(pread(fd, s_buf_read, sizeof(buff), offset), -1);
    TEST_ASSERT(memcmp(s_buf_read, buff, sizeof(buff)) == 0);
    TEST_ASSERT_EQUAL(0, close(fd));

    test_teardown_no_jrnl();
}

//rename - common to Posix and ISO-C
TEST(jrnl_vfs_fat, jrnl_rename_file)
{
    char test_file_name[64] = {0};
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, "testfile.txt");

    //1. create a new file and write testing contents into it in journaled FS
    test_setup_jrnl(NULL);

    FILE* testfile = fopen(test_file_name, "w+");
    TEST_ASSERT_NOT_NULL(testfile);

    uint8_t buff[] = "abcdefghijklmnop";
    TEST_ASSERT(fwrite(buff, sizeof(buff), 1, testfile) > 0);
    TEST_ASSERT(fclose(testfile) == 0);
    testfile = NULL;

    //2. rename file
    char new_file_name[64] = {0};
    snprintf(new_file_name, sizeof(new_file_name), "%s/%s", s_basepath, "newfile.txt");
    TEST_ASSERT(rename(test_file_name, new_file_name) == 0);

    //3. unmount JRNL
    test_teardown_jrnl();

    //4. mount non-journaled FS (don't reformat), and check the old file doesn't exist
    test_setup_no_jrnl();

    testfile = fopen(test_file_name, "r");
    TEST_ASSERT(testfile == NULL);

    //5. check the new filename works, read the contents and compare to the original data
    testfile = fopen(new_file_name, "r");
    TEST_ASSERT_NOT_NULL(testfile);
    s_buf_read = calloc(1, sizeof(buff));
    TEST_ASSERT_NOT_NULL(s_buf_read);
    TEST_ASSERT(fread(s_buf_read, sizeof(buff), 1, testfile) > 0);
    TEST_ASSERT(memcmp(s_buf_read, buff, sizeof(buff)) == 0);
    TEST_ASSERT(fclose(testfile) == 0);

    test_teardown_no_jrnl();
}

//unlink - common to Posix and ISO-C
TEST(jrnl_vfs_fat, jrnl_unlink_file)
{
    char test_file_name[64] = {0};
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, "testfile.txt");

    //1. create a new file in journaled FS
    test_setup_jrnl(NULL);
    FILE* testfile = fopen(test_file_name, "w+");
    TEST_ASSERT_NOT_NULL(testfile);
    TEST_ASSERT(fclose(testfile) == 0);
    testfile = NULL;
    test_teardown_jrnl();

    //2. check the file exists in non-journaled FS
    test_setup_no_jrnl();
    testfile = fopen(test_file_name, "r");
    TEST_ASSERT_NOT_NULL(testfile);
    test_teardown_no_jrnl();

    //3. re-mount JRNL & unlink the file (no reformat!)
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5
    };
    esp_jrnl_config_t jrnl_config = ESP_JRNL_DEFAULT_CONFIG();
    jrnl_config.replay_journal_after_mount = false;
    TEST_ESP_OK(esp_vfs_fat_spiflash_mount_jrnl(s_basepath, s_partlabel, &mount_config, &jrnl_config, &s_jrnl_handle));
    TEST_ASSERT(unlink(test_file_name) == 0);
    test_teardown_jrnl();

    //4. get back to non-journaled FatFS & confirm the file doesn't exist
    test_setup_no_jrnl();
    TEST_ASSERT(fopen(test_file_name, "r") == NULL);
    TEST_ASSERT(errno == ENOENT);
    test_teardown_no_jrnl();
}

//truncate, ftruncate
TEST(jrnl_vfs_fat, jrnl_truncate_file)
{
    const uint8_t buff[] = "GGHHIIJJKKLLMMNN";
    off_t truncate_size = sizeof(buff)/2;

    //1. create new files in journaled FS and write testing contents into them
    test_setup_jrnl(NULL);

    // - ISO C
    char test_file_name_c[64] = {0};
    snprintf(test_file_name_c, sizeof(test_file_name_c), "%s/%s", s_basepath, "test_c.txt");
    FILE* testfile = fopen(test_file_name_c, "w+");
    TEST_ASSERT_NOT_NULL(testfile);
    TEST_ASSERT(fwrite(buff, sizeof(buff), 1, testfile) > 0);
    TEST_ASSERT(fclose(testfile) == 0);
    testfile = NULL;

    TEST_ASSERT(truncate(test_file_name_c, truncate_size) == 0);

    // - Posix
    char test_file_name_p[64] = {0};
    snprintf(test_file_name_p, sizeof(test_file_name_p), "%s/%s", s_basepath, "test_p.txt");
    int fd = open(test_file_name_p, O_WRONLY | O_CREAT | O_TRUNC);
    TEST_ASSERT_NOT_EQUAL(fd, -1);
    TEST_ASSERT_NOT_EQUAL(write(fd, buff, sizeof(buff)), -1);

    TEST_ASSERT(ftruncate(fd, truncate_size) == 0);

    TEST_ASSERT_EQUAL(0, close(fd));
    fd = -1;

    test_teardown_jrnl();

    //2. mount the same volume without JRNL (don't reformat)
    test_setup_no_jrnl();

    //3. check the files exist and have expected size
    testfile = fopen(test_file_name_c, "r");
    TEST_ASSERT_NOT_NULL(testfile);
    fseek(testfile, 0L, SEEK_END);
    TEST_ASSERT(ftell(testfile) == truncate_size);
    TEST_ASSERT(fclose(testfile) == 0);
    testfile = NULL;

    fd = open(test_file_name_p, O_RDONLY);
    TEST_ASSERT_NOT_EQUAL(fd, -1);
    struct stat f_stat;
    TEST_ASSERT_NOT_EQUAL(fstat(fd, &f_stat), -1);
    TEST_ASSERT(f_stat.st_size == truncate_size);
    TEST_ASSERT_EQUAL(0, close(fd));

    test_teardown_no_jrnl();
}

//utime
TEST(jrnl_vfs_fat, jrnl_utime)
{
    //1. create new file in journaled FS
    test_setup_jrnl(NULL);

    char test_file_name[64] = {0};
    snprintf(test_file_name, sizeof(test_file_name), "%s/%s", s_basepath, "test.txt");
    FILE* testfile = fopen(test_file_name, "w+");
    TEST_ASSERT_NOT_NULL(testfile);
    TEST_ASSERT(fclose(testfile) == 0);
    testfile = NULL;

    //2. change file-modified time (10:11:12, April 1, 2020)
    struct tm test_tm;
    struct utimbuf test_time = {
            .actime = 0, // access time is not supported
            .modtime = 0,
    };
    test_tm.tm_mon = 3;
    test_tm.tm_mday = 1;
    test_tm.tm_year = 2020 - 1900;
    test_tm.tm_hour = 10;
    test_tm.tm_min = 11;
    test_tm.tm_sec = 12;

    test_time.modtime = mktime(&test_tm);
    TEST_ASSERT_NOT_EQUAL(test_time.modtime, -1);
    TEST_ASSERT_EQUAL(0, utime(test_file_name, &test_time));

    test_teardown_jrnl();

    //3. mount non-journaled FS and check the file time is changed properly
    test_setup_no_jrnl();

    struct stat f_stat;
    TEST_ASSERT_EQUAL(0, stat(test_file_name, &f_stat));
    TEST_ASSERT_EQUAL_UINT32(test_time.modtime, f_stat.st_mtime);

    test_teardown_no_jrnl();
}

//mkdir, rmdir
TEST(jrnl_vfs_fat, jrnl_mkdir_rmdir)
{
    char test_dir_name[64] = {0};
    snprintf(test_dir_name, sizeof(test_dir_name), "%s/%s", s_basepath, "testdir");

    //1. create a new directory in JRNL system
    test_setup_jrnl(NULL);
    TEST_ASSERT_EQUAL(0, mkdir(test_dir_name, 0777));
    test_teardown_jrnl();

    //2. mount the same volume without JRNL (don't reformat) & check the directory exists
    test_setup_no_jrnl();
    struct stat f_stat;
    TEST_ASSERT_NOT_EQUAL(stat(test_dir_name, &f_stat), -1);
    TEST_ASSERT(f_stat.st_mode & S_IFDIR);
    test_teardown_no_jrnl();

    //3. mount JRNL back and remove the dir
    esp_jrnl_config_t jrnl_cfg = ESP_JRNL_DEFAULT_CONFIG();
    jrnl_cfg.replay_journal_after_mount = false;
    test_setup_jrnl(&jrnl_cfg);
    TEST_ASSERT_EQUAL(0, rmdir(test_dir_name));
    test_teardown_jrnl();

    //4. check the directory is removed in non journaled FS
    test_setup_no_jrnl();
    TEST_ASSERT_TRUE(stat(test_dir_name, &f_stat) == -1 && errno == ENOENT);
    test_teardown_no_jrnl();
}

//create (any) record, don't finish the transaction, check the FS remains untouched
TEST(jrnl_vfs_fat, jrnl_create_nocommit)
{
    char test_dir_name[64] = {0};
    snprintf(test_dir_name, sizeof(test_dir_name), "%s/%s", s_basepath, "testdir");

    //1. create a new directory in JRNL system
    test_setup_jrnl(NULL);
    TEST_ASSERT_EQUAL(0, mkdir(test_dir_name, 0777));
    test_teardown_jrnl();

    //2. mount the same volume without JRNL (don't reformat) & check the directory exists
    test_setup_no_jrnl();
    struct stat f_stat;
    TEST_ASSERT_NOT_EQUAL(stat(test_dir_name, &f_stat), -1);
    TEST_ASSERT(f_stat.st_mode & S_IFDIR);
    test_teardown_no_jrnl();

    //3. mount JRNL back and remove the dir
    esp_jrnl_config_t jrnl_cfg = ESP_JRNL_DEFAULT_CONFIG();
    jrnl_cfg.replay_journal_after_mount = false;
    test_setup_jrnl(&jrnl_cfg);
    TEST_ASSERT_EQUAL(0, rmdir(test_dir_name));
    test_teardown_jrnl();

    //4. check the directory is removed in non journaled FS
    test_setup_no_jrnl();
    TEST_ASSERT_TRUE(stat(test_dir_name, &f_stat) == -1 && errno == ENOENT);
    test_teardown_no_jrnl();
}

TEST_GROUP_RUNNER(fs_journaling_vfs)
{
    RUN_TEST_CASE(jrnl_vfs_fat, jrnl_create_file);
    RUN_TEST_CASE(jrnl_vfs_fat, jrnl_write_file);
    RUN_TEST_CASE(jrnl_vfs_fat, jrnl_rename_file);
    RUN_TEST_CASE(jrnl_vfs_fat, jrnl_unlink_file);
    RUN_TEST_CASE(jrnl_vfs_fat, jrnl_truncate_file);
    RUN_TEST_CASE(jrnl_vfs_fat, jrnl_utime);
    RUN_TEST_CASE(jrnl_vfs_fat, jrnl_mkdir_rmdir);
}

void app_main(void)
{
    UNITY_MAIN(fs_journaling_vfs);
}
