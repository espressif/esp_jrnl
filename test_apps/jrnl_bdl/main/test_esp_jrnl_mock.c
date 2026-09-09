/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Mock-BDL tests for the esp_jrnl Block Device Layer factory.
 *
 * The real-stack tests in test_esp_jrnl_bdl.c only ever exercise SPI-NOR + wear-levelling
 * (erase_before_write = 1, write_size 1/16 B, erase_size 4 KiB). These tests use a small,
 * RAM-backed synthetic BDL with *configurable* geometry and flags so the media-agnostic
 * paths can be validated deterministically on any target, in particular:
 *
 *   - non-erase media (erase_before_write = 0) where the journal record unit is write_size
 *     and write_size < erase_size (SD/eMMC/USB-MSC/NAND+FTL class);
 *   - the erase-before-write granularity rule (write_size must divide erase_size);
 *   - >4 GB devices, proving the core byte-addressing is 64-bit and never truncates.
 *
 * To model a device far larger than available RAM, the mock materializes a single contiguous
 * backing window [win_base, win_base + win_size). Accesses outside the window are accepted but
 * are no-ops for write/erase and return the erased pattern (0xFF) for reads. Tests are arranged
 * so every byte they actually touch (the journal store at the top of the device plus the chosen
 * target sectors) falls inside that window.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "unity.h"
#include "esp_log.h"
#include "esp_blockdev.h"
#include "esp_jrnl.h"

static const char *TAG = "test_jrnl_mock";

#define MOCK_ERASED_VAL     0xFF
#define U32_MAX_PLUS_1      0x100000000ULL   /* 4 GiB; addresses above this need 64-bit math */

/* ===================================================================== */
/*  RAM-backed mock BDL                                                   */
/* ===================================================================== */

typedef struct {
    esp_blockdev_t dev;         /* embedded device; handle == &mock->dev */
    uint8_t *win;               /* backing buffer for [win_base, win_base + win_size) */
    uint64_t win_base;
    size_t   win_size;
    /* last byte addresses seen by the bottom device, for truncation assertions */
    uint64_t last_write_addr;
    uint64_t last_read_addr;
    uint64_t last_erase_addr;
} mock_bdl_t;

static inline mock_bdl_t *MOCK(esp_blockdev_handle_t dev)
{
    return (mock_bdl_t *)dev->ctx;
}

static inline bool win_contains(const mock_bdl_t *m, uint64_t addr, size_t len)
{
    return m->win != NULL && addr >= m->win_base && (addr + len) <= (m->win_base + m->win_size);
}

