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

**esp_jrnl** component requires IDF 5.3 and higher

```sh
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

```txt
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
```

Basic **esp_jrnl** configuration is given the following structure:

```c
typedef struct {
    bool overwrite_existing;                /* create a new journaling store at any rate */
    bool replay_journal_after_mount;        /* true = apply unfinished-commit transaction if found during journal mount */
    bool force_fs_format;                   /* (re)format journaled file-system */
    size_t store_size_sectors;              /* journal store size in sectors (disk space deducted from WL partition end) */
} esp_jrnl_config_t;
```

Default configuration is provided by `ESP_JRNL_DEFAULT_CONFIG` macro:

```c
#define ESP_JRNL_DEFAULT_CONFIG() { \
    .overwrite_existing = false, \
    .replay_journal_after_mount = true, \
    .force_fs_format = false, \
    .store_size_sectors = 32 \
}
```

All the persistent journaling store info is available in the master record:

```c
typedef struct {
    uint32_t jrnl_magic_mark;               /* journaling store master record identification stamp */
    uint32_t format_version;                /* JRNL_STORE_FORMAT_VERSION (currently 2) */
    uint32_t store_size_sectors;            /* size of journaling store in sectors */
    uint64_t store_volume_offset_sector;    /* index of the first journaling store sector within the volume */
    uint32_t next_free_sector;              /* next free block. Default = 0 (relative offset in the store space) */
    esp_jrnl_trans_status_t status;         /* transaction status. Default = ESP_JRNL_STATUS_TRANS_READY */
    esp_jrnl_volume_t volume;               /* disk volume properties */
} esp_jrnl_master_t;
```

During the application runtime, each partition has 1 journal instance defined by a handle (index to journal instance table):

```c
typedef struct {
    _lock_t trans_lock;                     /* short master-record critical sections */
    SemaphoreHandle_t window_sem;           /* binary semaphore: whole transaction window */
    TaskHandle_t window_owner;              /* task that last successfully opened the window */
    bool implicit_transactions;             /* standalone write/erase auto START+COMMIT */
    uint8_t fs_volume_id;                   /* file-system volume ID (PDRV for FatFS) */
    esp_jrnl_diskio_t diskio;               /* disk device access configuration */
    esp_blockdev_handle_t bottom_bdl;       /* underlying BDL used for all raw I/O */
    bool owns_bottom_bdl;
    esp_jrnl_master_t master;               /* journal master record for given instance */
    #ifdef CONFIG_ESP_JRNL_ENABLE_TESTMODE
    uint32_t test_config;                   /* runtime flags for internal testing, 0x0 by default */
    #endif
} esp_jrnl_instance_t;
```

The component implements own VFS/FAT interface for intercepting the high level file system API calls like `fopen()` or `fwrite()`. Each such a call is enclosed in a journaling transaction through `esp_jrnl_start()` and `esp_jrnl_stop()`, so all the disk-write operations invoked during the transaction lifetime are stored together with the following metadata record:

```c
typedef struct {
    uint64_t target_sector;                 /* first target sector in the filesystem address space */
    uint32_t sector_count;                  /* number of sectors involved in current operation */
    uint32_t op_type;                       /* ESP_JRNL_OP_WRITE or ESP_JRNL_OP_ERASE */
    uint32_t crc32_data;                    /* sector data checksum (write); 0 for erase */
} esp_jrnl_oper_header_t;

typedef struct {
    esp_jrnl_oper_header_t header;
    uint32_t crc32_header;                  /* operation header checksum (contents of the struct instance) */
} esp_jrnl_operation_t;
```

Given the above-mentioned, the journaling store overhead can be described as

> **master_record + N * (chunk_header + M * chunk_data)**

where **N** is a number of operations in one transaction and **M** is variable amount of single operation data blocks.

## Block Device Layer (BDL) integration

In addition to the legacy callback-based `esp_jrnl_mount()` API, the component exposes a Block
Device Layer factory so that the journal can be plugged into any BDL stack:

```c
esp_err_t esp_jrnl_get_blockdev(esp_blockdev_handle_t bottom_bdl,
                                const esp_jrnl_config_t *cfg,
                                esp_blockdev_handle_t *out_bdl_handle);
