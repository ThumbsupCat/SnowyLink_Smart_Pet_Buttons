idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_driver_ledc esp_driver_spi esp_driver_gpio lvgl spiffs esp_timer esp_wifi esp_event esp_netif nvs_flash esp_driver_i2s)

# MANDATORIU: Această comandă funcționează doar DACĂ este pusă sub idf_component_register
spiffs_create_partition_image(storage ../images FLASH_IN_PROJECT)