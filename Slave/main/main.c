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
#include "snowylink.h"   // pentru snowy_msg_t si MASTER_MAC
#include "esp_sleep.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/rtc_io.h"

#define BTN_GPIO        5      // butonul fizic (de la schema)
#define MY_BUTTON_ID    5      // ID-ul acestui buton (1=hrana, etc.)
#define BATTERY_ADC_PIN     0      // GPIO 0 = ADC1_0
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define SIMULATED_BATTERY MY_BUTTON_ID * 20   // Bateria simulata ID * 20

static const char *TAG = "C3";

RTC_DATA_ATTR static uint32_t seq_counter = 0;   // Persistent intre deep sleep cycles
RTC_DATA_ATTR static uint32_t boot_count = 0;

static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_handle = NULL;

// Callback pentru confirmarea livrarii
static void on_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "Livrat la Master");
    } else {
        ESP_LOGW(TAG, "Pierdut (no ACK)");
    }
}

static void init_adc(void) {
    // Init ADC1 unit
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_new_unit(&init_cfg, &adc1_handle);
    
    // Config canal (GPIO 0)
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,   // pentru range 0-3.3V
    };
    adc_oneshot_config_channel(adc1_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
    
    // Calibrare
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc1_cali_handle);
    
    ESP_LOGI(TAG, "ADC initialized on GPIO %d", BATTERY_ADC_PIN);
}

static void send_button_press(uint16_t battery_mv) {
    int procent = (battery_mv - 3000) * 100 / (4200 - 3000);
    if (procent < 0) procent = 0;
    if (procent > 100) procent = 100;
    snowy_msg_t msg = {
        .msg_type   = MSG_BUTTON_PRESS,
        .button_id  = MY_BUTTON_ID,
        .battery_mv = SIMULATED_BATTERY,
        .sequence   = ++seq_counter,
    };
    memset(msg.reserved, 0, sizeof(msg.reserved));
    
    esp_err_t err = esp_now_send(MASTER_MAC, (uint8_t*)&msg, sizeof(msg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Send error: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Trimis: btn=%d bat=%d%% seq=%lu",
                 msg.button_id, msg.battery_mv, msg.sequence);
    }
}

static void deinit_adc(void) {
    adc_cali_delete_scheme_curve_fitting(adc1_cali_handle);
    adc_oneshot_del_unit(adc1_handle);
}

static uint16_t read_battery_mv(void) {
    // citeste ADC pe GPIO0 (placeholder pentru moment)
    int raw;
    int voltage_mv;
    
    // Citeste valoarea raw (0-4095)
    adc_oneshot_read(adc1_handle, BATTERY_ADC_CHANNEL, &raw);
    
    // Converteste la millivolti (calibrat)
    adc_cali_raw_to_voltage(adc1_cali_handle, raw, &voltage_mv);
    
    // Divider 100K+100K -> tensiunea la jum. ca sa nu mancam niste friptura de ESP32-C3 
    // tensiunea actuala bateriei = 2x masurata
    uint16_t actual_mv = voltage_mv * 2;
    
    ESP_LOGI(TAG, "ADC raw=%d, measured=%dmV, battery=%dmV", 
             raw, voltage_mv, actual_mv);
    
    return actual_mv;
}

static void go_to_deep_sleep(void) {
    ESP_LOGI(TAG, "Pregatire deep sleep...");

    while (gpio_get_level(BTN_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    gpio_set_pull_mode(BTN_GPIO, GPIO_PULLUP_ONLY);
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        BIT(BTN_GPIO), ESP_GPIO_WAKEUP_GPIO_LOW));
    gpio_hold_en(BTN_GPIO);

    ESP_LOGI(TAG, "Nivel GPIO5: %d", gpio_get_level(BTN_GPIO));
    ESP_LOGI(TAG, "Adorm. Seq: %lu, Boot: %lu", seq_counter, boot_count);
    esp_deep_sleep_start();
}

void app_main(void) {
    boot_count++;
    gpio_hold_dis(BTN_GPIO);
    
    // Detecteaza cauza wake-ului
    uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    bool is_button_wake = (wake_causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) != 0;
    
    ESP_LOGI(TAG, "");
    if (is_button_wake) {
        ESP_LOGI(TAG, "Boot #%lu - TREZIT DE BUTON", boot_count);
    } else {
        ESP_LOGI(TAG, "Boot #%lu - PORNIRE INITIALA", boot_count);
    }
    
    // Init ADC (pentru baterie)
    init_adc();
    
    // Config buton cu pull-up (pentru wake)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = BIT64(BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_cfg);
    
    if (is_button_wake) {
        // Init WiFi + ESP-NOW (doar daca trimitem mesaj)
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
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));
        
        ESP_ERROR_CHECK(esp_now_init());
        ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));
        
        esp_now_peer_info_t peer = {
            .channel = 11,
            .encrypt = false,
            .ifidx = WIFI_IF_STA,
        };
        memcpy(peer.peer_addr, MASTER_MAC, 6);
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
        
        // Citeste baterie + trimite mesaj
        uint16_t battery = read_battery_mv();
        send_button_press(battery);
        
        // Asteapta confirmarea livrarii
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGI(TAG, "Primul boot - astept buton fara sa trimit nimic");
    }
    // Inapoi la somnic
    deinit_adc();
    go_to_deep_sleep();
}