idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_event spiffs driver esp_driver_i2s esp_driver_gpio)

spiffs_create_partition_image(storage ../wavs FLASH_IN_PROJECT)
