set(srcs "srcs/esp_jrnl.c"
         "srcs/esp_jrnl_blockdev.c"
         "srcs/fatfs/vfs/vfs_jrnl_fat.c"
         "srcs/fatfs/vfs/vfs_jrnl_fat_spiflash.c"
         "srcs/fatfs/diskio/diskio_jrnl.c")

# esp_blockdev is part of the public esp_jrnl.h interface (via the new esp_jrnl_get_blockdev
# factory and the ESP_JRNL_IOCTL_* command macros), so it must be a public REQUIRES.
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include srcs/fatfs/diskio srcs/fatfs/vfs
                       PRIV_INCLUDE_DIRS private_include
                       REQUIRES esp_blockdev wear_levelling fatfs vfs
                       PRIV_REQUIRES freertos)