static esp_err_t mock_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size,
                           uint64_t src_addr, size_t len)
{
    mock_bdl_t *m = MOCK(dev);
    if (dst == NULL || dst_size < len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src_addr + len > dev->geometry.disk_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    m->last_read_addr = src_addr;
    if (win_contains(m, src_addr, len)) {
        memcpy(dst, m->win + (src_addr - m->win_base), len);
    } else {
        memset(dst, MOCK_ERASED_VAL, len);
    }
    return ESP_OK;
}

static esp_err_t mock_write(esp_blockdev_handle_t dev, const uint8_t *src,
                            uint64_t dst_addr, size_t len)
{
    mock_bdl_t *m = MOCK(dev);
    if (src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dst_addr + len > dev->geometry.disk_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    m->last_write_addr = dst_addr;
    if (win_contains(m, dst_addr, len)) {
        memcpy(m->win + (dst_addr - m->win_base), src, len);
    }
    return ESP_OK;
}

static esp_err_t mock_erase(esp_blockdev_handle_t dev, uint64_t start, size_t len)
{
    mock_bdl_t *m = MOCK(dev);
    if (start + len > dev->geometry.disk_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    m->last_erase_addr = start;
    if (win_contains(m, start, len)) {
        memset(m->win + (start - m->win_base), MOCK_ERASED_VAL, len);
    }
    return ESP_OK;
}

static esp_err_t mock_release(esp_blockdev_handle_t dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mock_bdl_t *m = MOCK(dev);
    free(m->win);
    free(m);
    return ESP_OK;
}

static const esp_blockdev_ops_t s_mock_ops = {
    .read    = mock_read,
    .write   = mock_write,
    .erase   = mock_erase,
    .sync    = NULL,
    .ioctl   = NULL,
    .release = mock_release,
};

static esp_blockdev_handle_t mock_bdl_create(uint64_t disk_size, size_t read_size, size_t write_size,
                                             size_t erase_size, bool erase_before_write,
                                             uint64_t win_base, size_t win_size)
{
    mock_bdl_t *m = calloc(1, sizeof(mock_bdl_t));
    if (m == NULL) {
        return ESP_BLOCKDEV_HANDLE_INVALID;
    }
    if (win_size > 0) {
        m->win = malloc(win_size);
        if (m->win == NULL) {
            free(m);
            return ESP_BLOCKDEV_HANDLE_INVALID;
        }
        memset(m->win, MOCK_ERASED_VAL, win_size);
    }
    m->win_base = win_base;
    m->win_size = win_size;

    m->dev.ctx = m;
    m->dev.ops = &s_mock_ops;
    m->dev.device_flags.val = 0;
    m->dev.device_flags.erase_before_write   = erase_before_write ? 1 : 0;
    m->dev.device_flags.and_type_write       = erase_before_write ? 1 : 0;
    m->dev.device_flags.default_val_after_erase = 1;
    m->dev.geometry.disk_size              = disk_size;
    m->dev.geometry.read_size              = read_size;
    m->dev.geometry.write_size             = write_size;
    m->dev.geometry.erase_size             = erase_size;
    m->dev.geometry.recommended_read_size  = read_size;
    m->dev.geometry.recommended_write_size = write_size;
    m->dev.geometry.recommended_erase_size = erase_size;
    return &m->dev;
}

/* ===================================================================== */
/*  Test cases                                                           */
/* ===================================================================== */

/*
 * Non-erase media with write_size < erase_size (SD/eMMC class).
 *
 * Verifies the generalized geometry (write_size = bottom write_size, erase_size pass-through),
 * a sub-erase-block journaled write/commit round-trip, and TRANS_CANCEL discard.
 */
TEST_CASE("(jrnl-mock) non-erase media: write_size<erase_size round-trip", "[jrnl][bdl][mock]")
{
    const size_t WR = 512;
    const size_t ER = 4096;
    const uint64_t DISK = 64u * 1024u;   /* small enough to fully back in RAM */

    esp_blockdev_handle_t bottom = mock_bdl_create(DISK, WR, WR, ER, /*ebw=*/false,
                                                   /*win_base=*/0, /*win_size=*/(size_t)DISK);
    TEST_ASSERT_NOT_NULL(bottom);

    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;
    cfg.store_size_sectors = 32;

    esp_blockdev_handle_t jr = NULL;
    TEST_ESP_OK(esp_jrnl_get_blockdev(bottom, &cfg, &jr));
    TEST_ASSERT_NOT_NULL(jr);

    /* Geometry: record unit == bottom write_size on non-erase media; erase_size passes through. */
    TEST_ASSERT_EQUAL(WR, jr->geometry.write_size);
    TEST_ASSERT_EQUAL(WR, jr->geometry.read_size);
    TEST_ASSERT_EQUAL(ER, jr->geometry.erase_size);
    TEST_ASSERT_EQUAL((size_t)(DISK - (uint64_t)cfg.store_size_sectors * WR),
                      (size_t)jr->geometry.disk_size);

    esp_jrnl_trans_status_t status;
    TEST_ESP_OK(jr->ops->ioctl(jr, ESP_JRNL_IOCTL_GET_STATUS, &status));
    TEST_ASSERT_EQUAL(ESP_JRNL_STATUS_TRANS_READY, status);

    uint8_t *pat = malloc(WR);
    uint8_t *rb  = malloc(WR);
    uint8_t *before = malloc(WR);
    TEST_ASSERT_NOT_NULL(pat);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_NOT_NULL(before);
    memset(pat, 0xC3, WR);

    /* Several sub-erase-block (512 B) writes, committed in one transaction. */
    const uint64_t addrs[] = { 0, WR, 5u * WR, 9u * WR };
    TEST_ESP_OK(jr->ops->ioctl(jr, ESP_JRNL_IOCTL_TRANS_START, NULL));
    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); ++i) {
        TEST_ESP_OK(jr->ops->write(jr, pat, addrs[i], WR));
    }
    TEST_ESP_OK(jr->ops->ioctl(jr, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL));

    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); ++i) {
        memset(rb, 0, WR);
        TEST_ESP_OK(jr->ops->read(jr, rb, WR, addrs[i], WR));
        TEST_ASSERT_EQUAL_MEMORY(pat, rb, WR);
    }

    /* TRANS_CANCEL must leave the target untouched. */
    const uint64_t t = 7u * WR;
    TEST_ESP_OK(jr->ops->read(jr, before, WR, t, WR));
    memset(pat, 0x3C, WR);
    TEST_ESP_OK(jr->ops->ioctl(jr, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(jr->ops->write(jr, pat, t, WR));
    TEST_ESP_OK(jr->ops->ioctl(jr, ESP_JRNL_IOCTL_TRANS_CANCEL, NULL));
    TEST_ESP_OK(jr->ops->read(jr, rb, WR, t, WR));
    TEST_ASSERT_EQUAL_MEMORY(before, rb, WR);

    free(pat);
    free(rb);
    free(before);
    jr->ops->release(jr);
    TEST_ESP_OK(bottom->ops->release(bottom));
}

/*
 * Erase-before-write granularity rule.
 *
 * For erase-before-write media the journal records at erase-block granularity, so the bottom
 * write_size must be a non-zero divisor of erase_size. A pathological write_size (7 B vs a
 * 4 KiB erase block) must be rejected; a legitimate finer write_size (16 B, encrypted-NOR
 * class) must be accepted and the exposed write_size must equal the erase block.
 */
TEST_CASE("(jrnl-mock) erase media: write_size must divide erase_size", "[jrnl][bdl][mock]")
{
    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;
    cfg.store_size_sectors = 4;

    /* Negative: 4096 % 7 != 0 -> ESP_ERR_NOT_SUPPORTED. Rejection happens before any I/O,
     * so no backing window is needed. */
    esp_blockdev_handle_t bad = mock_bdl_create(256u * 1024u, 1, 7, 4096, /*ebw=*/true,
                                                /*win_base=*/0, /*win_size=*/0);
    TEST_ASSERT_NOT_NULL(bad);
    esp_blockdev_handle_t jr = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, esp_jrnl_get_blockdev(bad, &cfg, &jr));
    TEST_ASSERT_NULL(jr);
    TEST_ESP_OK(bad->ops->release(bad));

    /* Positive: 4096 % 16 == 0. Back a window covering the journal store at the top. */
    const uint64_t DISK = 256u * 1024u;
    const size_t   ER   = 4096;
    const size_t   STORE_BYTES = (size_t)cfg.store_size_sectors * ER;   /* 4 * 4096 = 16 KiB */
    esp_blockdev_handle_t good = mock_bdl_create(DISK, 1, 16, ER, /*ebw=*/true,
                                                 /*win_base=*/DISK - STORE_BYTES,
                                                 /*win_size=*/STORE_BYTES);
    TEST_ASSERT_NOT_NULL(good);
    jr = NULL;
    TEST_ESP_OK(esp_jrnl_get_blockdev(good, &cfg, &jr));
    TEST_ASSERT_NOT_NULL(jr);
    /* On erase-before-write media the record unit (and exposed write_size) is the erase block. */
    TEST_ASSERT_EQUAL(ER, jr->geometry.write_size);
    TEST_ASSERT_EQUAL(ER, jr->geometry.erase_size);

    jr->ops->release(jr);
    TEST_ESP_OK(good->ops->release(good));
}

