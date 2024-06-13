set(srcs "srcs/esp_jrnl.c"
         "srcs/fatfs/vfs/vfs_jrnl_fat.c"
         "srcs/fatfs/vfs/vfs_jrnl_fat_spiflash.c"
         "srcs/fatfs/diskio/diskio_jrnl.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include srcs/fatfs/diskio srcs/fatfs/vfs
                       PRIV_INCLUDE_DIRS private_include
                       REQUIRES wear_levelling fatfs vfs)
