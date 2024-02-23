
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#if CONFIG_HMI_LCD_CONTROLLER_ST7796
#include "esp_lcd_st7796.h"
#endif

/* Check if its required for sleep */
// #if CONFIG_HMI_LCD_TOUCH_ENABLED
// #include <esp_lcd_touch.h>
// #endif

#if CONFIG_HMI_LCD_TOUCH_CONTROLLER_FT5X06
#include "esp_lcd_touch_ft5x06.h"
#endif
#include <driver/i2c.h>


static const char *TAG = "HMI";

// Using SPI2 in the example
#define LCD_HOST  SPI2_HOST

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define HMI_LCD_PIXEL_CLOCK_HZ     (20 * 1000 * 1000)
#define HMI_LCD_BK_LIGHT_ON_LEVEL  1
#define HMI_LCD_BK_LIGHT_OFF_LEVEL !HMI_LCD_BK_LIGHT_ON_LEVEL

#define HMI_PIN_NUM_SCLK           14
#define HMI_PIN_NUM_MOSI           13
#define HMI_PIN_NUM_MISO           -1
#define HMI_PIN_NUM_LCD_DC         21
#define HMI_PIN_NUM_LCD_RST        22
#define HMI_PIN_NUM_LCD_CS         15

#define HMI_PIN_NUM_BK_LIGHT       23
#define HMI_PIN_NUM_TOUCH_CS       15   // ??

#define HMI_TOUCH_I2C_NUM 1
#define HMI_I2C_CLK_SPEED_HZ 400000
#define HMI_PIN_TOUCH_INT   GPIO_NUM_39
#define HMI_PIN_TOUCH_SDA   GPIO_NUM_18
#define HMI_PIN_TOUCH_SCL   GPIO_NUM_19

// The pixel number in horizontal and vertical
#if CONFIG_HMI_LCD_CONTROLLER_ST7796
#define HMI_LCD_H_RES              320
#define HMI_LCD_V_RES              480
#define HMI_LCD_DRAW_BUFF_HEIGHT (50)
#define HMI_LCD_COLOR_SPACE     (ESP_LCD_COLOR_SPACE_BGR)
#define HMI_LCD_BITS_PER_PIXEL  (16)
#define HMI_LCD_DRAW_BUFF_DOUBLE (1)
#endif

// Bit number used to represent command and parameter
#define HMI_LCD_CMD_BITS           8
#define HMI_LCD_PARAM_BITS         8

// LVGL Settings
#define HMI_LVGL_TICK_PERIOD_MS    2
#define HMI_LVGL_TASK_MAX_DELAY_MS 500
#define HMI_LVGL_TASK_MIN_DELAY_MS 1
#define HMI_LVGL_TASK_STACK_SIZE   (4 * 1024)
#define HMI_LVGL_TASK_PRIORITY     2

#define HMI_LVGL_TICK_PERIOD_MS    2

/* LCD IO and panel */
static esp_lcd_panel_io_handle_t lcd_io = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;


static esp_err_t app_lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    /* LCD backlight */
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << HMI_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    /* LCD initialization */
    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .sclk_io_num = HMI_PIN_NUM_SCLK,
        .mosi_io_num = HMI_PIN_NUM_MOSI,
        .miso_io_num = HMI_PIN_NUM_MISO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = HMI_LCD_H_RES * HMI_LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGD(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = HMI_PIN_NUM_LCD_DC,
        .cs_gpio_num = HMI_PIN_NUM_LCD_CS,
        .pclk_hz = HMI_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = HMI_LCD_CMD_BITS,
        .lcd_param_bits = HMI_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &lcd_io));

    ESP_LOGD(TAG, "Install LCD driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = HMI_PIN_NUM_LCD_RST,
        .color_space = HMI_LCD_COLOR_SPACE,
        .bits_per_pixel = HMI_LCD_BITS_PER_PIXEL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(lcd_io, &panel_config, &lcd_panel));

    esp_lcd_panel_reset(lcd_panel);
    esp_lcd_panel_init(lcd_panel);
    esp_lcd_panel_mirror(lcd_panel, true, true);
    esp_lcd_panel_disp_on_off(lcd_panel, true);

    /* LCD backlight on */
    ESP_ERROR_CHECK(gpio_set_level(HMI_PIN_NUM_BK_LIGHT, HMI_LCD_BK_LIGHT_ON_LEVEL));

    return ret;
}


static esp_err_t app_touch_init(void)
{
    /* Initilize I2C */
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = HMI_PIN_TOUCH_SDA,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_io_num = HMI_PIN_TOUCH_SCL,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = HMI_I2C_CLK_SPEED_HZ
    };
    ESP_ERROR_CHECK(i2c_param_config(HMI_TOUCH_I2C_NUM, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(HMI_TOUCH_I2C_NUM, i2c_conf.mode, 0, 0, 0));

    /* Initialize touch HW */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = HMI_LCD_H_RES,
        .y_max = HMI_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
        .int_gpio_num = HMI_PIN_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)HMI_TOUCH_I2C_NUM, &tp_io_config, &tp_io_handle));
    return esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &touch_handle);
}

static esp_err_t app_lvgl_init(void)
{
    /* Initialize LVGL */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,         /* LVGL task priority */
        .task_stack = 4096,         /* LVGL task stack size */
        .task_affinity = -1,        /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500,   /* Maximum sleep in LVGL task */
        .timer_period_ms = 5        /* LVGL timer tick period in ms */
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = HMI_LCD_H_RES * HMI_LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t),
        .double_buffer = HMI_LCD_DRAW_BUFF_DOUBLE,
        .hres = HMI_LCD_H_RES,
        .vres = HMI_LCD_V_RES,
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        }
    };
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);

    return ESP_OK;
}

static void _app_button_cb(lv_event_t *e)
{
    lv_disp_rotation_t rotation = lv_disp_get_rotation(lvgl_disp);
    rotation++;
    if (rotation > LV_DISPLAY_ROTATION_270) {
        rotation = LV_DISPLAY_ROTATION_0;
    }

    /* LCD HW rotation */
    lv_disp_set_rotation(lvgl_disp, rotation);
}

static void app_main_display(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* Task lock */
    lvgl_port_lock(0);

    /* Your LVGL objects code here .... */

    /* Label */
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_width(label, HMI_LCD_H_RES);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
#if LVGL_VERSION_MAJOR == 8
    lv_label_set_recolor(label, true);
    lv_label_set_text(label, "#FF0000 "LV_SYMBOL_BELL" Hello world Espressif and LVGL "LV_SYMBOL_BELL"#\n#FF9400 "LV_SYMBOL_WARNING" For simplier initialization, use BSP "LV_SYMBOL_WARNING" #");
#else
    lv_label_set_text(label, LV_SYMBOL_BELL" Hello world Espressif and LVGL "LV_SYMBOL_BELL"\n "LV_SYMBOL_WARNING" For simplier initialization, use BSP "LV_SYMBOL_WARNING);
#endif
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -30);

    /* Button */
    lv_obj_t *btn = lv_btn_create(scr);
    label = lv_label_create(btn);
    lv_label_set_text_static(label, "Rotate screen");
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(btn, _app_button_cb, LV_EVENT_CLICKED, NULL);

    /* Task unlock */
    lvgl_port_unlock();
}

void app_main(void)
{
    /* LCD HW initialization */
    ESP_ERROR_CHECK(app_lcd_init());

    /* Touch initialization */
    ESP_ERROR_CHECK(app_touch_init());

    /* LVGL initialization */
    ESP_ERROR_CHECK(app_lvgl_init());

    /* Show LVGL objects */
    app_main_display();
}