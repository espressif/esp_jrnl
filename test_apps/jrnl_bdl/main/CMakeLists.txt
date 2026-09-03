idf_component_register(SRCS "test_esp_jrnl_bdl.c" "test_esp_jrnl_mock.c"
                       INCLUDE_DIRS ../../../include
                       PRIV_INCLUDE_DIRS ../../../private_include
                       PRIV_REQUIRES unity esp_partition wear_levelling fatfs esp_blockdev)
