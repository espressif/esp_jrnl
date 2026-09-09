/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Block-Device-Layer (BDL) integration for esp_jrnl
 *
 * This module provides:
 *  - The journaled BDL factory (esp_jrnl_get_blockdev) and its esp_blockdev_ops_t table,
 *    including the ioctl dispatcher for the ESP_JRNL_IOCTL_TRANS_* / SET_DIRECT_IO /
 *    GET_STATUS commands and the pass-through rules for system ioctls.
 *
 *  - The internal callback-bridge BDL (jrnl_legacy_bridge_bdl_create) used by the legacy
 *    esp_jrnl_mount() path so that the journaling core operates uniformly through BDL ops
 *    regardless of how the instance was created.
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "esp_log.h"
#include "esp_blockdev.h"
#include "esp_jrnl_internal.h"

#ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE
#include "esp_system.h"   /* esp_restart() - power-off emulation for the BDL commit path */
#endif

static const char *TAG = "esp_jrnl_bdl";

/* ------------------------------------------------------------------ */
/*  Legacy callback-bridge BDL                                        */
/*                                                                    */
/*  Wraps the three esp_jrnl_diskio_t callbacks in a minimal BDL      */
/*  device so the core journal engine can use the same BDL-based      */
/*  raw I/O path for both legacy and BDL-factory creation flows.      */
/*  Initialized with SPI Flash defaults (erase_before_write=1,        */
/*  and_type_write=1, default_val_after_erase=1) so that historical   */
/*  behavior of esp_vfs_fat_spiflash_mount_jrnl is preserved exactly. */
/* ------------------------------------------------------------------ */

/* dev->ctx holds a heap-allocated esp_jrnl_diskio_t directly (no single-member wrapper). */
static inline const esp_jrnl_diskio_t *jrnl_bridge_diskio(esp_blockdev_handle_t dev)
{
    return (const esp_jrnl_diskio_t *)dev->ctx;
}