```

The returned `out_bdl_handle` is a regular `esp_blockdev_t` device whose address space is the
bottom device's `disk_size` minus the journal store (the last `cfg->store_size_sectors` journal
records at the end of the bottom device). The journal store layout and the replay semantics are
identical to those used by the legacy API.

The journal records and replays data at one fixed internal unit derived from the bottom geometry:

- erase-before-write media (SPI NOR, the legacy callback-bridge): the unit is `erase_size`. The
  bottom `write_size` only has to be a non-zero divisor of `erase_size` (the usual NOR case of
  `write_size` 1/16 B with a 4 kB erase block is supported); otherwise `ESP_ERR_NOT_SUPPORTED`
  is returned.
- media without host-driven erase (SD/MMC, USB MSC, NAND+FTL): the unit is `write_size`.

The exposed geometry follows from that: `read_size` and `erase_size` are pass-throughs of
`bottom_bdl`, while `write_size` (and `recommended_write_size`) is the journal record unit — so
the layer above always writes at record granularity, which the journal can log and replay
atomically. On media without host erase this equals the bottom `write_size`, so it is a
transparent pass-through there. esp_jrnl is BDL middleware and otherwise trusts the bottom
device's reported granularities.

> Note: component 1.1.0 introduces on-disk format version 2 (`JRNL_STORE_FORMAT_VERSION`).
> The master record now carries an explicit `format_version`, fixed-width store geometry,
> a 64-bit `store_volume_offset_sector`, and operation headers use 64-bit `target_sector`
> plus an `op_type` (write or header-only erase). Unknown versions fail the mount sanity
> check. A live 1.0.x in-place upgrade with an in-flight `TRANS_COMMIT` is **not supported**
> — remount after a clean READY store, or format.

A typical stack looks like this (any of the lower BDL devices could be replaced by an SDMMC,
USB MSC or NAND-FTL BDL provider):

```c
esp_blockdev_handle_t part_bdl, wl_bdl, jrnl_bdl;
esp_partition_get_blockdev(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
                           "storage", &part_bdl);
wl_get_blockdev(part_bdl, &wl_bdl);

esp_jrnl_config_t cfg = ESP_JRNL_DEFAULT_CONFIG();
esp_jrnl_get_blockdev(wl_bdl, &cfg, &jrnl_bdl);

