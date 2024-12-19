| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- |

# ESP_JRNL VFS/FAT tests (jrnl_vfs)

FatFS/VFS specific testing scenarios for 'esp_jrnl' component.
The test cases are designed to verify 'esp_jrnl' deployment for FatFS partition, ie testing of real file-system operations
being properly journaled and finally deployed to the target block-device.

NOTE:
All the 'esp_jrnl' tests are chip-type agnostic, the only parameter required is default SPI Flash chip with minimum 2MB of available space (test app default flash size is 4MB).

To run the test all-in-one, use 'pytest' (see https://docs.espressif.com/projects/esp-idf/en/stable/esp32/contribute/esp-idf-tests-with-pytest.html). For example, to run the tests on ESP32S3, do the following:

```
    cd esp-idf
    git pull
    git submodule update --init --recursive
    cd ../esp_jrnl
    ../esp-idf/install.sh --enable-pytest
    . ../esp-idf/export.sh
    cd test_apps/jrnl_vfs
    idf.py set-target esp32s3
    idf.py build
    pytest
```