static esp_err_t jrnl_bridge_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size,
                                  uint64_t src_addr, size_t len)
{
    if (dst_size < len) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_jrnl_diskio_t *diskio = jrnl_bridge_diskio(dev);
    if (src_addr > (uint64_t)SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return diskio->disk_read(diskio->diskio_ctrl_handle, (size_t)src_addr, dst, len);
}

static esp_err_t jrnl_bridge_write(esp_blockdev_handle_t dev, const uint8_t *src,
                                   uint64_t dst_addr, size_t len)
{
    const esp_jrnl_diskio_t *diskio = jrnl_bridge_diskio(dev);
    if (dst_addr > (uint64_t)SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return diskio->disk_write(diskio->diskio_ctrl_handle, (size_t)dst_addr, src, len);
}

static esp_err_t jrnl_bridge_erase(esp_blockdev_handle_t dev, uint64_t start, size_t len)
{
    const esp_jrnl_diskio_t *diskio = jrnl_bridge_diskio(dev);
    if (start > (uint64_t)SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    return diskio->disk_erase_range(diskio->diskio_ctrl_handle, (size_t)start, len);
}

static esp_err_t jrnl_bridge_release(esp_blockdev_handle_t dev)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    free(dev->ctx);
    free(dev);
    return ESP_OK;
}

static const esp_blockdev_ops_t s_jrnl_bridge_ops = {
    .read    = jrnl_bridge_read,
    .write   = jrnl_bridge_write,
    .erase   = jrnl_bridge_erase,
    .sync    = NULL,
    .ioctl   = NULL,
    .release = jrnl_bridge_release,
};

esp_err_t jrnl_legacy_bridge_bdl_create(const esp_jrnl_diskio_t *diskio,
                                        uint64_t volume_size,
                                        size_t sector_size,
                                        esp_blockdev_handle_t *out_bdl)
{
    if (diskio == NULL || out_bdl == NULL ||
        diskio->disk_read == NULL || diskio->disk_write == NULL || diskio->disk_erase_range == NULL ||
        volume_size == 0 || sector_size == 0 || (volume_size % sector_size) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_jrnl_diskio_t *diskio_ctx = (esp_jrnl_diskio_t *)calloc(1, sizeof(esp_jrnl_diskio_t));
    if (diskio_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *diskio_ctx = *diskio;

    esp_blockdev_t *dev = (esp_blockdev_t *)calloc(1, sizeof(esp_blockdev_t));
    if (dev == NULL) {
        free(diskio_ctx);
        return ESP_ERR_NO_MEM;
    }

    dev->ctx = diskio_ctx;
    dev->ops = &s_jrnl_bridge_ops;
    ESP_BLOCKDEV_FLAGS_INST_CONFIG_DEFAULT(dev->device_flags);
    dev->geometry.disk_size              = (uint64_t)volume_size;
    dev->geometry.read_size              = sector_size;
    dev->geometry.write_size             = sector_size;
    dev->geometry.erase_size             = sector_size;
    dev->geometry.recommended_read_size  = sector_size;
    dev->geometry.recommended_write_size = sector_size;
    dev->geometry.recommended_erase_size = sector_size;

    *out_bdl = dev;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Journaled BDL (esp_jrnl_get_blockdev)                             */
/*                                                                    */
/*  ctx is the esp_jrnl_instance_t*. Ops translate BDL byte addresses */
/*  into journaled sector operations on the bottom device.            */
/* ------------------------------------------------------------------ */

static inline esp_jrnl_instance_t *jrnl_bdl_inst(esp_blockdev_handle_t dev)
{
    return (esp_jrnl_instance_t *)dev->ctx;
}

static esp_err_t jrnl_bdl_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size,
                               uint64_t src_addr, size_t len)
{
    if (dst == NULL || dst_size < len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->geometry.read_size == 0 ||
        src_addr % dev->geometry.read_size != 0 || len % dev->geometry.read_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (src_addr + len > dev->geometry.disk_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_jrnl_instance_t *inst = jrnl_bdl_inst(dev);
    return jrnl_log_read(inst, dst, src_addr, len);
}

static esp_err_t jrnl_bdl_write(esp_blockdev_handle_t dev, const uint8_t *src,
                                uint64_t dst_addr, size_t len)
{
    if (src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->device_flags.read_only) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (dev->geometry.write_size == 0 ||
        dst_addr % dev->geometry.write_size != 0 || len % dev->geometry.write_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (dst_addr + len > dev->geometry.disk_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_jrnl_instance_t *inst = jrnl_bdl_inst(dev);
    const size_t sector_size = inst->master.volume.disk_sector_size;
    const uint64_t target_sector = dst_addr / sector_size;
    const uint32_t sector_count  = (uint32_t)(len / sector_size);

    if (inst->master.status == ESP_JRNL_STATUS_FS_DIRECT) {
        if (inst->bottom_bdl->device_flags.erase_before_write) {
            esp_err_t err = inst->bottom_bdl->ops->erase(inst->bottom_bdl, dst_addr, len);
            if (err != ESP_OK) {
                return err;
            }
        }
        return inst->bottom_bdl->ops->write(inst->bottom_bdl, src, dst_addr, len);
    }

    return jrnl_inst_log_or_implicit(inst, src, target_sector, sector_count);
}

static esp_err_t jrnl_bdl_erase(esp_blockdev_handle_t dev, uint64_t start, size_t len)
{
    if (dev->device_flags.read_only) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (dev->geometry.erase_size == 0 ||
        start % dev->geometry.erase_size != 0 || len % dev->geometry.erase_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (start + len > dev->geometry.disk_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_jrnl_instance_t *inst = jrnl_bdl_inst(dev);

    /* Host-driven erase is only meaningful on erase-before-write media. FatFS TRIM on
     * other backends is advisory: do not log it, and forward only when no transaction is
     * open (the bottom device may no-op). */
    if (!inst->bottom_bdl->device_flags.erase_before_write) {
        if (inst->master.status == ESP_JRNL_STATUS_TRANS_OPEN) {
            return ESP_OK;
        }
        return inst->bottom_bdl->ops->erase(inst->bottom_bdl, start, len);
    }

    if (inst->master.status == ESP_JRNL_STATUS_FS_DIRECT) {
        return inst->bottom_bdl->ops->erase(inst->bottom_bdl, start, len);
    }

    const size_t sector_size = inst->master.volume.disk_sector_size;
    if (sector_size == 0 || start % sector_size != 0 || len % sector_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return jrnl_inst_log_or_implicit(inst, NULL, start / sector_size, (uint32_t)(len / sector_size));
}

static esp_err_t jrnl_bdl_sync(esp_blockdev_handle_t dev)
{
    esp_jrnl_instance_t *inst = jrnl_bdl_inst(dev);
    if (inst->bottom_bdl->ops->sync == NULL) {
        return ESP_OK;
    }
    return inst->bottom_bdl->ops->sync(inst->bottom_bdl);
}

static esp_err_t jrnl_bdl_ioctl(esp_blockdev_handle_t dev, const uint8_t cmd, void *args)
{
    esp_jrnl_instance_t *inst = jrnl_bdl_inst(dev);

    switch (cmd) {
    case ESP_JRNL_IOCTL_TRANS_START:
        return jrnl_inst_trans_start(inst);

    case ESP_JRNL_IOCTL_TRANS_COMMIT:
        return jrnl_inst_trans_stop(inst, true);

    case ESP_JRNL_IOCTL_TRANS_CANCEL:
        return jrnl_inst_trans_stop(inst, false);

    case ESP_JRNL_IOCTL_SET_DIRECT_IO: {
        if (args == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        return jrnl_inst_set_direct_io(inst, *(const bool *)args);
    }

    case ESP_JRNL_IOCTL_GET_STATUS:
        if (args == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        *((esp_jrnl_trans_status_t *)args) = inst->master.status;
        return ESP_OK;

    default:
        break;
    }

    /* System ioctls (and any other user-range commands) are forwarded to the bottom BDL when
     * it is safe to do so. We block forwarding while a transaction is open because system
     * commands like MARK_DELETED / ERASE_CONTENTS would touch the address space behind the
     * journal's back. */
    if (inst->bottom_bdl->ops->ioctl == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (inst->master.status == ESP_JRNL_STATUS_TRANS_OPEN) {
        ESP_LOGE(TAG, "ioctl 0x%02x rejected: a transaction is open", (unsigned)cmd);
        return ESP_ERR_INVALID_STATE;
    }
    return inst->bottom_bdl->ops->ioctl(inst->bottom_bdl, cmd, args);
}

static esp_err_t jrnl_bdl_release(esp_blockdev_handle_t dev)
{
    if (dev == NULL || dev->ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_jrnl_instance_t *inst = (esp_jrnl_instance_t *)dev->ctx;

    /* Per design: release is local-only. The on-disk master record is left untouched and any
     * pending transaction is resolved by jrnl_replay() on the next mount (OPEN -> discarded,
     * COMMIT -> completed). bottom_bdl ownership stays with the caller. */
    inst->owns_bottom_bdl = false;
    jrnl_instance_destroy(inst);
    dev->ctx = NULL;
    free(dev);
    return ESP_OK;
}

static const esp_blockdev_ops_t s_jrnl_bdl_ops = {
    .read    = jrnl_bdl_read,
    .write   = jrnl_bdl_write,
    .erase   = jrnl_bdl_erase,
    .sync    = jrnl_bdl_sync,
    .ioctl   = jrnl_bdl_ioctl,
    .release = jrnl_bdl_release,
};

/* ------------------------------------------------------------------ */
/*  Public BDL factory                                                */
/* ------------------------------------------------------------------ */

esp_err_t esp_jrnl_get_blockdev(esp_blockdev_handle_t bottom_bdl,
                                const esp_jrnl_config_t *cfg,
                                esp_blockdev_handle_t *out_bdl_handle)
{
    if (bottom_bdl == ESP_BLOCKDEV_HANDLE_INVALID || cfg == NULL || out_bdl_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bottom_bdl->ops == NULL ||
        bottom_bdl->ops->read == NULL ||
        bottom_bdl->ops->write == NULL ||
        bottom_bdl->ops->erase == NULL) {
        ESP_LOGE(TAG, "%s: bottom_bdl missing required read/write/erase ops", __func__);
        return ESP_ERR_INVALID_ARG;
    }

    *out_bdl_handle = ESP_BLOCKDEV_HANDLE_INVALID;

    /* Allocate the journaled-BDL device wrapper up-front so failure paths have a clear
     * cleanup tree. dev->ctx holds the journal instance pointer directly. */
    esp_blockdev_t *dev = (esp_blockdev_t *)calloc(1, sizeof(esp_blockdev_t));
    if (dev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Create the journal instance. The caller owns bottom_bdl (owns_bdl=false), so the
     * instance will NOT release it on destroy. The diskio/fs_volume_id fields are not
     * populated for the BDL path. */
    esp_jrnl_instance_t *inst = NULL;
    esp_err_t err = jrnl_instance_create(cfg, bottom_bdl, false, NULL, 0, &inst);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }

    /* Default end-state of the factory is ESP_JRNL_STATUS_TRANS_READY: "once a file-system
     * exists, the natural behaviour is to journal every disk-write." A freshly initialised
     * journal store (overwrite_existing or fresh disk) is created in FS_DIRECT by
     * jrnl_instance_create; promote it to TRANS_READY here so the very first
     * ioctl(TRANS_START) is accepted without an intermediate SET_DIRECT_IO(false) round trip.
     *
     * The only opt-out is cfg->force_fs_format == true, which signals that the caller is
     * about to format an upper file-system (or otherwise needs un-cached writes) and will
     * issue the SET_DIRECT_IO(false) ioctl itself once the format pass is done. */
    if (!cfg->force_fs_format && inst->master.status == ESP_JRNL_STATUS_FS_DIRECT) {
        err = jrnl_inst_set_direct_io(inst, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: failed to promote fresh journal store to TRANS_READY (0x%08X)",
                     __func__, err);
            jrnl_instance_destroy(inst);
            free(dev);
            return err;
        }
    }

    dev->ctx = inst;
    dev->ops = &s_jrnl_bdl_ops;

    /* The journal store occupies the last store_size_sectors records of the bottom device.
     * The record unit was chosen by jrnl_instance_create() from the bottom geometry
     * (erase_size for erase-before-write media, write_size otherwise) and is recorded in
     * master.volume.disk_sector_size; reuse it so the exposed capacity matches the instance's
     * own view of the store boundary exactly. */
    const size_t jrnl_unit = inst->master.volume.disk_sector_size;
    const uint64_t store_bytes = (uint64_t)cfg->store_size_sectors * jrnl_unit;

    /* Propagate flags from the bottom device. The journal does not change medium semantics:
     * encryption, NAND-vs-NOR, and read-only properties are inherited verbatim. */
    dev->device_flags = bottom_bdl->device_flags;

    /* Geometry. read_size and erase_size are pure pass-throughs of the bottom device (esp_jrnl
     * trusts the bottom's reported granularities). write_size, however, is the journal RECORD
     * unit, not the bottom write_size: the journal logs and replays whole records, so the layer
     * above can only write at record granularity. On erase-before-write media that unit is the
     * erase block (e.g. 4 kB over NOR, where the bottom write_size is just 1/16 B); on media
     * without host erase it equals the bottom write_size, so this is a transparent pass-through
     * there. disk_size is shrunk by the journal store; the FS sits in [0, disk_size). */
    dev->geometry.disk_size              = bottom_bdl->geometry.disk_size - store_bytes;
    dev->geometry.read_size              = bottom_bdl->geometry.read_size;
    dev->geometry.write_size             = jrnl_unit;
    dev->geometry.erase_size             = bottom_bdl->geometry.erase_size;
    dev->geometry.recommended_read_size  = bottom_bdl->geometry.recommended_read_size;
    dev->geometry.recommended_write_size = jrnl_unit;
    dev->geometry.recommended_erase_size = bottom_bdl->geometry.recommended_erase_size;

    *out_bdl_handle = dev;
    ESP_LOGD(TAG, "%s: journaled BDL ready (disk_size=%llu, record_unit=%u, store_sectors=%u)",
             __func__, (unsigned long long)dev->geometry.disk_size,
             (unsigned)jrnl_unit, (unsigned)cfg->store_size_sectors);
    return ESP_OK;
}
