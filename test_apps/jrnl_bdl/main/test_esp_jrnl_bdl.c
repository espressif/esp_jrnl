/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Test app for the esp_jrnl Block Device Layer (BDL) integration.
 *
 * The canonical stack exercised here is:
 *     esp_partition  ->  wear_levelling  ->  esp_jrnl  ->  FatFS (via diskio_bdl)
 * built using the *_get_blockdev() factories from each layer. Transactions are driven from
 * the test bodies via the ESP_JRNL_IOCTL_TRANS_* commands, simulating both manual user
 * control and IDF-internal "in-code" deployment.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "esp_log.h"
#include "esp_blockdev.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "esp_jrnl.h"
#include "esp_jrnl_internal.h"   /* exposes esp_jrnl_master_t / JRNL_STORE_MARKER for assertions */
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "diskio_bdl.h"
#include "ff.h"

static const char *TAG = "test_jrnl_bdl";

static const char *PART_LABEL = "jrnl";

/* ===================================================================== */
/*  Stack helpers                                                        */
/* ===================================================================== */

typedef struct {
    esp_blockdev_handle_t partition_bdl;
    esp_blockdev_handle_t wl_bdl;
    esp_blockdev_handle_t jrnl_bdl;
} jrnl_bdl_stack_t;

static void stack_build(jrnl_bdl_stack_t *stk, const esp_jrnl_config_t *jrnl_cfg)
{
    memset(stk, 0, sizeof(*stk));

    TEST_ESP_OK(esp_partition_get_blockdev(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
                                           PART_LABEL, &stk->partition_bdl));
    TEST_ASSERT_NOT_NULL(stk->partition_bdl);
    ESP_LOGI(TAG, "partition BDL: disk_size=%llu, erase_size=%u",
             (unsigned long long)stk->partition_bdl->geometry.disk_size,
             (unsigned)stk->partition_bdl->geometry.erase_size);

    TEST_ESP_OK(wl_get_blockdev(stk->partition_bdl, &stk->wl_bdl));
    TEST_ASSERT_NOT_NULL(stk->wl_bdl);
    ESP_LOGI(TAG, "wl       BDL: disk_size=%llu, erase_size=%u",
             (unsigned long long)stk->wl_bdl->geometry.disk_size,
             (unsigned)stk->wl_bdl->geometry.erase_size);

    TEST_ESP_OK(esp_jrnl_get_blockdev(stk->wl_bdl, jrnl_cfg, &stk->jrnl_bdl));
    TEST_ASSERT_NOT_NULL(stk->jrnl_bdl);
    ESP_LOGI(TAG, "jrnl     BDL: disk_size=%llu, erase_size=%u, write_size=%u",
             (unsigned long long)stk->jrnl_bdl->geometry.disk_size,
             (unsigned)stk->jrnl_bdl->geometry.erase_size,
             (unsigned)stk->jrnl_bdl->geometry.write_size);
}

static void stack_tear_down(jrnl_bdl_stack_t *stk)
{
    if (stk->jrnl_bdl)      { stk->jrnl_bdl->ops->release(stk->jrnl_bdl);      stk->jrnl_bdl = NULL; }
    if (stk->wl_bdl)        { stk->wl_bdl->ops->release(stk->wl_bdl);          stk->wl_bdl = NULL; }
    if (stk->partition_bdl) { stk->partition_bdl->ops->release(stk->partition_bdl); stk->partition_bdl = NULL; }
}

/* ===================================================================== */
/*  Test cases                                                           */
/* ===================================================================== */

/*
 * 1. Mount/unmount on a fresh partition; verify exposed geometry.
 */
TEST_CASE("(jrnl-bdl) factory: build + release the BDL stack", "[jrnl][bdl]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    TEST_ASSERT(stk.jrnl_bdl->geometry.disk_size > 0);
    TEST_ASSERT(stk.jrnl_bdl->geometry.disk_size <= stk.wl_bdl->geometry.disk_size);
    TEST_ASSERT_EQUAL(stk.wl_bdl->geometry.erase_size, stk.jrnl_bdl->geometry.erase_size);
    TEST_ASSERT_EQUAL(stk.wl_bdl->geometry.erase_size, stk.jrnl_bdl->geometry.write_size);

    /* The output disk_size must equal the bottom disk_size minus the journal store.
     * Compared as size_t because the embedded Unity in IDF is built without
     * CONFIG_UNITY_ENABLE_64BIT by default; the actual partition sizes fit in 32 bits. */
    size_t expected = (size_t)stk.wl_bdl->geometry.disk_size
                    - (size_t)cfg.store_size_sectors * (size_t)stk.wl_bdl->geometry.erase_size;
    TEST_ASSERT_EQUAL(expected, (size_t)stk.jrnl_bdl->geometry.disk_size);

    /* device_flags must be propagated unchanged from the bottom. */
    TEST_ASSERT_EQUAL(stk.wl_bdl->device_flags.read_only, stk.jrnl_bdl->device_flags.read_only);
    TEST_ASSERT_EQUAL(stk.wl_bdl->device_flags.erase_before_write, stk.jrnl_bdl->device_flags.erase_before_write);
    TEST_ASSERT_EQUAL(stk.wl_bdl->device_flags.and_type_write, stk.jrnl_bdl->device_flags.and_type_write);

    stack_tear_down(&stk);
}

