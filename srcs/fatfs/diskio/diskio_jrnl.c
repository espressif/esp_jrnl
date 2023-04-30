/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*-----------------------------------------------------------------------*/
/* Low level disk I/O module for FatFS journaling (FatFS (C)ChaN, 2016)  */
/* ESP-IDF port Copyright 2016 Espressif Systems (Shanghai) PTE LTD      */
/*-----------------------------------------------------------------------*/

#include "diskio_impl.h"

#include "esp_log.h"
#include "esp_jrnl.h"

static const char* TAG = "diskio_jrnl";

/* MV2DO: !!!
 * JRNL_MAX_HANDLES == MAX_WL_HANDLES (8)
 * but FF_VOLUMES (1-10) can be higher than MAX_WL_HANDLES
 */

esp_jrnl_handle_t ff_jrnl_handles[JRNL_MAX_HANDLES] = {
        [0 ... JRNL_MAX_HANDLES - 1] = JRNL_INVALID_HANDLE
};

DSTATUS ff_jrnl_initialize(BYTE pdrv)
{
    return 0;
}

DSTATUS ff_jrnl_status(BYTE pdrv)
{
    return 0;
}

/* On the command GET_SECTOR_COUNT, return modified partition size in sectors(WL sector count - JRNL sector count)
 * All other commands are processed by WL */
DRESULT ff_jrnl_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    ESP_LOGV(TAG, "ff_jrnl_ioctl: pdrv=%i, cmd=%i\n", pdrv, cmd);

    assert(pdrv < JRNL_MAX_HANDLES);
    esp_jrnl_handle_t jrnl_handle = ff_jrnl_handles[pdrv];

    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            {
                size_t ff_sector_count;
                esp_err_t err = esp_jrnl_get_sector_count(jrnl_handle, &ff_sector_count);
                if (unlikely(err != ESP_OK)) {
                    ESP_LOGE(TAG, "esp_jrnl_get_sector_count failed (0x%08X)", err);
                    return RES_ERROR;
                }

                ESP_LOGV(TAG, "ff_sector_count: %" PRIu16, ff_sector_count);
                *((DWORD*) buff) = ff_sector_count;
            }
            return RES_OK;
        case GET_SECTOR_SIZE:
            {
                size_t ff_sector_size;
                esp_err_t err = esp_jrnl_get_sector_size(jrnl_handle, &ff_sector_size);
                if (unlikely(err != ESP_OK)) {
                    ESP_LOGE(TAG, "esp_jrnl_get_sector_size failed (0x%08X)", err);
                    return RES_ERROR;
                }

                ESP_LOGV(TAG, "ff_sector_size: %" PRIu16, ff_sector_size);
                *((WORD*) buff) = ff_sector_size;
            }
            return RES_OK;
        case GET_BLOCK_SIZE:
            return RES_ERROR;
    }

    return RES_ERROR;
}

DRESULT ff_jrnl_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    ESP_LOGV(TAG, "ff_jrnl_write - pdrv=%i, sector=%i, count=%i\n", (unsigned int)pdrv, (unsigned int)sector, (unsigned int)count);

    assert(pdrv < JRNL_MAX_HANDLES);
    esp_jrnl_handle_t jrnl_handle = ff_jrnl_handles[pdrv];

    esp_err_t err = esp_jrnl_write(jrnl_handle, buff, sector, count);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "esp_jrnl_write failed (0x%08X)", err);
        return RES_ERROR;
    }

    return RES_OK;
}

DRESULT ff_jrnl_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    ESP_LOGV(TAG, "ff_jrnl_read - pdrv=%i, sector=%i, count=%i\n", (unsigned int)pdrv, (unsigned int)sector, (unsigned int)count);

    assert(pdrv < JRNL_MAX_HANDLES);
    esp_jrnl_handle_t jrnl_handle = ff_jrnl_handles[pdrv];

    esp_err_t err = esp_jrnl_read(jrnl_handle, sector, buff, count);
    if (unlikely(err != ESP_OK)) {
        ESP_LOGE(TAG, "esp_jrnl_read failed (0x%08X)", err);
        return RES_ERROR;
    }

    return RES_OK;
}

esp_err_t ff_diskio_register_jrnl(const BYTE pdrv, const esp_jrnl_handle_t jrnl_handle)
{
    //MV2DO: can be removed after resolving FF_VOLUMES x MAX_WL_HANDLES discrepancy
    assert(FF_VOLUMES <= JRNL_MAX_HANDLES);

    if (pdrv >= FF_VOLUMES) {
        return ESP_ERR_INVALID_ARG;
    }

    static const ff_diskio_impl_t jrnl_impl = {
        .init = &ff_jrnl_initialize,
        .status = &ff_jrnl_status,
        .read = &ff_jrnl_read,
        .write = &ff_jrnl_write,
        .ioctl = &ff_jrnl_ioctl
    };

    //register journaled operations into the FATFS
    ff_diskio_register(pdrv, &jrnl_impl);
    ff_jrnl_handles[pdrv] = jrnl_handle;

    return ESP_OK;
}

BYTE ff_diskio_get_pdrv_jrnl(const esp_jrnl_handle_t jrnl_handle)
{
    for (int i=0; i<FF_VOLUMES; i++) {
        if (jrnl_handle == ff_jrnl_handles[i]) {
            return i;
        }
    }
    return 0xff;
}

void ff_diskio_clear_pdrv_jrnl(const esp_jrnl_handle_t jrnl_handle)
{
    for (int i=0; i<FF_VOLUMES; i++) {
        if (jrnl_handle == ff_jrnl_handles[i]) {
            ff_jrnl_handles[i] = JRNL_INVALID_HANDLE;
        }
    }
}
