/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_vfs_jrnl_fat.h"
#include "diskio_impl.h"
#include "wear_levelling.h"
#include "../diskio/diskio_jrnl.h"
#include "private_include/esp_vfs_jrnl_fat_private.h"
#include "vfs_fat_internal.h"

static const char* TAG = "vfs_jrnl_fat_spiflash";


esp_err_t esp_vfs_fat_spiflash_mount_jrnl(const char* base_path,
                                                const char* partition_label,
                                                const esp_vfs_fat_mount_config_t* mount_config,
                                                const esp_jrnl_config_t* jrnl_config,
                                                esp_jrnl_handle_t* jrnl_handle)
{
    //sanity check
    if (base_path == NULL || mount_config == NULL || jrnl_config == NULL || jrnl_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    //find required partition
    const esp_partition_t *jrnl_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, partition_label);
    if (jrnl_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition (type='data', subtype='fat', partition_label='%s'). Check the partition table.", partition_label);
        return ESP_ERR_NOT_FOUND;
    }

    //get available FATFS drive number for the partition
    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
        ESP_LOGD(TAG, "the maximum count of volumes is already mounted");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGD(TAG, "using pdrv=%i", pdrv);
    char drv[3] = {(char) ('0' + pdrv), ':', 0};

    //create journaling clockwork:
    esp_jrnl_handle_t jrnl_handle_temp = JRNL_INVALID_HANDLE;
    esp_err_t result = ESP_FAIL;
    wl_handle_t wl_handle = WL_INVALID_HANDLE;

    do {
        //1. install wear levelling
        result = wl_mount(jrnl_partition, &wl_handle);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "failed to mount wear levelling layer, error: 0x%08X", result);
            break;
        }

        ESP_LOGV(TAG, "WL partition size (wl_size, sector_count): %"PRIu32 ", %"PRIu32, (uint32_t)wl_size(wl_handle), (uint32_t)(wl_size(wl_handle) / wl_sector_size(wl_handle)));

        //2. mount journaling layer (ESP_JRNL_STATUS_FS_INIT)
        /*
        esp_jrnl_config_extended_t jrnl_config_ext = {
                .user_cfg = *jrnl_config,
                .fs_volume_id = pdrv,
                .volume_cfg = {
                        .volume_size = wl_size(wl_handle),
                        .disk_sector_size = wl_sector_size(wl_handle)
                },
                .diskio_cfg = {
                        .diskio_ctrl_handle = wl_handle,
                        .disk_read = &wl_read,
                        .disk_write = &wl_write,
                        .disk_erase_range = &wl_erase_range
                }
        };
         */
        esp_jrnl_config_extended_t jrnl_config_ext = {
                .user_cfg = *jrnl_config,
                .fs_volume_id = pdrv,
                .volume_cfg = ESP_JRNL_VOLUME_DEFAULT_CONFIG(wl_handle),
                .diskio_cfg = ESP_JRNL_DISKIO_DEFAULT_CONFIG(wl_handle)
        };

        result = esp_jrnl_mount(&jrnl_config_ext, &jrnl_handle_temp);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_jrnl_mount failed for pdrv=%i, error: 0x%08X)", pdrv, result);
            break;
        }

        //3. connect FATFS IO to the journaling component
        result = ff_diskio_register_jrnl(pdrv, jrnl_handle_temp);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "ff_diskio_register_jrnl failed for pdrv=%i, error: 0x%08X", pdrv, result);
            break;
        }

        //4. register FATFS partition
        FATFS *fs;
        esp_vfs_fat_conf_t conf = {
                .base_path = base_path,
                .fat_drive = drv,
                .max_files = mount_config->max_files,
        };
        result = vfs_fat_register_cfg_jrnl(&conf, &fs);
        //ESP_ERR_INVALID_STATE == already registered with VFS
        if (result != ESP_ERR_INVALID_STATE && result != ESP_OK) {
            ESP_LOGE(TAG, "vfs_fat_register failed for pdrv=%i, error: 0x%08X", pdrv, result);
            break;
        }

        //5. connect JRNL instance to FATFS volume
        result = vfs_fat_register_pdrv_jrnl_handle(pdrv, jrnl_handle_temp);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_vfs_fat_register_pdrv_jrnl_handle failed for pdrv=%i, error: 0x%08X", pdrv, result);
            break;
        }

        //6. mount the filesystem (format, if not yet done or required by config)
        bool need_mount_again = jrnl_config->force_fs_format;
        ESP_LOGD(TAG, "Mounting FatFS file-system (force_fs_format = %d)", need_mount_again);

        if (!need_mount_again) {
            FRESULT fres = f_mount(fs, drv, 1);
            ESP_LOGW(TAG, "f_mount result = %d", fres);
            if (fres != FR_OK) {
                need_mount_again =
                        (fres == FR_NO_FILESYSTEM || fres == FR_INT_ERR) && mount_config->format_if_mount_failed;
                if (!need_mount_again) {
                    ESP_LOGE(TAG, "f_mount failed (%d)", fres);
                    result = ESP_FAIL;
                    break;
                }
                else {
                    ESP_LOGD(TAG, "No file-system found");
                }
            }
        } else {
            ESP_LOGD(TAG, "Formatting FATFS partition forced by config");
        }

        if (need_mount_again) {
            const size_t workbuf_size = 4096;
            void *workbuf = ff_memalloc(workbuf_size);
            if (workbuf == NULL) {
                result = ESP_ERR_NO_MEM;
                break;
            }

            size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(CONFIG_WL_SECTOR_SIZE, mount_config->allocation_unit_size);
            ESP_LOGD(TAG, "Formatting FATFS partition (allocation unit size=%d)", alloc_unit_size);

            const MKFS_PARM opt = {(BYTE)(FM_ANY | FM_SFD), 0, 0, 0, alloc_unit_size};
            FRESULT fresult = f_mkfs(drv, &opt, workbuf, workbuf_size);

            free(workbuf);
            workbuf = NULL;

            if (fresult != FR_OK) {
                ESP_LOGE(TAG, "f_mkfs failed (%d)", fresult);
                result = ESP_FAIL;
                break;
            }

            ESP_LOGD(TAG, "Formatting done, mounting the volume");
            fresult = f_mount(fs, drv, 0);
            if (fresult != FR_OK) {
                ESP_LOGE(TAG, "f_mount after (re)format failed (%d)", fresult);
                result = ESP_FAIL;
                break;
            }
        }

        //set journal store as ready
        result = esp_jrnl_set_direct_io(jrnl_handle_temp, false);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "esp_jrnl_set_direct_io failed for pdrv=%i, error: 0x%08X", pdrv, result);
            break;
        }
    } while(0);

    if (result == ESP_OK) {
        *jrnl_handle = jrnl_handle_temp;
        ESP_LOGD(TAG, "Mount successful (pdrv=%i, jrnl_handle=%ld)", pdrv, *jrnl_handle);
    }
    else {
        esp_err_t err_temp = esp_vfs_fat_spiflash_unmount_jrnl(&jrnl_handle_temp, base_path);
        if( err_temp != ESP_OK) {
            ESP_LOGE(TAG, "esp_vfs_fat_spiflash_unmount_jrnl() failed with error 0x%08X)", err_temp);
        }
    }

    return result;
}

