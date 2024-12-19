# ESP file-system journaling component (esp_jrnl)

The **esp_jrnl** is ESP-IDF component which provides a transaction mechanism (journaling layer) for disk-write operations generally in any file system, over its underlying block device. The journaling significantly improves the file system reliability during sudden power-off events and thus helps to avoid the system corruption. However, 100% power-off resilience cannot be guaranteed at any rate.

NOTE: **esp_jrnl** is chip-type agnostic. Current implementation supports only the FatFS file system on SPI Flash partitions.

## Overview

The journaling feature operates within a space of one partition on the target media/disk. It requires to occupy certain part of the partition space for its internal operations (so called "journaling store"), which is a sector-aligned section at the partition's end. The journaling store size is configurable and is set to 32 sectors by default. The store is used for holding the journaling transaction data and metadata. Consequently, the space actually available for the file system is the capacity required by the partition table minus the journaling store size, and minus the wear levelling cache in case of FatFS/SPI_Flash implementation.

The component works transparently from user's perspective, the only required steps are configuration and mounting/unmounting using dedicated APIs:

`esp_vfs_fat_spiflash_mount_jrnl()` and `esp_vfs_fat_spiflash_unmount_jrnl()`.


Currently, **esp_jrnl** can process only 1 transaction at time, each transaction scope is limited to one file-system API call. Once journaling transaction is started, all the subsequent disk-write operations are redirected to the journaling store and kept there until the transaction is closed. The transaction can be either committed or canceled. On cancel, no changes are done to the target file system and the store is reset to Ready state. On commit, the data blocks saved in the journaling store are transferred to the target disk in the same order as originally appeared (journaling store is "replayed"), and the store is finally marked clean. If the committing process fails at any point - for instance due to power-off, the data remains unchanged for the next attempts to replay the store. By default, the next try happens automatically on the nearest **esp_jrnl** mount, but this behaviour is configurable. It is strongly recommended to use the defaults except for testing purposes.

The above-mentioned process minimises corruption-prone period of the file-system operations. Still, if the power-off appears at unfortunate time during the data block transfer the system can get corrupted.

## Requirements and installation

**esp_jrnl** component requires IDF 5.0 and higher

```
    cd esp-idf
    git pull
    git submodule update --init --recursive
    cd ../esp_jrnl
    ../esp-idf/install.sh --enable-pytest
    . ../esp-idf/export.sh
    cd test_apps/jrnl_basic
    pytest
```   

## Implementation details

Due to its extensive use of specific disk area, **esp_jrnl** should be installed only on wear-levelled media. The FatFS version is primarily targeted on SPI FLash and automatically deploys the IDF wear-levelling component.

Basic journaling unit is a sector of the same size as given by related file system - the data are stored per blocks of this size. Also, the metadata records occupy one sector each. The main information pack is called 'master record' and it is always the last sector in the store, to allow straightforward readout of the **esp_jrnl** store details for given partition. The rest of the store (data blocks + metadata) are indexed from the store's sector 0:

    |--------------------------------------------------------------------||------------|---||------|
    |                                                                    ||            | M ||      |
    |                                                                    ||            | A ||      |
    |                                                                    ||            | S ||      |
    |                                                                    ||            | T ||      |
    |                            FILE SYSTEM                             ||   DATA     | E ||  WL  |
    |                                                                    ||     +      | R ||      |
    |                                                                    || METADATA   |   ||      |
    |                                                                    ||            | R ||      |
    |                                                                    ||            | E ||      |
    |                                                                    ||            | C ||      |
    |                                                                    ||            |   ||      |
    |                                                                    ||------------|---||      |
    |                                                                    ||   JOURNALING   ||      |
    |                                                                    ||     STORE      ||      |
    |--------------------------------------------------------------------||----------------||------|
    |<--------  - - -                  - - - PARTITION SPACE - - -                 - - -  -------->|
    |----------------------------------------------------------------------------------------------|

Basic **esp_jrnl** configuration is given the following structure:

```
typedef struct {
bool overwrite_existing;                /* create a new journaling store at any rate */
bool replay_journal_after_mount;        /* true = apply unfinished-commit transaction if found during journal mount */
bool force_fs_format;                   /* (re)format journaled file-system */
size_t store_size_sectors;              /* journal store size in sectors (disk space deducted from WL partition end) */
} esp_jrnl_config_t;
```

Default configuration is provided by `ESP_JRNL_DEFAULT_CONFIG` macro:

```
#define ESP_JRNL_DEFAULT_CONFIG() {
.overwrite_existing = false,
.replay_journal_after_mount = true,
.force_fs_format = false,
.store_size_sectors = 32
}
```

All the persistent journaling store info is available in the master record:

```
typedef struct {
uint32_t jrnl_magic_mark;               /* journaling store master record identification stamp */
size_t store_size_sectors;              /* size of journaling store in sectors */
size_t store_volume_offset_sector;      /* index of the first journaling store sector within the volume */
uint32_t next_free_sector;              /* next free block. Default = 0 (relative offset in the store space) */
esp_jrnl_trans_status_t status;         /* transaction status. Default = ESP_JRNL_STATUS_TRANS_READY */
esp_jrnl_volume_t volume;               /* disk volume properties */
} esp_jrnl_master_t;
```

During the application runtime, each partition has 1 journal instance defined by a handle (index to journal instance table):

```
typedef struct {
_lock_t trans_lock;
uint8_t fs_volume_id;                   /* file-system volume ID (PDRV for FatFS) */
esp_jrnl_diskio_t diskio;               /* disk device access configuration */
esp_jrnl_master_t master;               /* journal master record for given instance */
#ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE
uint32_t test_config;                   /* runtime flags for internal testing, 0x0 by default */
#endif
} esp_jrnl_instance_t;
```

The component implements own VFS/FAT interface for intercepting the high level file system API calls like `fopen()` or `fwrite()`. Each such a call is enclosed in a journaling transaction through `esp_jrnl_start()` and `esp_jrnl_stop()`, so all the disk-write operations invoked during the transaction lifetime are stored together with the following metadata record:

```
typedef struct {
uint32_t target_sector;                 /* target sector number in the filesystem (first sector of the sequence) */
size_t sector_count;                    /* number of sectors involved in current operation */
uint32_t crc32_data;                    /* sector data checksum (all sectors in the sequence) */
} esp_jrnl_oper_header_t;

typedef struct {
esp_jrnl_oper_header_t header;
uint32_t crc32_header;                  /* operation header checksum (contents of the struct instance) */
} esp_jrnl_operation_t;
```
Given the above-mentioned, the journaling store overhead can be described as

**master_record + N * (chunk_header + M * chunk_data)**

where **N** is a number of operations in one transaction and **M** is variable amount of single operation data blocks.

## Examples

See the component's repository `examples/basic` for the default use-case
