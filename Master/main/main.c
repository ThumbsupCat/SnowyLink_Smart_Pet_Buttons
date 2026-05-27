#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_spiffs.h"
#include "lvgl.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "../../Slave/Slave/main/snowylink.h"

// ---- DISPLAY ----
#define PIN_SCK    48
#define PIN_MOSI   38
#define PIN_DC     10
#define PIN_RST     9
#define PIN_CS     -1
#define LCD_HOST       SPI2_HOST
#define LCD_W          240
#define LCD_H          240
#define LCD_PCLK_HZ    (40 * 1000 * 1000)

// ---- AUDIO ----
#define AUDIO_PIN       5
#define SAMPLE_RATE     22050

static const char *TAG = "SNOWY";
static esp_lcd_panel_handle_t panel = NULL;
static SemaphoreHandle_t display_dma_sem = NULL;
static lv_obj_t *ui_image_obj = NULL;
static lv_obj_t *battery_bar = NULL;
static i2s_chan_handle_t tx_chan = NULL;

// === COZI pentru comunicare intre task-uri ===
static QueueHandle_t display_queue;   // mesaje pentru display
static QueueHandle_t audio_queue;     // ID-uri pentru audio

// === MAPPING button_id -> resource ===
static const char* get_image_for_button(uint8_t id) {
    switch (id) {
        case 1: return "bol_mic.jpg";       // hrana
        case 2: return "apa_mica.jpg";      // apa
        case 3: return "joaca_mica.jpg";    // joaca
        case 4: return "atentie_mica.jpg";  // litiera
        case 5: return "iubire_mica.jpg";  // atentie
        default: return "iubire_mica.jpg";
    }
}

static const char* get_audio_for_button(uint8_t id) {
    switch (id) {
        case 1: return "/spiffs/mancare.wav";
        case 2: return "/spiffs/apa.wav";
        case 3: return "/spiffs/joaca.wav";
        case 4: return "/spiffs/atentie.wav";
        case 5: return "/spiffs/iubire.wav";
        default: return NULL; 
    }
}

static int mv_to_percent(uint16_t mv) {
    return mv ;
}

// === DISPLAY ===

static bool notify_panel_io_trans_done(esp_lcd_panel_io_handle_t panel_io, 
                                       esp_lcd_panel_io_event_data_t *edata, 
                                       void *user_ctx) {
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(display_dma_sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static void my_disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int x_start = area->x1, x_end = area->x2 + 1;
    int y_start = area->y1, y_end = area->y2 + 1;
    int num_pixels = (x_end - x_start) * (y_end - y_start);
    uint16_t *pixels = (uint16_t*)px_map;
    
    for (int i = 0; i < num_pixels; i++) {
        pixels[i] = (pixels[i] << 8) | (pixels[i] >> 8);
    }
    
    esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, pixels);
    xSemaphoreTake(display_dma_sem, portMAX_DELAY);
    lv_display_flush_ready(disp);
}

static uint32_t my_lvgl_tick_cb(void) {
    return esp_timer_get_time() / 1000;
}

static void setup_display(void) {
    display_dma_sem = xSemaphoreCreateBinary();
    
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_panel_io_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));
    
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = PIN_RST,
        .bits_per_pixel = 16,
        .rgb_ele_order = COLOR_RGB_ELEMENT_ORDER_RGB,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pcfg, &panel));
    
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
}

static void init_lvgl(void) {
    lv_init();
    lv_tick_set_cb(my_lvgl_tick_cb);
    static lv_color_t buf1[240 * 50];
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_disp_flush_cb);
}

static void init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 10,
        .format_if_mount_failed = false,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %d KB total, %d KB used", total/1024, used/1024);
}

static void load_image_to_screen(const char *file_name) {
    if (ui_image_obj == NULL) {
        ui_image_obj = lv_image_create(lv_screen_active());
        lv_obj_center(ui_image_obj);
    }
    char path[64];
    snprintf(path, sizeof(path), "S:%s", file_name);
    lv_image_set_src(ui_image_obj, path);
    lv_obj_invalidate(ui_image_obj);
}

