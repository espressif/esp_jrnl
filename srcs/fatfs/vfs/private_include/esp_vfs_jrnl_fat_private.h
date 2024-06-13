/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "esp_vfs_fat.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
* @brief Registers FatFS volume number for given esp_fs_journal instance handle
* For internal use only.
*
* @param[in] pdrv               FatFS drive number
* @param[in] jrnl_handle        handle of FS journaling instance for given 'pdrv'
*
* @return
*      - ESP_OK                 on success
*      - ESP_ERR_INVALID_ARG    if 'pdrv' exceeds maximum FatFS volumes
*      - ESP_ERR_INVALID_STATE  if 'pdrv' is already assigned to other esp_fs_journal instance handle
*/
esp_err_t vfs_fat_register_pdrv_jrnl_handle(const uint8_t pdrv, const esp_jrnl_handle_t jrnl_handle);

/**
* @brief Unregisters FatFS volume number for given esp_fs_journal instance handle, previously registered with vfs_fat_register_pdrv_jrnl_handle().
* For internal use only.
*
* @param[in] jrnl_handle        handle of FS journaling instance handle to unregister
*
* @return
*      - ESP_OK                 on success
*      - ESP_ERR_INVALID_ARG    if 'jrnl_handle' is invalid
*      - ESP_ERR_NOT_FOUND      if 'jrnl_handle' is not found among registered handles
*/
esp_err_t vfs_fat_unregister_pdrv_jrnl_handle(const esp_jrnl_handle_t jrnl_handle);

/**
 * @brief Register FATFS with journaled VFS component
 *
 * This function works the same way as 'esp_vfs_fat_register_cfg' (IDF/VFS) except for
 * registering journaled variant of the VFS FAT APIs
 *
 * @param conf  pointer to esp_vfs_fat_conf_t configuration structure
 * @param[out] out_fs  pointer to FATFS structure which can be used for FATFS f_mount call is returned via this argument.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if esp_vfs_fat_register was already called
 *      - ESP_ERR_NO_MEM if not enough memory or too many VFSes already registered
 */
esp_err_t vfs_fat_register_cfg_jrnl(const esp_vfs_fat_conf_t* conf, FATFS** out_fs);

/**
 * @brief Unregister FATFS from journaled VFS
 *
 * @param base_path     path prefix where FATFS is registered. This is the same
 *                      used when 'vfs_fat_register_cfg_jrnl' was called
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if FATFS is not registered in VFS
 */
esp_err_t vfs_fat_unregister_path_jrnl(const char* base_path);

#ifdef __cplusplus
}
#endif
