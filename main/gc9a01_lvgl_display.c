#include "gc9a01_lvgl_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "lvgl.h"


static const char *TAG = "DISPLAY";

/* =========================
   PINS (YOUR WORKING ONES)
   ========================= */
#define PIN_NUM_SCLK    18
#define PIN_NUM_MOSI    23
#define PIN_NUM_MISO    -1
#define PIN_NUM_DC      21
#define PIN_NUM_RST     22   // <-- Set a real GPIO pin for RST
#define PIN_NUM_CS      27

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 240
#define LCD_V_RES 240

static esp_lcd_panel_handle_t panel_handle = NULL;
static lv_obj_t *label = NULL;

/* =========================
   LVGL FLUSH
   ========================= */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(
        panel_handle,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        color_map
    );
    lv_disp_flush_ready(drv);
}

/* ========================= */
void gc9a01_init(void)
{
    ESP_LOGI(TAG, "Init SPI");

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Init IO");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 10 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Init GC9A01 panel");

    // Setup RST pin if defined
    if (PIN_NUM_RST >= 0) {
        gpio_reset_pin(PIN_NUM_RST);
        gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);
        // Hardware reset sequence
        gpio_set_level(PIN_NUM_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(PIN_NUM_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = (PIN_NUM_RST >= 0) ? PIN_NUM_RST : -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Fix: correct color inversion / mirroring
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));

    ESP_LOGI(TAG, "Init LVGL");

    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t *buf = NULL;
    buf = heap_caps_malloc(LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LCD_H_RES * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.sw_rotate = 0;
    disp_drv.rotated = 0;
    disp_drv.full_refresh = 1;

    lv_disp_drv_register(&disp_drv);

    /* =========================
       UI: create main label
       ========================= */
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    label = lv_label_create(scr);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, "");

    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_text_color(&style, lv_color_white());
    lv_style_set_text_font(&style, &lv_font_montserrat_40);
    lv_obj_add_style(label, &style, 0);

    ESP_LOGI(TAG, "Display ready");
}

/* =========================
   Send text safely
   ========================= */
void display_send_text(const char *text)
{
    if (!label) return;
    ESP_LOGI("DISPLAY", "display_send_text: %s", text);
    lv_label_set_text(label, text);
}

/* =========================
   Call periodically in main loop
   ========================= */
void display_task(void)
{
    lv_timer_handler();
}