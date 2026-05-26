#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"


#define BTN_PIN    2
#define LED_R_PIN  3
#define LED_G_PIN  4
#define LED_B_PIN  5

static const char *TAG = "SNOWY_HARDWARE_TEST";

void set_leds(int r, int g, int b) {
    gpio_set_level(LED_R_PIN, r);
    gpio_set_level(LED_G_PIN, g);
    gpio_set_level(LED_B_PIN, b);
}

void app_main(void) {
    ESP_LOGI(TAG, "Configurare hardware...");

    // Configurare Buton
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_cfg);

    // Configurare LED-uri
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_R_PIN) | (1ULL << LED_G_PIN) | (1ULL << LED_B_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);

    int test_step = 0;
    bool last_btn_state = true;

    ESP_LOGI(TAG, "Test inceput! Apasa butonul de pe carcasa.");

    while (1) {
        bool current_btn_state = gpio_get_level(BTN_PIN);

        // Detectam apasarea (tranzitia de la 1 la 0)
        if (last_btn_state == true && current_btn_state == false) {
            test_step = (test_step + 1) % 5;
            
            switch (test_step) {
                case 0: set_leds(0, 0, 0); ESP_LOGI(TAG, "LED: OFF"); break;
                case 1: set_leds(1, 0, 0); ESP_LOGI(TAG, "LED: ROSU"); break;
                case 2: set_leds(0, 1, 0); ESP_LOGI(TAG, "LED: VERDE"); break;
                case 3: set_leds(0, 0, 1); ESP_LOGI(TAG, "LED: ALBASTRU"); break;
                case 4: set_leds(1, 1, 1); ESP_LOGI(TAG, "LED: ALB (Toate)"); break;
            }
            vTaskDelay(pdMS_TO_TICKS(200)); // Debounce
        }

        last_btn_state = current_btn_state;
        vTaskDelay(pdMS_TO_TICKS(20)); // Evitam watchdog reset
    }
}