/*
 * 2. Read/write/erase boundary checks: alignment + OOB.
 */
TEST_CASE("(jrnl-bdl) write/erase: alignment + OOB rejection", "[jrnl][bdl]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    /* Switch to direct mode so the bdl write/erase actually exercise the bottom layer. */
    bool direct = true;
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_SET_DIRECT_IO, &direct));

    const size_t sec = stk.jrnl_bdl->geometry.erase_size;
    uint8_t *buf = calloc(1, sec);
    TEST_ASSERT_NOT_NULL(buf);

    /* Misaligned start address. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      stk.jrnl_bdl->ops->write(stk.jrnl_bdl, buf, 1, sec));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      stk.jrnl_bdl->ops->erase(stk.jrnl_bdl, 1, sec));

    /* Misaligned length. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      stk.jrnl_bdl->ops->write(stk.jrnl_bdl, buf, 0, sec + 3));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      stk.jrnl_bdl->ops->erase(stk.jrnl_bdl, 0, sec + 3));

    /* OOB. */
    uint64_t oob = stk.jrnl_bdl->geometry.disk_size;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      stk.jrnl_bdl->ops->write(stk.jrnl_bdl, buf, oob, sec));

    free(buf);

    direct = false;
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_SET_DIRECT_IO, &direct));

    stack_tear_down(&stk);
}

/*
 * 3. Full transaction lifecycle: START -> multiple writes -> COMMIT; verify data on bottom
 *    and verify status transitions via GET_STATUS along the way.
 */
TEST_CASE("(jrnl-bdl) ioctl: TRANS_START / writes / TRANS_COMMIT", "[jrnl][bdl]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    esp_jrnl_trans_status_t status;
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_GET_STATUS, &status));
    TEST_ASSERT_EQUAL(ESP_JRNL_STATUS_TRANS_READY, status);

    const size_t sec = stk.jrnl_bdl->geometry.erase_size;
    uint8_t *pattern = malloc(sec);
    uint8_t *readback = malloc(sec);
    TEST_ASSERT_NOT_NULL(pattern);
    TEST_ASSERT_NOT_NULL(readback);
    memset(pattern, 0xA5, sec);

    /* Begin transaction. */
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_GET_STATUS, &status));
    TEST_ASSERT_EQUAL(ESP_JRNL_STATUS_TRANS_OPEN, status);

    /* Three writes at different offsets. */
    const uint64_t addrs[] = { 3 * sec, 7 * sec, 11 * sec };
    for (size_t i = 0; i < sizeof(addrs)/sizeof(addrs[0]); ++i) {
        TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, pattern, addrs[i], sec));
    }

    /* Reads during the open transaction overlay journaled writes onto the bottom content. */
    for (size_t i = 0; i < sizeof(addrs)/sizeof(addrs[0]); ++i) {
        memset(readback, 0, sec);
        TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, addrs[i], sec));
        TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sec);
    }

    /* Commit and verify replayed data is now present at the target addresses. */
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_GET_STATUS, &status));
    TEST_ASSERT_EQUAL(ESP_JRNL_STATUS_TRANS_READY, status);

    for (size_t i = 0; i < sizeof(addrs)/sizeof(addrs[0]); ++i) {
        memset(readback, 0, sec);
        TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, addrs[i], sec));
        TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sec);
    }

    free(pattern);
    free(readback);
    stack_tear_down(&stk);
}

/*
 * 4. TRANS_CANCEL discards writes without touching the bottom.
 */
