#include "gc9a01_lvgl_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "lvgl.h"

static const char *TAG = "DISPLAY";  // Logging tag for this module

/* =========================
   PIN CONFIGURATION
   ========================= */
#define PIN_NUM_SCLK    18   // SPI clock (SCK)
#define PIN_NUM_MOSI    23   // SPI MOSI (data to display)
#define PIN_NUM_MISO    -1   // Not used (display is write-only)
#define PIN_NUM_DC      21   // Data/Command control pin
#define PIN_NUM_RST     22   // Reset pin for the display
#define PIN_NUM_CS      27   // Chip select pin

#define LCD_HOST SPI2_HOST   // SPI peripheral used
#define LCD_H_RES 240        // Horizontal resolution
#define LCD_V_RES 240        // Vertical resolution

static esp_lcd_panel_handle_t panel_handle = NULL;  // Handle to LCD panel
static lv_obj_t *label = NULL;                      // LVGL label object

/* =========================
   LVGL FLUSH CALLBACK
   ========================= */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    // Send rendered pixel buffer to the display
    esp_lcd_panel_draw_bitmap(
        panel_handle,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        color_map
    );

    // Notify LVGL that flushing is complete
    lv_disp_flush_ready(drv);
}

/* =========================
   DISPLAY INITIALIZATION
   ========================= */
void gc9a01_init(void)
{
    ESP_LOGI(TAG, "Init SPI");

    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,     // Clock pin
        .mosi_io_num = PIN_NUM_MOSI,     // MOSI pin
        .miso_io_num = PIN_NUM_MISO,     // Not used
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t), // Max transfer size
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Init IO");

    esp_lcd_panel_io_handle_t io_handle = NULL;

    // Configure SPI panel IO
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,        // Data/Command pin
        .cs_gpio_num = PIN_NUM_CS,        // Chip select
        .pclk_hz = 10 * 1000 * 1000,      // SPI clock (10 MHz)
        .lcd_cmd_bits = 8,                // Command width
        .lcd_param_bits = 8,              // Parameter width
        .spi_mode = 0,                    // SPI mode
        .trans_queue_depth = 10,          // Transaction queue size
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Init GC9A01 panel");

    // Optional hardware reset using RST pin
    if (PIN_NUM_RST >= 0) {
        gpio_reset_pin(PIN_NUM_RST);
        gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);

        // Reset sequence: LOW → delay → HIGH → delay
        gpio_set_level(PIN_NUM_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(PIN_NUM_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // Configure panel driver
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = (PIN_NUM_RST >= 0) ? PIN_NUM_RST : -1, // Use RST if available
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,              // Color format
        .bits_per_pixel = 16,                                    // RGB565
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));

    // Initialize panel
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Display orientation and color configuration
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false)); // Disable inversion
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));   // Turn display ON
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false)); // Horizontal mirror
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));      // No XY swap

    ESP_LOGI(TAG, "Init LVGL");

    // Initialize LVGL library
    lv_init();

    // Allocate display buffer (DMA-capable memory)
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t *buf = NULL;
    buf = heap_caps_malloc(LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);

    // Initialize LVGL draw buffer
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LCD_H_RES * 40);

    // Initialize display driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res = LCD_H_RES;      // Horizontal resolution
    disp_drv.ver_res = LCD_V_RES;      // Vertical resolution
    disp_drv.flush_cb = lvgl_flush_cb; // Flush callback
    disp_drv.draw_buf = &draw_buf;     // Drawing buffer
    disp_drv.sw_rotate = 0;            // No software rotation
    disp_drv.rotated = 0;              // Default orientation
    disp_drv.full_refresh = 1;         // Full screen refresh

    // Register display driver with LVGL
    lv_disp_drv_register(&disp_drv);

    /* =========================
       UI: MAIN LABEL SETUP
       ========================= */
    lv_obj_t *scr = lv_scr_act();  // Get active screen

    // Set background to black
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Create label in center of screen
    label = lv_label_create(scr);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP); // Enable text wrapping
    lv_label_set_text(label, "");                      // Initialize empty text

    // Configure label style
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_text_color(&style, lv_color_white());             // White text
    lv_style_set_text_font(&style, &lv_font_montserrat_40);        // Large font
    lv_obj_add_style(label, &style, 0);

    ESP_LOGI(TAG, "Display ready");
}

/* =========================
   UPDATE DISPLAY TEXT
   ========================= */
void display_send_text(const char *text)
{
    if (!label) return;  // Ensure label is initialized

    ESP_LOGI("DISPLAY", "display_send_text: %s", text);  // Log text being displayed
    lv_label_set_text(label, text);                      // Update label text
}

/* =========================
   LVGL TASK HANDLER
   ========================= */
void display_task(void)
{
    lv_timer_handler();  // Process LVGL tasks (must be called periodically)
}