static void update_battery_display(int level) {
    if (battery_bar == NULL) {
        battery_bar = lv_bar_create(lv_screen_active());
        lv_obj_set_size(battery_bar, 40, 15);
        lv_obj_align(battery_bar, LV_ALIGN_TOP_RIGHT, -10, 10);
        lv_obj_set_style_bg_color(battery_bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
    }
    lv_bar_set_value(battery_bar, level, LV_ANIM_ON);
}

// === AUDIO I2S PDM ===

static void init_i2s_pdm(void) {
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
    i2s_channel_enable(tx_chan);
}

static void play_wav(uint8_t id) {
    const char *path = get_audio_for_button(id);
    if (path == NULL) return;
    
    
    ESP_LOGI(TAG, "Redare: %s", path);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Fisier negăsit: %s", path);
        i2s_channel_disable(tx_chan);
        return;
    }
    
    fseek(f, 44, SEEK_SET);
    size_t bytes_read, bytes_written;
    static uint8_t buffer[4096];
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        i2s_channel_write(tx_chan, buffer, bytes_read, &bytes_written, portMAX_DELAY);
    }
    fclose(f);
    
    // Drain
    int16_t silent_padding[256] = {0};
    i2s_channel_write(tx_chan, silent_padding, sizeof(silent_padding), &bytes_written, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(100));
    
}

// === ESP-NOW CALLBACK ===

static uint32_t last_sequence = 0;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len != sizeof(snowy_msg_t)) return;
    
    snowy_msg_t msg;
    memcpy(&msg, data, sizeof(msg));
    
    // Anti-duplicate (daca Slave trimite multiple copii)
    if (msg.sequence == last_sequence) {
        return;
    }
    last_sequence = msg.sequence;
    
    ESP_LOGI(TAG, "Btn=%d Bat=%dmV Seq=%lu",
             msg.button_id, msg.battery_mv, msg.sequence);
    
    // Trimite la ambele task-uri (display + audio)
    if (msg.msg_type == MSG_BUTTON_PRESS) {
        xQueueSend(display_queue, &msg, 0);
        uint8_t id = msg.button_id;
        xQueueSend(audio_queue, &id, 0);
    }
}

// === TASK-URI ===

void lvgl_task(void *p) {
    snowy_msg_t msg;
    
    load_image_to_screen("iubire_mica.jpg");
    update_battery_display(100);
    
    while (1) {
        if (xQueueReceive(display_queue, &msg, 0) == pdTRUE) {
            const char *img = get_image_for_button(msg.button_id);
            ESP_LOGI(TAG, "Display: %s", img);
            load_image_to_screen(img);
            update_battery_display(mv_to_percent(msg.battery_mv));
        }
        
        // LVGL returnează ms până la următorul update necesar
        uint32_t time_till_next = lv_timer_handler();
        if (time_till_next < 5) time_till_next = 5;
        if (time_till_next > 30) time_till_next = 30;
        vTaskDelay(pdMS_TO_TICKS(time_till_next));
    }
}

void audio_task(void *p) {
    uint8_t id;
    while (1) {
        // Asteapta comanda de audio (blocking)
        if (xQueueReceive(audio_queue, &id, portMAX_DELAY) == pdTRUE) {
            play_wav(id);
        }
    }
}

// === MAIN ===

void app_main(void) {
    ESP_LOGI(TAG, "SnowyLink Master - Demo");
    
    // === NVS + WiFi + ESP-NOW ===
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
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
    ESP_LOGI(TAG, "MAC Master: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    
    // === Hardware ===
    init_spiffs();
    setup_display();
    init_lvgl();
    init_i2s_pdm();
    
    
    // === Cozi ===
    display_queue = xQueueCreate(10, sizeof(snowy_msg_t));
    audio_queue = xQueueCreate(10, sizeof(uint8_t));
    
    // === Task-uri ===
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 1024*12, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(audio_task, "audio", 1024*8, NULL, 4, NULL, 1);
    
    ESP_LOGI(TAG, "Sistem complet. Astept apasari de la Slave...");
}