TEST_CASE("(jrnl-bdl) ioctl: TRANS_CANCEL discards journaled writes", "[jrnl][bdl]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    const size_t sec = stk.jrnl_bdl->geometry.erase_size;
    uint8_t *pattern  = malloc(sec);
    uint8_t *before   = malloc(sec);
    uint8_t *readback = malloc(sec);
    TEST_ASSERT_NOT_NULL(pattern);
    TEST_ASSERT_NOT_NULL(before);
    TEST_ASSERT_NOT_NULL(readback);
    memset(pattern, 0x5A, sec);

    const uint64_t target = 5 * sec;

    /* Snapshot the target sector before the transaction. */
    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, before, sec, target, sec));

    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, pattern, target, sec));
    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, target, sec));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sec);
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_CANCEL, NULL));

    /* The target sector must still contain its pre-transaction content. */
    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, target, sec));
    TEST_ASSERT_EQUAL_MEMORY(before, readback, sec);

    free(pattern);
    free(before);
    free(readback);
    stack_tear_down(&stk);
}

/*
 * 5. ioctl rejection rules: nested START, COMMIT/CANCEL without START, SET_DIRECT_IO
 *    during an open transaction, system ioctl during an open transaction.
 */
TEST_CASE("(jrnl-bdl) ioctl: rejection rules", "[jrnl][bdl]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;
    cfg.implicit_transactions = false;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    /* COMMIT / CANCEL without a transaction. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_CANCEL, NULL));

    /* Open a transaction. */
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));

    /* Nested START is rejected. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));

    /* SET_DIRECT_IO during open transaction is rejected. */
    bool direct = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_SET_DIRECT_IO, &direct));

    /* System ioctls during open transaction: rejected. */
    esp_blockdev_cmd_arg_erase_t earg = { .start_addr = 0, .erase_len = stk.jrnl_bdl->geometry.erase_size };
    esp_err_t sys_err = stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_BLOCKDEV_CMD_MARK_DELETED, &earg);
    /* Either the journal explicitly rejected it (INVALID_STATE) or the bottom does not
     * implement it. Both are valid outcomes for v1; INVALID_STATE is what we expect when
     * the bottom does have an ioctl impl. */
    TEST_ASSERT_TRUE(sys_err == ESP_ERR_INVALID_STATE
                  || sys_err == ESP_ERR_NOT_SUPPORTED);

    /* SET_DIRECT_IO with NULL args. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_SET_DIRECT_IO, NULL));
    /* GET_STATUS with NULL args. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_GET_STATUS, NULL));

    /* Clean up. */
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_CANCEL, NULL));

    /* Write outside of a transaction (TRANS_READY) is rejected. The buffer must be heap-
     * allocated: a stack-resident FF_MAX_SS array (4 KiB with the default
     * CONFIG_WL_SECTOR_SIZE) immediately blows past the 3584-byte main_task stack. */
    const size_t write_size = stk.jrnl_bdl->geometry.write_size;
    uint8_t *dummy = calloc(1, write_size);
    TEST_ASSERT_NOT_NULL(dummy);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      stk.jrnl_bdl->ops->write(stk.jrnl_bdl, dummy, 0, write_size));
    free(dummy);

    stack_tear_down(&stk);
}

/*
 * Implicit standalone write: default implicit_transactions wraps READY write in START+COMMIT.
 */
TEST_CASE("(jrnl-bdl) implicit transaction: standalone write commits", "[jrnl][bdl]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    const size_t sec = stk.jrnl_bdl->geometry.write_size;
    uint8_t *pattern = malloc(sec);
    uint8_t *readback = malloc(sec);
    TEST_ASSERT_NOT_NULL(pattern);
    TEST_ASSERT_NOT_NULL(readback);
    memset(pattern, 0x3C, sec);

    const uint64_t target = 4 * sec;
    TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, pattern, target, sec));

    esp_jrnl_trans_status_t status;
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_GET_STATUS, &status));
    TEST_ASSERT_EQUAL(ESP_JRNL_STATUS_TRANS_READY, status);

    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, target, sec));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sec);

    free(pattern);
    free(readback);
    stack_tear_down(&stk);
}

/*
 * Journaled erase is visible through read-through and discarded on CANCEL.
 */
TEST_CASE("(jrnl-bdl) journaled erase: overlay then cancel", "[jrnl][bdl]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    const size_t sec = stk.jrnl_bdl->geometry.erase_size;
    uint8_t *pattern = malloc(sec);
    uint8_t *before = malloc(sec);
    uint8_t *readback = malloc(sec);
    TEST_ASSERT_NOT_NULL(pattern);
    TEST_ASSERT_NOT_NULL(before);
    TEST_ASSERT_NOT_NULL(readback);
    memset(pattern, 0xA7, sec);

    const uint64_t target = 6 * sec;
    TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, pattern, target, sec));
    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, before, sec, target, sec));
    TEST_ASSERT_EQUAL_MEMORY(pattern, before, sec);

    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->erase(stk.jrnl_bdl, target, sec));
    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, target, sec));
    for (size_t i = 0; i < sec; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, readback[i]);
    }
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_CANCEL, NULL));

    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, target, sec));
    TEST_ASSERT_EQUAL_MEMORY(before, readback, sec);

    free(pattern);
    free(before);
    free(readback);
    stack_tear_down(&stk);
}

