| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- |

# ESP_JRNL basic tests (jrnl_basic)

Set of testing scenarios to verify essential functions of the 'esp_fs_journal' component APIs and internal procedures.
The tests are designed to cover all the API calls and internal structures processing, regardless of "privacy level" of the functions involved - 'esp_jrnl' API
set can be divided into few stack-like privacy levels, where the topmost functions provide the component's public interface (mount/dismount),
the mid-layers work as internal cross-module functions (FatFS disk I/Os, VFS calls) and the lowest floor is component's purely internal code.
Higher levels partially deploy lower levels function calls, thus, not each single API is necessarily called directly from the jrnl_basic test code.

Furthermore, these tests operate at sector level, without any knowledge of the data possibly contained on the underlying block device.
Testing procedures use own data strings and offsets, erase and rewrite various sectors and are generally careless about structures possibly existing
on the test disk before the test suite start.

FatFS/VFS aware test parts are implemented in the 'jrnl_vfs' application.

NOTE:
All the 'esp_jrnl' tests are chip-type agnostic, the only parameter required is default SPI Flash chip with minimum 2MB of available space (test app default flash size is 4MB).

To run the test all-in-one, use 'pytest' (see https://docs.espressif.com/projects/esp-idf/en/stable/esp32/contribute/esp-idf-tests-with-pytest.html).

