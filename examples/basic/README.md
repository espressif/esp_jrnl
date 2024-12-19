# ESP_JRNL basic example

This example demonstrates a default deployment of the ESP-IDF file system journaling (esp_jrnl) for FatFS partition on SPI Flash disk. The only steps needed are:
1. configuration of 'esp_jrnl' instance
2. mounting of the journaled FatFS via dedicated API  
   _(... common file system operations...)_  
3. unmount the file system

## How to use the example

There is no need for specific configuration or modifications, the example should run well out-of-box. However, if you would like to test the configuration options, have a look at the following structure:

```
typedef struct {
   bool overwrite_existing;                /* create a new journaling store at any rate */
   bool replay_journal_after_mount;        /* true = apply unfinished-commit transaction if found during journal mount */
   bool force_fs_format;                   /* (re)format journaled file-system */
   size_t store_size_sectors;              /* journal store size in sectors (disk space deducted from WL partition end) */
} esp_jrnl_config_t;
```
The only member recommended to change is the journaling store size.

## Build and Flash

The example runs on any ESP development board with at least 4 MB SPI Flash memory. For instance, to build and run the code on ESP32-S3, use:

```
idf.py set-target esp32s3
idf.py build flash monitor
```

NOTE: the environment of IDF 5.* must be set properly before running the example.

## Example Output

The example's output log should look as follows:

```
...
I (284) main_task: Calling app_main()
I (384) esp_jrnl_example_basic: Journaled FatFS mounted successfully.
I (384) esp_jrnl_example_basic: Opening file
I (1204) esp_jrnl_example_basic: File written
I (1214) esp_jrnl_example_basic: Renaming file
I (1494) esp_jrnl_example_basic: Reading file
I (1764) esp_jrnl_example_basic: Read from file: 'Hello World!'
I (1824) esp_jrnl_example_basic: Journaled FatFS unmounted.
I (1824) main_task: Returned from app_main()
```

## Documentation

See the esp_jrnl component's README.md file.

## Troubleshooting

In case you reconfigure the journaling store size using `esp_jrnl_config_t` and a journaling store of different size already exists on your SPI Flash, you will see the following error in our console output:

```
...
I (284) main_task: Calling app_main()
E (284) esp_jrnl: Journaling configuration inconsistent with found jrnl master record (record corrupted?)
E (284) esp_jrnl: esp_jrnl_mount failed (0x00000103)
E (294) vfs_jrnl_fat_spiflash: esp_jrnl_mount failed for pdrv=0, error: 0x00000103)
E (294) vfs_jrnl_fat_spiflash: esp_vfs_fat_spiflash_unmount_jrnl() failed with error 0x00000102)
E (304) esp_jrnl_example_basic: Failed to mount journaled FatFS file system.
I (314) main_task: Returned from app_main()
```

To resolve the issue, set the following configuration parameter for the first round after your change:
`jrnl_config.overwrite_existing = true;` right after `esp_jrnl_config_t jrnl_config = ESP_JRNL_DEFAULT_CONFIG();`. After successful application, set the `overwrite_existing` parameter to false or remove it completely. 