typedef struct {
    esp_blockdev_handle_t jrnl_bdl;
    SemaphoreHandle_t done;
    esp_err_t err;
} jrnl_cross_task_ctx_t;

static void jrnl_cross_task_commit(void *arg)
{
    jrnl_cross_task_ctx_t *ctx = (jrnl_cross_task_ctx_t *)arg;
    ctx->err = ctx->jrnl_bdl->ops->ioctl(ctx->jrnl_bdl, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

/*
 * COMMIT from a different task than START must succeed (window is a binary semaphore).
 */
TEST_CASE("(jrnl-bdl) cross-task COMMIT", "[jrnl][bdl]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    const size_t sec = stk.jrnl_bdl->geometry.write_size;
    uint8_t *pattern = malloc(sec);
    uint8_t *readback = malloc(sec);
    TEST_ASSERT_NOT_NULL(pattern);
    TEST_ASSERT_NOT_NULL(readback);
    memset(pattern, 0x19, sec);

    const uint64_t target = 2 * sec;
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, pattern, target, sec));

    jrnl_cross_task_ctx_t ctx = {
        .jrnl_bdl = stk.jrnl_bdl,
        .done = xSemaphoreCreateBinary(),
        .err = ESP_FAIL,
    };
    TEST_ASSERT_NOT_NULL(ctx.done);
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(jrnl_cross_task_commit, "jrnl_cmt", 3072, &ctx, 5, NULL));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(ctx.done, pdMS_TO_TICKS(2000)));
    TEST_ESP_OK(ctx.err);
    vSemaphoreDelete(ctx.done);

    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, target, sec));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sec);

    free(pattern);
    free(readback);
    stack_tear_down(&stk);
}

#ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE

typedef struct {
    esp_blockdev_handle_t jrnl_bdl;
    const uint8_t *buff;
    uint64_t offset;
    size_t len;
    SemaphoreHandle_t done;
    esp_err_t err;
} jrnl_race_write_ctx_t;

static void jrnl_race_writer(void *arg)
{
    jrnl_race_write_ctx_t *ctx = (jrnl_race_write_ctx_t *)arg;
    ctx->err = ctx->jrnl_bdl->ops->write(ctx->jrnl_bdl, ctx->buff, ctx->offset, ctx->len);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

/*
 * Regression test: a journal append must not land in a store that was committed underneath it.
 *
 * The status check that routes a write to the journal is made without trans_lock, and since
 * COMMIT may be issued from any task, the transaction can close between that check and the
 * actual append. ESP_JRNL_TEST_LOG_RACE_DELAY widens that gap deterministically:
 *
 *   writer task : sees TRANS_OPEN -> [delay] ------------------> append
 *   this task   :                    -> TRANS_COMMIT (replay, master reset to READY)
 *
 * The append must be refused. Before the status was re-validated under trans_lock it would
 * succeed, writing a record into a just-reset store and advancing next_free_sector while the
 * master said READY - i.e. a corrupt journal that the next TRANS_START would trip over.
 */
TEST_CASE("(jrnl-bdl) append racing a cross-task COMMIT is refused", "[jrnl][bdl][concurrency]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    esp_jrnl_instance_t *inst = (esp_jrnl_instance_t *)stk.jrnl_bdl->ctx;
    const size_t sec = stk.jrnl_bdl->geometry.write_size;

    uint8_t *committed = malloc(sec);
    uint8_t *racing = malloc(sec);
    uint8_t *readback = malloc(sec);
    TEST_ASSERT_NOT_NULL(committed);
    TEST_ASSERT_NOT_NULL(racing);
    TEST_ASSERT_NOT_NULL(readback);
    memset(committed, 0x5A, sec);
    memset(racing, 0xA5, sec);

    const uint64_t committed_at = 2 * sec;
    const uint64_t racing_at    = 3 * sec;

    /* Seed racing_at with a known pattern in its own committed transaction. Asserting against a
     * seed rather than the erased value keeps the test independent of what earlier cases left on
     * the media (overwrite_existing resets the journal, not the data area). The seed must differ
     * from the racing pattern so a leaked append is detectable. */
    memset(readback, 0x3C, sec);
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, readback, racing_at, sec));
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL));

    /* Open a transaction with one legitimate record in it. */
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, committed, committed_at, sec));

    /* Arm the hook and launch a writer that will stall between its status check and its append. */
    inst->test_config |= ESP_JRNL_TEST_LOG_RACE_DELAY;

    jrnl_race_write_ctx_t ctx = {
        .jrnl_bdl = stk.jrnl_bdl,
        .buff = racing,
        .offset = racing_at,
        .len = sec,
        .done = xSemaphoreCreateBinary(),
        .err = ESP_FAIL,
    };
    TEST_ASSERT_NOT_NULL(ctx.done);
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(jrnl_race_writer, "jrnl_race", 4096, &ctx, 5, NULL));

    /* Let the writer get past its status check and into the delay, then commit underneath it.
     * Clear the flag first so the commit path itself is never delayed. */
    vTaskDelay(pdMS_TO_TICKS(200));
    inst->test_config &= ~ESP_JRNL_TEST_LOG_RACE_DELAY;
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL));

    /* The commit must have completed and closed the store before the writer wakes. */
    TEST_ASSERT_EQUAL(ESP_JRNL_STATUS_TRANS_READY, inst->master.status);

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(ctx.done, pdMS_TO_TICKS(10000)));
    vSemaphoreDelete(ctx.done);

    /* The racing append must have been refused... */
    TEST_ASSERT_EQUAL_HEX32(ESP_ERR_INVALID_STATE, ctx.err);

    /* ...and must have left no trace in the journal store or on the media. */
    TEST_ASSERT_EQUAL_UINT32(0, inst->master.next_free_sector);
    TEST_ASSERT_EQUAL(ESP_JRNL_STATUS_TRANS_READY, inst->master.status);

    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, committed_at, sec));
    TEST_ASSERT_EQUAL_MEMORY(committed, readback, sec);

    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, racing_at, sec));
    TEST_ASSERT_EACH_EQUAL_HEX8(0x3C, readback, sec);

    /* A store left consistent by the rejection accepts a new transaction. */
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_CANCEL, NULL));

    free(committed);
    free(racing);
    free(readback);
    stack_tear_down(&stk);
}

