#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "snowylink.h"   // pentru snowy_msg_t și MASTER_MAC

#define BTN_GPIO        5      // butonul fizic (de la schema)
#define MY_BUTTON_ID    5      // ID-ul acestui buton (1=hrana, etc.)

static const char *TAG = "C3";

static uint32_t seq_counter = 0;

// Callback pentru confirmarea livrarii
static void on_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "Livrat la Master");
    } else {
        ESP_LOGW(TAG, "Pierdut (no ACK)");
    }
}

static void send_button_press(uint16_t battery_mv) {
    int procent = (battery_mv - 3000) * 100 / (4200 - 3000);
    if (procent < 0) procent = -1;
    snowy_msg_t msg = {
        .msg_type   = MSG_BUTTON_PRESS,
        .button_id  = MY_BUTTON_ID,
        .battery_mv = procent,
        .sequence   = ++seq_counter,
    };
    memset(msg.reserved, 0, sizeof(msg.reserved));
    
    esp_err_t err = esp_now_send(MASTER_MAC, (uint8_t*)&msg, sizeof(msg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Send error: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Trimis: btn=%d bat=%dmV seq=%lu",
                 msg.button_id, msg.battery_mv, msg.sequence);
    }
}

static uint16_t read_battery_mv(void) {
    // citeste ADC pe GPIO0 (placeholder pentru moment)
    return 3700;
}

void app_main(void) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "SnowyLink SLAVE - Buton #%d", MY_BUTTON_ID);

    
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(11, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, 
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "MAC-ul meu: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // INIT ESP-NOW + ADAUGA MASTER CA PEER ===
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));
    
    esp_now_peer_info_t peer = {
        .channel = 11,
        .encrypt = false,
        .ifidx   = WIFI_IF_STA,
    };
    memcpy(peer.peer_addr, MASTER_MAC, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    
    esp_now_rate_config_t rate_cfg = {
        .phymode = WIFI_PHY_MODE_11B,
        .rate    = WIFI_PHY_RATE_1M_L,
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(MASTER_MAC, &rate_cfg));
    ESP_LOGI(TAG, "Rate forced: 1Mbps DSSS");
    
    // CONFIG BUTON cu PULL-UP
    
    gpio_config_t btn_cfg = {
        .pin_bit_mask = BIT64(BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&btn_cfg);
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Aștept apăsare buton (GPIO %d)...", BTN_GPIO);
    ESP_LOGI(TAG, "");
    
    // BUCLA PRINCIPALA - POLLING BUTON
    
    while (1) {
        // Asteaptă ca butonul sa fie apăsat (GPIO trece de la 1 la 0)
        while (gpio_get_level(BTN_GPIO) == 1) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        ESP_LOGI(TAG, "Pisica a apăsat butonul!");
        
        uint16_t battery = read_battery_mv();
        send_button_press(battery);
        
        // Anti-bounce (300ms) + asteapta eliberare buton
        vTaskDelay(pdMS_TO_TICKS(300));
        while (gpio_get_level(BTN_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        ESP_LOGI(TAG, "Buton eliberat. Astept urmatoarea apasare.\n");
    }
}