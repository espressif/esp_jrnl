/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_jrnl.h"

typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint32_t DWORD;

/**
 * @brief Register esp_fs_journal FatFS callbacks for the journaled partition
 *
 * FatFS file-system communicates with underlying disk (block) device using predefined interface,
 * see 'Media Access Interface' chapter in http://elm-chan.org/fsw/ff. 'esp_fs_journal' needs to intercept
 * these callbacks to deploy the journaling mechanism
 *
 * @param[in] pdrv          FatFS drive number
 * @param[in] jrnl_handle   handle of FS journaling instance for the partition given by 'pdrv'
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if 'pdrv' number exceeds maximum amount of FatFS volumes
 */
esp_err_t ff_diskio_register_jrnl(const unsigned char pdrv, const esp_jrnl_handle_t jrnl_handle);

/**
 * @brief Return FatFS drive number for given esp_fs_journal handle
 *
 * @param[in] jrnl_handle  FS journaling instance handle
 *
 * @return
 *      - 'pdrv' number on success
 *      - 0xFF if not found
 */
BYTE ff_diskio_get_pdrv_jrnl(const esp_jrnl_handle_t jrnl_handle);

/**
 * @brief Disconnects esp_fs_journal instance from FatFS drive (previsouly connected using ff_diskio_register_jrnl)
 *
 * @param[in] jrnl_handle  FS journaling instance handle
 */
void ff_diskio_clear_pdrv_jrnl(const esp_jrnl_handle_t jrnl_handle);

#ifdef __cplusplus
}
#endif