/*
 * 64-bit addressing on a >4 GB device.
 *
 * Proves the core byte-addressing never truncates: disk_size stays above 4 GiB, the fresh
 * master record is written at a >4 GiB byte offset, and a journaled write/commit round-trip at
 * a >4 GiB target replays to the correct (>4 GiB) bottom address.
 */
TEST_CASE("(jrnl-mock) 64-bit: >4GB device, byte offsets not truncated", "[jrnl][bdl][mock]")
{
    const size_t WR = 512;
    const size_t ER = 4096;
    const uint64_t DISK = 8ULL * 1024 * 1024 * 1024;   /* 8 GiB */
    const size_t   WIN  = 64u * 1024u;                 /* back only the top 64 KiB */
    const uint64_t WIN_BASE = DISK - WIN;

    esp_blockdev_handle_t bottom = mock_bdl_create(DISK, WR, WR, ER, /*ebw=*/false,
                                                   WIN_BASE, WIN);
    TEST_ASSERT_NOT_NULL(bottom);
    mock_bdl_t *m = MOCK(bottom);

    esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
    cfg.overwrite_existing = true;
    cfg.store_size_sectors = 32;   /* 32 * 512 = 16 KiB store, well inside the 64 KiB window */

    esp_blockdev_handle_t jr = NULL;
    TEST_ESP_OK(esp_jrnl_get_blockdev(bottom, &cfg, &jr));
    TEST_ASSERT_NOT_NULL(jr);

    /* disk_size must remain a 64-bit value above 4 GiB (no size_t/uint32 truncation). */
    TEST_ASSERT_TRUE(jr->geometry.disk_size > U32_MAX_PLUS_1);
    const uint64_t expect_disk = DISK - (uint64_t)cfg.store_size_sectors * WR;
    TEST_ASSERT_TRUE(jr->geometry.disk_size == expect_disk);

    /* Creating the store wrote the master record at volume_size - unit, i.e. above 4 GiB.
     * The bottom device must have seen a 64-bit address. */
    TEST_ASSERT_TRUE(m->last_write_addr > U32_MAX_PLUS_1);

    /* Full journaled round-trip at a >4 GiB target that lives inside the backed window and
     * below the journal store. */
    const uint64_t target = WIN_BASE + 16u * WR;   /* 8 KiB into the window */
    TEST_ASSERT_TRUE(target > U32_MAX_PLUS_1);
    TEST_ASSERT_TRUE(target + WR <= jr->geometry.disk_size);

    uint8_t *pat = malloc(WR);
    uint8_t *rb  = malloc(WR);
    TEST_ASSERT_NOT_NULL(pat);
    TEST_ASSERT_NOT_NULL(rb);
    memset(pat, 0x9E, WR);

    TEST_ESP_OK(jr->ops->ioctl(jr, ESP_JRNL_IOCTL_TRANS_START, NULL));
    TEST_ESP_OK(jr->ops->write(jr, pat, target, WR));
    TEST_ESP_OK(jr->ops->ioctl(jr, ESP_JRNL_IOCTL_TRANS_COMMIT, NULL));

    /* The replayed data write must have reached a >4 GiB bottom address. */
    TEST_ASSERT_TRUE(m->last_write_addr > U32_MAX_PLUS_1);

    memset(rb, 0, WR);
    TEST_ESP_OK(jr->ops->read(jr, rb, WR, target, WR));
    TEST_ASSERT_EQUAL_MEMORY(pat, rb, WR);

    ESP_LOGI(TAG, "8GiB device: disk_size=%llu, last bottom write addr=%llu",
             (unsigned long long)jr->geometry.disk_size,
             (unsigned long long)m->last_write_addr);

    free(pat);
    free(rb);
    jr->ops->release(jr);
    TEST_ESP_OK(bottom->ops->release(bottom));
}