esp_err_t esp_vfs_fat_spiflash_unmount_jrnl(esp_jrnl_handle_t* jrnl_handle, const char* base_path)
{
    ESP_LOGD(TAG, "Unmounting JRNL");
    esp_err_t err;

    if (jrnl_handle == NULL || *jrnl_handle == JRNL_INVALID_HANDLE || base_path == NULL) {
        err = ESP_ERR_INVALID_ARG;
        goto unmount_exit;
    }

    //disconnect JRNL from FAT volume
    vfs_fat_unregister_pdrv_jrnl_handle(*jrnl_handle);

    BYTE pdrv = ff_diskio_get_pdrv_jrnl(*jrnl_handle);
    if (pdrv == 0xff) {
        err = ESP_ERR_INVALID_STATE;
        goto unmount_exit;
    }

    //disconnect FATFS DISKIO from JRNL
    ff_diskio_clear_pdrv_jrnl(*jrnl_handle);

    //unmount FATFS partition
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    f_mount(0, drv, 0);

    //remove diskio association with given FATFS volume
    ff_diskio_unregister(pdrv);

    wl_handle_t wl_handle;
    err = esp_jrnl_get_diskio_handle(*jrnl_handle, &wl_handle);
    if (err != ESP_OK) {
        goto unmount_exit;
    }

    //unmount JRNL instance
    err = esp_jrnl_unmount(*jrnl_handle);
    if (err != ESP_OK) {
        goto unmount_exit;
    }

    *jrnl_handle = JRNL_INVALID_HANDLE;

    //unmount WL component & unregister base_path
    esp_err_t err_drv = wl_unmount(wl_handle);
    err = vfs_fat_unregister_path_jrnl(base_path);
    if (err == ESP_OK) err = err_drv;

unmount_exit:
    ESP_LOGD(TAG, "Unmounting JRNL done with 0x%08X", err);
    return err;
}
