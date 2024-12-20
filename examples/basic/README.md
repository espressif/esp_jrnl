| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- |

# ESP_JRNL basic example

This example demonstrates a default deployment of the ESP-IDF file system journaling (esp_jrnl) for FatFS partition on SPI Flash disk. The only steps needed are:
1. configuration of 'esp_jrnl' instance
2. mounting of the journaled FatFS via dedicated API  
   _(... common file system operations...)_  
3. unmount the file system

## How to use the example

There is no need for specific configuration or modifications, the example should run well out-of-box. However, if you would like to test the configurationn options, have a look at the following structure:

```
typedef struct {
bool overwrite_existing;                /* create a new journaling store at any rate */
bool replay_journal_after_mount;        /* true = apply unfinished-commit transaction if found during journal mount */
bool force_fs_format;                   /* (re)format journaled file-system */
size_t store_size_sectors;              /* journal store size in sectors (disk space deducted from WL partition end) */
} esp_jrnl_config_t;
```
The only member recommended to change the journaling store size, which can be done in the following way:

```
...
esp_jrnl_config_t jrnl_config = ESP_JRNL_DEFAULT_CONFIG();
jrnl_config.store_size_sectors = 32;
...
```
(extend the default size from 16 to 32 sectors).

## Build and Flash

The esp_jrnl component is chip-type agnostic, so you can use any of ESP32 hardware for testing (as long as there is sufficient SPI Flash memory installed). To build and run the example for instance on ESP32S3, use:

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

There is no specific troubleshoooting for esp_jrnl, beside of the common techniques like debugging or enabling verbose logging.


