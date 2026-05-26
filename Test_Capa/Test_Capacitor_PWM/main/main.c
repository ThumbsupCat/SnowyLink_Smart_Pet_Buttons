#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"   // Vital pentru evitarea crash-ului!
#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_spiffs.h"
#include "driver/i2s_pdm.h"
#include "../../../Slave/Slave/main/snowylink.h"
#include "driver/gpio.h"

static const char *TAG = "MASTER_AUDIO";

// --- CONFIGURATIE HARDWARE AUDIO ---
#define AUDIO_PIN       5       
#define SAMPLE_RATE     22050   

// --- DEFINIRE COADA ---
QueueHandle_t coada_comenzi_audio;
i2s_chan_handle_t tx_chan;

// 1. Functia de obtinere a fisierului
const char* get_fisier_audio(uint8_t id_buton) {
    switch (id_buton) {
        case 1: return "/spiffs/mancare.wav";
        case 2: return "/spiffs/apa.wav";
        case 3: return "/spiffs/joaca.wav";
        case 5: return "/spiffs/atentie.wav";
        case 6: return "/spiffs/iubire.wav";
        default: return NULL; 
    }
}

// 2. Functiile de initializare Hardware
void init_spiffs() {
    ESP_LOGI(TAG, "Initializare SPIFFS...");
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = false
    };
    esp_vfs_spiffs_register(&conf);
}

void init_i2s_pdm() {
    ESP_LOGI(TAG, "Configurare I2S PDM TX...");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_new_channel(&chan_cfg, &tx_chan, NULL);

    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_GPIO_UNUSED, 
            .dout = AUDIO_PIN,      
            .invert_flags = { .clk_inv = false },
        },
    };
    i2s_channel_init_pdm_tx_mode(tx_chan, &pdm_cfg);
    //i2s_channel_enable(tx_chan);
}

// 3. Functia de redare
void play_wav(uint8_t id) {
    const char* fisier_curent = get_fisier_audio(id);
    if (fisier_curent == NULL) return;
    
    // === 1. Recreare canal I2S de la zero ===
    if (tx_chan != NULL) {
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    init_i2s_pdm();
    
    esp_err_t ret = i2s_channel_enable(tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "Redare: %s", fisier_curent);
    FILE *f = fopen(fisier_curent, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Nu s-a gasit fisierul!");
        i2s_channel_disable(tx_chan);
        return;
    }
    
    fseek(f, 44, SEEK_SET);  // Skip WAV header
    size_t bytes_read = 0;
    size_t bytes_written = 0;
    static uint8_t buffer[4096] = {0};
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        i2s_channel_write(tx_chan, buffer, bytes_read, &bytes_written, portMAX_DELAY);
    }
    fclose(f);
    
    // === 2. Drain - asteapta sa se goleasca buffer-ul DMA ===
    static int16_t silent_padding[1024] = {0};
    size_t bytes_pushed;
    i2s_channel_write(tx_chan, silent_padding, sizeof(silent_padding), &bytes_pushed, pdMS_TO_TICKS(100));
    
    // === 3. Distrugere completa canal ===
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
    
    // === 4. FORTEAZA GPIO la LOW  ===
    gpio_reset_pin(AUDIO_PIN);
    gpio_set_direction(AUDIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_PIN, 0);
    
    ESP_LOGI(TAG, "I2S deconectat complet");
}

// 4. Callback-ul ESP-NOW
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    const snowy_msg_t *mesaj = (snowy_msg_t*)data;
    ESP_LOGI(TAG, "Mesaj primit, button_id = %d", mesaj->button_id);
    
    if (mesaj->msg_type == 0x01) {
        ESP_LOGI(TAG, "Semnal interceptat! Baterie Slave: %d%%", mesaj->battery_mv);
        
        uint8_t id_buton = mesaj->button_id;
        // Trimitem ID-ul in coada; timeout 0 = instant
        xQueueSend(coada_comenzi_audio, &id_buton, 0); 
    }
}

// 5. Ansamblarea 
void app_main(void) {

    ESP_LOGI(TAG, "S3 MASTER - AUDIO + ESP-NOW LR ACTIVE");

    
    // Initializare Hardware & Coada
    init_spiffs();
    init_i2s_pdm();
    i2s_del_channel(tx_chan);
    tx_chan = NULL;

    // Seteaza GPIO 5 la LOW de la boot
    gpio_reset_pin(AUDIO_PIN);
    gpio_set_direction(AUDIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_PIN, 0);
    coada_comenzi_audio = xQueueCreate(10, sizeof(uint8_t));
    
    // Initializare Retea
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
    
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR));
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    
    ESP_LOGI(TAG, "Sistem complet. Astept comenzi pe canalul 11!");
    
    uint8_t id_primit;
    
    // Bucla principala care sta si pazeste coada
    while (1) {
        // ESP-ul "doarme" aici pana apare ceva in coada
        if (xQueueReceive(coada_comenzi_audio, &id_primit, portMAX_DELAY) == pdTRUE) {
            play_wav(id_primit);
        }
    }
}
