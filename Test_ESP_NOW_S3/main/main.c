#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "../../Slave/Slave/main/snowylink.h"

// Pentru cine citeste asta, chiar mi-am rupt parul din cap, antena de la C3-Supermini este un gunoi


static const char *TAG = "S3_MASTER_LR";

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "MESAJ PRIMIT DE LA C3!");
    const snowy_msg_t *mesaj = (snowy_msg_t*)data;
    if (mesaj->msg_type == 0x01) {
        const char *interpretare_id;
        switch (mesaj->button_id) {
            case 1: interpretare_id = "Mancare"; break;
            case 2: interpretare_id = "Apa"; break;
            case 3: interpretare_id = "Joaca"; break;
            case 4: interpretare_id = "Litiera"; break;
            case 5: interpretare_id = "Atentie"; break;
            case 6: interpretare_id = "Iubire"; break;
            default: interpretare_id = "Necunoscut"; break;
        }
        ESP_LOGI(TAG, "Snowy a apasat butonasul %s", interpretare_id);
        ESP_LOGI(TAG, "Bateria este %d%%", mesaj->battery_mv);
    }
    ESP_LOGI(TAG, "  RSSI: %d dBm (Putere semnal)", info->rx_ctrl->rssi);
    
    char text_buf[80] = {0};
    int copy_len = len < 79 ? len : 79;
    memcpy(text_buf, data, copy_len);
    //ESP_LOGI(TAG, "  Text: \"%s\"", text_buf);
    ESP_LOGI(TAG, "----------------------------------------");
}

void app_main(void) {
    ESP_LOGI(TAG, "S3 MASTER - ESP-NOW TEST");
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "esp_now_dummy_s3",
            .ssid_len = 16,
            .ssid_hidden = 1,
            .channel = 11, 
            .max_connection = 1,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(11, WIFI_SECOND_CHAN_NONE)); 
    
    //  Adaugam protocolul LR ---
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR));
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    
    ESP_LOGI(TAG, "Astept mesaje LR pe Canalul 11...");
    
    int counter = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "S3 alive - %d sec", ++counter * 5);
    }
}