#endif /* CONFIG_ESP_JRNL_ENABLE_TESTMODE */

/*
 * 6. Replay-after-restart: leave a COMMIT-state journal on disk (by faulting after the master
 *    update but before replay completes), then reopen the stack and confirm jrnl_replay()
 *    completes the transaction. We simulate the fault by releasing the stack mid-commit
 *    using the testmode flag if available; otherwise we settle for the easier check that a
 *    clean commit followed by re-open still replays correctly.
 */
TEST_CASE("(jrnl-bdl) replay: persistence across release+rebuild", "[jrnl][bdl]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    const size_t sec = stk.jrnl_bdl->geometry.erase_size;
    uint8_t *pattern  = malloc(sec);
    uint8_t *readback = malloc(sec);
    TEST_ASSERT_NOT_NULL(pattern);
    TEST_ASSERT_NOT_NULL(readback);
    memset(pattern, 0x77, sec);

    const uint64_t target = 9 * sec;

    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, pattern, target, sec));
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL));

    stack_tear_down(&stk);

    /* Rebuild WITHOUT overwriting. The new mount should observe the previous state
     * (TRANS_READY, magic mark present) and not modify anything. */
    cfg.overwrite_existing = false;
    cfg.replay_journal_after_mount = true;
    stack_build(&stk, &cfg);

    /* Committed data must still be present on the bottom. */
    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, readback, sec, target, sec));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sec);

    free(pattern);
    free(readback);
    stack_tear_down(&stk);
}

/*
 * 7. FATFS-over-journaled-BDL smoke: format in FS_DIRECT, then write a file with no
 *    explicit ioctl wrapping. Default implicit_transactions plus read-through let FatFS
 *    disk I/O run as standalone journaled operations.
 */
TEST_CASE("(jrnl-bdl) FatFS over journaled BDL: implicit transactions", "[jrnl][bdl][fatfs]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;
    cfg.force_fs_format = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    /* Allow direct (non-journaled) I/O during format and mount. */
    bool direct = true;
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_SET_DIRECT_IO, &direct));

    BYTE pdrv;
    TEST_ESP_OK(ff_diskio_get_drive(&pdrv));
    TEST_ESP_OK(ff_diskio_register_bdl(pdrv, stk.jrnl_bdl));
    char drv[3] = {(char)('0' + pdrv), ':', 0};

    /* Format. work_area must be heap-allocated for the same reason as in test 5:
     * FF_MAX_SS is 4 KiB with the default CONFIG_WL_SECTOR_SIZE and would not fit on
     * the 3584-byte main_task stack alongside the rest of this function's locals. */
    BYTE *work_area = malloc(FF_MAX_SS);
    TEST_ASSERT_NOT_NULL(work_area);
    const MKFS_PARM opt = {(BYTE)(FM_ANY | FM_SFD), 2, 0, 0, 0};
    TEST_ASSERT_EQUAL(FR_OK, f_mkfs(drv, &opt, work_area, FF_MAX_SS));
    free(work_area);

    FATFS fs;
