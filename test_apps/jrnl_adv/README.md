| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- |

# ESP_JRNL advanced tests (jrnl_adv)

Advanced testing scenarios for 'esp_jrnl' component.
The test cases are designed to verify 'esp_jrnl' function over FatFS partition for real file-system operations
being properly journaled/deployed/committed/ignored during emulated unexpected power-off events.


NOTE:
All the 'esp_fs_journal' tests are chip-type agnostic, the only parameter required is default SPI Flash chip with minimum 2MB of available space (test app default flash size is 4MB)

