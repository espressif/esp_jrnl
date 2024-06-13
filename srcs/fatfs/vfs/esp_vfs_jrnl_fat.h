/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "esp_jrnl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
* @brief Convenience function to install esp_fs_journal instance, initialize FAT filesystem in SPI flash and register it in VFS
*
* This is an all-in-one function which does the following:
*
* - finds the partition with defined partition_label. Partition label should be
*   configured in the partition table.
* - initializes flash wear levelling library on top of the given partition
* - installs FS journal instance at the end of remaining partition space ("next" to thw WL sectors)
* - mounts FAT partition using FATFS library on top of esp_fs_journal instance
* - registers FATFS library with VFS, with prefix given by base_prefix variable, FatFS callbacks over-riden by esp_fs_journal
*
* Using this function i preferred way to use file-system journaling for FatFS/WL partition on SPI flash
*
* @param[in] base_path        path where FATFS partition should be mounted (e.g. "/spiflash")
* @param[in] partition_label  label of the partition which should be used
* @param[in] mount_config     pointer to structure with extra parameters for mounting FATFS
* @param[in] jrnl_config      pointer to structure with esp_fs_journal instance configuration
* @param[out] jrnl_handle     esp_fs_journal instance handle (needed for unmounting)
*
* @return
*      - ESP_OK                 on success
*      - ESP_ERR_INVALID_ARG    if any of the input parameters is NULL
*      - ESP_ERR_NOT_FOUND      if the partition table does not contain FATFS partition with given label
*      - ESP_ERR_INVALID_STATE  if esp_vfs_fat_spiflash_mount_rw_wl was already called
*      - ESP_ERR_NO_MEM         if FatFS required memory can not be allocated
*      - ESP_FAIL               if partition can not be mounted due to internal FatFS error
*      - other error codes from wear levelling library, SPI flash driver, FATFS drivers and esp_fs_journal component
*/
esp_err_t esp_vfs_fat_spiflash_mount_jrnl(const char* base_path,
                                          const char* partition_label,
                                          const esp_vfs_fat_mount_config_t* mount_config,
                                          const esp_jrnl_config_t* jrnl_config,
                                          esp_jrnl_handle_t* jrnl_handle);

/**
 * @brief Unmounts FAT filesystem from journaled partition and release resources acquired using esp_vfs_fat_spiflash_mount_jrnl
 *
 * @param[inout] jrnl_handle    esp_fs_journal instance handle returned by esp_vfs_fat_spiflash_mount_jrnl
 * @param[in] base_path         path where partition should be registered (e.g. "/spiflash")
 *
 * @return
 *      - ESP_OK                on success
 *      - ESP_ERR_INVALID_ARG   if any of the parameters is invalid
 *      - ESP_ERR_INVALID_STATE if jrnl_handle cannot be unregistered
 *      - other error codes from wl_unmount() or esp_vfs_fat_unregister_path_jrnl() API calls
 */
esp_err_t esp_vfs_fat_spiflash_unmount_jrnl(esp_jrnl_handle_t* jrnl_handle, const char* base_path);

#ifdef __cplusplus
}
#endif