#if FF_USE_DYN_BUFFER
    fs.win = NULL;
#endif
    TEST_ASSERT_EQUAL(FR_OK, f_mount(&fs, drv, 1));

    /* Switch back to journaled mode for file writes. */
    direct = false;
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_SET_DIRECT_IO, &direct));

    const char *payload = "hello-from-journaled-bdl";
    FIL file;
#if !FF_FS_TINY && FF_USE_DYN_BUFFER
    file.buf = NULL;
#endif
    UINT bw = 0;

    TEST_ASSERT_EQUAL(FR_OK, f_open(&file, "trx.txt", FA_CREATE_ALWAYS | FA_READ | FA_WRITE));
    TEST_ASSERT_EQUAL(FR_OK, f_write(&file, payload, strlen(payload), &bw));
    TEST_ASSERT_EQUAL(strlen(payload), bw);
    TEST_ASSERT_EQUAL(FR_OK, f_close(&file));

    /* Read it back after commit (no transaction needed). */
    TEST_ASSERT_EQUAL(FR_OK, f_open(&file, "trx.txt", FA_OPEN_EXISTING | FA_READ));
    char rb[64] = {0};
    UINT br = 0;
    TEST_ASSERT_EQUAL(FR_OK, f_read(&file, rb, sizeof(rb) - 1, &br));
    TEST_ASSERT_EQUAL(strlen(payload), br);
    TEST_ASSERT_EQUAL_STRING(payload, rb);
    TEST_ASSERT_EQUAL(FR_OK, f_close(&file));

    TEST_ASSERT_EQUAL(FR_OK, f_mount(0, drv, 0));
    ff_diskio_unregister(pdrv);
    ff_diskio_clear_pdrv_bdl(stk.jrnl_bdl);

    stack_tear_down(&stk);
}

/* ===================================================================== */
/*  Power-off recovery on the BDL ioctl path                             */
/*                                                                       */
/*  These mirror the legacy jrnl_adv power-off matrix but drive the      */
/*  transaction exclusively through ESP_JRNL_IOCTL_TRANS_*. Each case is */
/*  a two-stage test: stage 1 arms an interruption point and triggers a  */
/*  real esp_restart() inside the journaling core; stage 2 reopens the   */
/*  stack with replay enabled and asserts the journal resolved the       */
/*  interrupted transaction the expected way (rollback vs fast-forward). */
/*  The bottom partition/wl layers persist across the reset, which is    */
/*  exactly why these run on the real stack (not the RAM-backed mock).   */
/* ===================================================================== */

#ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE

#define REC_BASE_FILL   0xB0    /* committed baseline pattern */
#define REC_NEW_FILL    0xC1    /* pattern written by the interrupted transaction */
#define REC_TARGET_IDX  9       /* target sector index (well inside the FS region) */

/* stage 1: fresh store -> commit a baseline -> open a second transaction writing a new
 * pattern -> arm a power-off fault and issue the commit. The commit ioctl must NOT return:
 * the journaling core calls esp_restart() at the armed interruption point. */
static void jrnl_bdl_recovery_setup(uint32_t fault_flag)
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;
    cfg.replay_journal_after_mount = false;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    const size_t sec = stk.jrnl_bdl->geometry.erase_size;
    const uint64_t target = (uint64_t)REC_TARGET_IDX * sec;
    uint8_t *buf = malloc(sec);
    TEST_ASSERT_NOT_NULL(buf);

    /* Committed baseline so the interrupted transaction has a known "before" state. */
    memset(buf, REC_BASE_FILL, sec);
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, buf, target, sec));
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL));

    /* Second transaction with the new pattern - this is the one we interrupt. */
    memset(buf, REC_NEW_FILL, sec);
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(stk.jrnl_bdl->ops->write(stk.jrnl_bdl, buf, target, sec));
    free(buf);

    /* Arm the interruption point on the live instance and commit. */
    ((esp_jrnl_instance_t *)stk.jrnl_bdl->ctx)->test_config = fault_flag;
    stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL);

    /* Unreachable if the fault fired as expected. */
    TEST_FAIL_MESSAGE("expected power-off (esp_restart) during commit, but the ioctl returned");
}

/* stage 2: reopen with replay and check the resolved on-disk content.
 *  expect_committed == true  -> interrupted commit was completed  (target == new pattern)
 *  expect_committed == false -> interrupted commit was rolled back (target == baseline)  */
