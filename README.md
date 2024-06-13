# ESP file-system journaling component (esp_jrnl)

The *esp_jrnl* is ESP IDF component which provides transaction mechanism for file-system operations over
underlying disk device (journaling layer). The journaling significantly improves the file-system reliability during
sudden power-off events, ie it helps to avoid its corruption.

NOTE: Current version supports only the FatFS (wear-levelled) over SPI Flash media.

*esp_jrnl* implements a generic journaling for block devices in the following way:
* the journaling feature operates within one partition space of the target media/disk
* certain part of the partition space is required for the journaling store (block-aligned section at the partition end, used for the journal
  instance master record and transaction data). This space is not available for the journaled file-system. Its size is configurable (16 sectors by default)
* current version of _esp_jrnl_ can process only 1 transaction at time (no simultaneous starting/stopping of transactions on the same partition)
* once the journaling transaction is started, all subsequent disk-write operations are redirected to the journal store and kept there until the transaction is closed
* journaling transaction can be either committed or canceled. On cancel, no changes are done to the target file-system. On commit, the block operations
  saved in the journaling store are transferred in the same order to the target disk. The journaling store is reset to its initial state after in either case
* the journal transaction record is kept active until all the data is transferred, thus in case of sudden power-off all the transaction information remains valid is replayed on the next mounting attempt
* due to its extensive use of specific disk area, *esp_jrnl* is expected to run only on wear-levelled media
* journaling store overhead: **master_record + N * (chunk_header + M * chunk_data)**, where N is a number of operations in one transaction and M is variable amount
  of single operation data blocks

In order to compile the _esp_jrnl_ code, ESP IDF v5.0 and higher is required. The environment needs to be set to use this IDF branch, ideally with 'install.sh --enable-pytest',
to allow simple 'pytest' run of the test_apps. The test application are a good way to get familiar with the journaling implementation details.
Example setup of jrnl_basic tests for ESP32 platform:

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
