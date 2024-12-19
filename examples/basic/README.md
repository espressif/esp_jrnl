| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- |

# ESP_JRNL basic example

This example demonstrates how to deploy ESP-IDF FatFS journaling within common file-system mounting steps.
To build and run the example for instance on ESP32S3, use:

```bash
cd esp-idf
./install.sh
. ./export.sh
cd esp_jrnl/examples/basic
idf.py set-target esp32s3
idf.py flash monitor
```