static void jrnl_bdl_recovery_check(bool expect_committed)
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = false;
    cfg.replay_journal_after_mount = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    /* After replay/rollback the store must be back to a clean READY state. */
    esp_jrnl_trans_status_t status;
    TEST_ESP_OK(stk.jrnl_bdl->ops->ioctl(stk.jrnl_bdl, ESP_JRNL_IOCTL_GET_STATUS, &status));
    TEST_ASSERT_EQUAL(ESP_JRNL_STATUS_TRANS_READY, status);

    const size_t sec = stk.jrnl_bdl->geometry.erase_size;
    const uint64_t target = (uint64_t)REC_TARGET_IDX * sec;
    uint8_t *rb = malloc(sec);
    uint8_t *expected = malloc(sec);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_NOT_NULL(expected);
    memset(expected, expect_committed ? REC_NEW_FILL : REC_BASE_FILL, sec);

    TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, rb, sec, target, sec));
    TEST_ASSERT_EQUAL_MEMORY(expected, rb, sec);

    free(rb);
    free(expected);
    stack_tear_down(&stk);
}

/* a) interrupted before the store is flagged COMMIT -> transaction discarded (rollback). */
static void jrnl_bdl_rec_skip_commit_a(void)  { jrnl_bdl_recovery_setup(ESP_JRNL_TEST_STOP_SKIP_COMMIT); }
static void jrnl_bdl_rec_skip_commit_b(void)  { jrnl_bdl_recovery_check(false); }
TEST_CASE_MULTIPLE_STAGES("(jrnl-bdl) recovery: power-off before commit -> rollback",
                          "[jrnl][bdl][recovery]",
                          jrnl_bdl_rec_skip_commit_a, jrnl_bdl_rec_skip_commit_b);

/* b) interrupted right after the store is flagged COMMIT, before any transfer -> completed. */
static void jrnl_bdl_rec_set_commit_a(void)   { jrnl_bdl_recovery_setup(ESP_JRNL_TEST_STOP_SET_COMMIT_AND_EXIT); }
static void jrnl_bdl_rec_set_commit_b(void)   { jrnl_bdl_recovery_check(true); }
TEST_CASE_MULTIPLE_STAGES("(jrnl-bdl) recovery: power-off after commit flag -> replay completes",
                          "[jrnl][bdl][recovery]",
                          jrnl_bdl_rec_set_commit_a, jrnl_bdl_rec_set_commit_b);

/* c) interrupted after erasing the first target sector during replay -> completed. */
static void jrnl_bdl_rec_replay_erase_a(void) { jrnl_bdl_recovery_setup(ESP_JRNL_TEST_REPLAY_ERASE_AND_EXIT); }
static void jrnl_bdl_rec_replay_erase_b(void) { jrnl_bdl_recovery_check(true); }
TEST_CASE_MULTIPLE_STAGES("(jrnl-bdl) recovery: power-off mid-replay (erase) -> replay completes",
                          "[jrnl][bdl][recovery]",
                          jrnl_bdl_rec_replay_erase_a, jrnl_bdl_rec_replay_erase_b);

/* d) interrupted after writing the first target sector during replay -> completed. */
static void jrnl_bdl_rec_replay_write_a(void) { jrnl_bdl_recovery_setup(ESP_JRNL_TEST_REPLAY_WRITE_AND_EXIT); }
static void jrnl_bdl_rec_replay_write_b(void) { jrnl_bdl_recovery_check(true); }
TEST_CASE_MULTIPLE_STAGES("(jrnl-bdl) recovery: power-off mid-replay (write) -> replay completes",
                          "[jrnl][bdl][recovery]",
                          jrnl_bdl_rec_replay_write_a, jrnl_bdl_rec_replay_write_b);

/* e) interrupted after all sectors transferred but before the master is reset -> completed. */
static void jrnl_bdl_rec_replay_close_a(void) { jrnl_bdl_recovery_setup(ESP_JRNL_TEST_REPLAY_EXIT_BEFORE_CLOSE); }
static void jrnl_bdl_rec_replay_close_b(void) { jrnl_bdl_recovery_check(true); }
TEST_CASE_MULTIPLE_STAGES("(jrnl-bdl) recovery: power-off after transfer, before close -> replay completes",
                          "[jrnl][bdl][recovery]",
                          jrnl_bdl_rec_replay_close_a, jrnl_bdl_rec_replay_close_b);

#endif /* CONFIG_ESP_JRNL_ENABLE_TESTMODE */

