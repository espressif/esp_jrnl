/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_jrnl_fat.h"

static const char *TAG = "esp_jrnl_example_basic";
#define MOUNT_POINT "/spiflash"

void app_main(void)
{
    const char* basepath = MOUNT_POINT;
    const char* partlabel = "jrnl";
    esp_jrnl_handle_t jrnl_handle = JRNL_INVALID_HANDLE;
    esp_jrnl_config_t jrnl_config = ESP_JRNL_DEFAULT_CONFIG();

    esp_vfs_fat_mount_config_t mount_config = {
            .format_if_mount_failed = true,
            .max_files = 5
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_jrnl(basepath, partlabel, &mount_config, &jrnl_config, &jrnl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount journaled FatFS file system.");
        return;
    }
    ESP_LOGI(TAG, "Journaled FatFS mounted successfully.");

    //create file
    ESP_LOGI(TAG, "Opening file");
    FILE *f = fopen(MOUNT_POINT"/hello.txt", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return;
    }
    fprintf(f, "Hello World!\n");
    fclose(f);
    ESP_LOGI(TAG, "File written");

    // Check if destination file exists before renaming
    struct stat st;
    if (stat(MOUNT_POINT"/foo.txt", &st) == 0) {
        // Delete it if it exists
        unlink(MOUNT_POINT"/foo.txt");
    }

    // Rename original file
    ESP_LOGI(TAG, "Renaming file");
    if (rename(MOUNT_POINT"/hello.txt", MOUNT_POINT"/foo.txt") != 0) {
        ESP_LOGE(TAG, "Rename failed");
        return;
    }

    // Open renamed file for reading
    ESP_LOGI(TAG, "Reading file");
    f = fopen(MOUNT_POINT"/foo.txt", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return;
    }

    char line[128];
    fgets(line, sizeof(line), f);
    fclose(f);
    // strip newline
    char*pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line);

    // All done, unmount partition and disable LittleFS
    err = esp_vfs_fat_spiflash_unmount_jrnl(&jrnl_handle, basepath);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount journaled FatFS file system.");
        return;
    }

    ESP_LOGI(TAG, "Journaled FatFS unmounted.");
}