ff_diskio_register_bdl(pdrv, jrnl_bdl);
/* ... f_mkfs / f_mount on FatFS ... */
```

Ownership of `bottom_bdl` stays with the caller: `jrnl_bdl->ops->release(jrnl_bdl)` frees only
the journaled wrapper; the caller is responsible for releasing `wl_bdl` and `part_bdl`
afterwards.

### Initial state on return

The factory call returns with the journal in one of two well-defined states:

- `cfg.force_fs_format == false` (the default) — the journal is in `ESP_JRNL_STATUS_TRANS_READY`.
  The caller can issue `ESP_JRNL_IOCTL_TRANS_START` immediately. Any on-disk `TRANS_COMMIT`
  record left over from an interrupted commit is replayed before the call returns, and any
  on-disk `TRANS_OPEN` record is discarded — i.e. after each successful remount the journal
  is "clean" again, matching the legacy `esp_jrnl_mount()` recovery policy.
- `cfg.force_fs_format == true` — the journal is left in `ESP_JRNL_STATUS_FS_DIRECT`. This is
  the explicit opt-in for a one-shot direct (non-journaled) pass over the device, typically
  used to format an upper file-system. Once the direct pass is done, switch back to journaled
  mode with `ESP_JRNL_IOCTL_SET_DIRECT_IO(false)` (see below).

`FS_DIRECT` is therefore opt-in: once a file-system exists on top of the journaled BDL, the
default behaviour is to journal every disk-write — the engine intentionally never lingers in
`FS_DIRECT` after a remount.

### Transaction control through ioctl

Transactions are driven by `esp_blockdev_ops_t::ioctl` on the journaled BDL handle. The
commands live in the BDL system command space at `ESP_JRNL_IOCTL_BASE = ESP_BLOCKDEV_CMD_SYSTEM_BASE
+ 30` and are reserved with `ESP_BLOCKDEV_RESERVE_CMD_RANGE(esp_jrnl, ESP_JRNL_IOCTL_BASE,
ESP_JRNL_IOCTL_BASE + 4)` so the build-time overlap checker in `esp_blockdev.h` sees them.
The authoritative map of system, per-component and user command ranges is in `esp_blockdev.h`
— do not assume a NAND-specific hole in the system space.

The five commands cover both user-controlled scopes (application code wraps a sequence of
writes in a transaction) and IDF-internal scopes (e.g. a file-system auto-repair routine):

| Command | Args | Behaviour |
| --- | --- | --- |
| `ESP_JRNL_IOCTL_TRANS_START` | `NULL` | Begin a transaction. Fails with `ESP_ERR_INVALID_STATE` if one is already open. |
| `ESP_JRNL_IOCTL_TRANS_COMMIT` | `NULL` | Mark the journal `TRANS_COMMIT` and replay it to the bottom device. |
| `ESP_JRNL_IOCTL_TRANS_CANCEL` | `NULL` | Reset the journal store to `TRANS_READY` without touching the bottom data. |
| `ESP_JRNL_IOCTL_SET_DIRECT_IO` | `const bool *` | Toggle direct (non-journaled) access on the underlying device. Required for filesystem format / mount. |
| `ESP_JRNL_IOCTL_GET_STATUS` | `esp_jrnl_trans_status_t *` | Query the current transaction status. |

Coordination is cooperative: only one transaction can be open at a time. A second
`ESP_JRNL_IOCTL_TRANS_START` while a transaction is already open returns
`ESP_ERR_INVALID_STATE`. COMMIT/CANCEL may be issued from a different task than START
(the window is a binary semaphore). Nested START from the owning task returns
`ESP_ERR_INVALID_STATE` without taking the semaphore. Use `ESP_JRNL_IOCTL_GET_STATUS`
to probe the current state when callers from different layers may be racing.

By default (`esp_jrnl_config_t.implicit_transactions == true`) a standalone write or
host-driven erase while the store is `TRANS_READY` is wrapped in its own START + log +
COMMIT. Explicit ioctl still groups several operations into one atomic transaction.
Set the flag to `false` if READY-state I/O must be rejected.

System ioctls of the BDL interface (`ESP_BLOCKDEV_CMD_MARK_DELETED`,
`ESP_BLOCKDEV_CMD_ERASE_CONTENTS`) are forwarded to the bottom BDL when no transaction is
open (status is `TRANS_READY` or `FS_DIRECT`) and rejected with `ESP_ERR_INVALID_STATE` while
a transaction is open — they would otherwise bypass the journal.

#### Reads during an open transaction

Reads first fetch the requested range from the bottom device, then overlay any later
journaled WRITE/ERASE records that overlap that range (erase fills
`default_val_after_erase`). FatFS read-modify-write therefore sees its own uncommitted
directory updates. After `TRANS_CANCEL` the overlay disappears and subsequent reads see
the original bottom content again.

### Medium-agnostic erase

The journal core honours `bottom_bdl->device_flags.erase_before_write` to decide whether to
issue erase calls on the underlying device. On erase-before-write media, a standalone
`erase()` during `TRANS_OPEN` is logged as `ESP_JRNL_OP_ERASE` (header only) and replayed
at commit; `FS_DIRECT` writes erase then write. On other media, host erase is not logged
(FatFS TRIM is advisory). This makes the same engine usable on:

- SPI NOR flash (and the legacy wear-levelling backend), where `erase_before_write = 1` and
  the journal performs an erase before every write — identical to the historical behaviour.
- SD/MMC and USB MSC, where the host has no erase concept; writes are in-place and the
  `device_flags.erase_before_write = 0` setting tells the journal to skip its internal erase
  calls.
- NAND with an FTL (e.g. `spi_nand` exposed through BDL), where the FTL handles erase
  internally; `device_flags.erase_before_write = 0` again.

Raw NAND without an FTL is not recommended: the journal records are sized to the bottom
device's `erase_size`, so a typical raw-NAND erase block (>= 128 kB) makes each journaled
write very expensive on the journal store.

The legacy `esp_jrnl_mount(esp_jrnl_config_extended_t)` path internally builds a synthetic
BDL initialised with SPI Flash defaults (`erase_before_write = 1`, `and_type_write = 1`,
`default_val_after_erase = 1`), so existing code based on `esp_vfs_fat_spiflash_mount_jrnl()`
keeps the exact behaviour it had before.

### Release behaviour with an open transaction

`release()` on the journaled BDL — like `esp_jrnl_unmount()` for the legacy path — performs
a local cleanup only: it tears down the in-memory journal instance and does NOT touch the
on-disk master record. Any pending transaction is resolved on the next
`esp_jrnl_get_blockdev()` / `esp_jrnl_mount()` of the same store, following the standard
recovery policy:

- `TRANS_OPEN` on disk → discarded (the partial transaction is abandoned, target data is
  untouched).
- `TRANS_COMMIT` on disk → completed at any rate (the in-flight commit is replayed).

## Examples

See the component's repository `examples/basic` for the default use-case