/* ===================================================================== */
/*  Concurrency: per-instance transaction window semaphore               */
/*                                                                       */
/*  Several tasks share a single journaled BDL and each runs full        */
/*  START / write / COMMIT cycles against its OWN target sector. The     */
/*  window semaphore taken by ESP_JRNL_IOCTL_TRANS_START must serialize  */
/*  whole transaction windows: a second task's TRANS_START blocks until  */
/*  the holder commits. The test passes only if every iteration of every */
/*  task returns ESP_OK and every target ends up with that task's final  */
/*  pattern.                                                             */
/* ===================================================================== */

#define CONC_TASKS  3
#define CONC_ITERS  6

typedef struct {
    esp_blockdev_handle_t jrnl_bdl;
    uint64_t          target;       /* this task's exclusive target sector */
    uint8_t           base_fill;    /* per-task pattern base */
    SemaphoreHandle_t done;
    esp_err_t         result;       /* first non-OK error, ESP_OK if all good */
    uint8_t           final_fill;   /* pattern of the last committed iteration */
} conc_task_arg_t;

static void conc_task_fn(void *param)
{
    conc_task_arg_t *a = (conc_task_arg_t *)param;
    const size_t sec = a->jrnl_bdl->geometry.erase_size;
    esp_err_t err = ESP_OK;

    uint8_t *buf = malloc(sec);
    if (buf == NULL) {
        a->result = ESP_ERR_NO_MEM;
        xSemaphoreGive(a->done);
        vTaskDelete(NULL);
        return;
    }

    for (int i = 0; i < CONC_ITERS; ++i) {
        const uint8_t fill = (uint8_t)(a->base_fill + i);
        memset(buf, fill, sec);

        err = a->jrnl_bdl->ops->ioctl(a->jrnl_bdl, ESP_JRNL_IOCTL_TRANS_START, NULL);
        if (err != ESP_OK) {
            break;  /* owner lock failed to serialize -> START hit a non-READY store */
        }

        esp_err_t werr = a->jrnl_bdl->ops->write(a->jrnl_bdl, buf, a->target, sec);
        esp_err_t cerr = a->jrnl_bdl->ops->ioctl(a->jrnl_bdl,
                                                 werr == ESP_OK ? ESP_JRNL_IOCTL_TRANS_COMMIT
                                                                : ESP_JRNL_IOCTL_TRANS_CANCEL, NULL);
        err = (werr != ESP_OK) ? werr : cerr;
        if (err != ESP_OK) {
            break;
        }
        a->final_fill = fill;
    }

    free(buf);
    a->result = err;
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

TEST_CASE("(jrnl-bdl) concurrency: owner lock serializes parallel transactions", "[jrnl][bdl][concurrency]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;

    jrnl_bdl_stack_t stk;
    stack_build(&stk, &cfg);

    const size_t sec = stk.jrnl_bdl->geometry.erase_size;

    conc_task_arg_t args[CONC_TASKS] = {0};
    for (int t = 0; t < CONC_TASKS; ++t) {
        args[t].jrnl_bdl  = stk.jrnl_bdl;
        args[t].target    = (uint64_t)(13 + t * 4) * sec;   /* disjoint target sectors */
        args[t].base_fill = (uint8_t)(0x10 + t * 0x20);
        args[t].final_fill = 0;
        args[t].result    = ESP_OK;
        args[t].done      = xSemaphoreCreateBinary();
        TEST_ASSERT_NOT_NULL(args[t].done);
    }

    for (int t = 0; t < CONC_TASKS; ++t) {
        char name[16];
        snprintf(name, sizeof(name), "jrnl_conc_%d", t);
        BaseType_t ok = xTaskCreate(conc_task_fn, name, 8192, &args[t], 5, NULL);
        TEST_ASSERT_EQUAL(pdPASS, ok);
    }

    /* Wait for all workers (generous timeout - the lock serializes, never deadlocks). */
    for (int t = 0; t < CONC_TASKS; ++t) {
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(args[t].done, pdMS_TO_TICKS(30000)));
    }

    /* Every task must have completed all iterations cleanly... */
    for (int t = 0; t < CONC_TASKS; ++t) {
        TEST_ASSERT_EQUAL(ESP_OK, args[t].result);
    }

    /* ...and each disjoint target must hold that task's last committed pattern. */
    uint8_t *rb = malloc(sec);
    uint8_t *expected = malloc(sec);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_NOT_NULL(expected);
    for (int t = 0; t < CONC_TASKS; ++t) {
        memset(expected, args[t].final_fill, sec);
        TEST_ESP_OK(stk.jrnl_bdl->ops->read(stk.jrnl_bdl, rb, sec, args[t].target, sec));
        TEST_ASSERT_EQUAL_MEMORY(expected, rb, sec);
        vSemaphoreDelete(args[t].done);
    }
    free(rb);
    free(expected);

    stack_tear_down(&stk);
}

void app_main(void)
{
    unity_run_menu();
}
