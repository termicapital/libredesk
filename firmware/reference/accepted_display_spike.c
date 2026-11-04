#include "display_spike.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#define RGB565_RED    UINT16_C(0xF800)
#define RGB565_GREEN  UINT16_C(0x07E0)
#define RGB565_BLUE   UINT16_C(0x001F)
#define RGB565_WHITE  UINT16_C(0xFFFF)
#define RGB565_BLACK  UINT16_C(0x0000)
#define FRAME_PIXELS  ((size_t)BOARD_LCD_WIDTH * (size_t)BOARD_LCD_HEIGHT)
#define BACKLIGHT_MAX_DUTY ((1U << 10U) - 1U)
#define DIRECT_TRANSFER_TIMEOUT_MS 1000U

_Static_assert(BOARD_LCD_INITIAL_BRIGHTNESS <= 25, "first-light brightness must stay at or below 25 percent");

static const char *TAG = "display_spike";
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_frame;
static SemaphoreHandle_t s_transfer_done;

static bool IRAM_ATTR direct_transfer_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static void backlight_init_off(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_PIN_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = BOARD_LCD_BACKLIGHT_ACTIVE_HIGH ? 0 : 1,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

static void backlight_set_percent(unsigned int percent)
{
    if (percent > 25U) {
        percent = 25U;
    }
    const uint32_t duty = (BACKLIGHT_MAX_DUTY * percent) / 100U;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

static void shared_spi_devices_deselect(void)
{
    /* The socket has a 10K pull-up, but drive CS high before SCLK/MOSI become outputs too. */
    ESP_ERROR_CHECK(gpio_set_level(BOARD_SD_PIN_CS, 1));
    const gpio_config_t sd_cs = {
        .pin_bit_mask = UINT64_C(1) << BOARD_SD_PIN_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_cs));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_SD_PIN_CS, 1));
}

static void frame_fill(uint16_t color)
{
    for (size_t index = 0; index < FRAME_PIXELS; index++) {
        s_frame[index] = color;
    }
}

static void frame_present(void)
{
    (void)xSemaphoreTake(s_transfer_done, 0);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
        s_panel,
        0,
        0,
        BOARD_LCD_WIDTH,
        BOARD_LCD_HEIGHT,
        s_frame));
    if (xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(DIRECT_TRANSFER_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "LCD DMA transfer did not complete within %u ms", DIRECT_TRANSFER_TIMEOUT_MS);
        abort();
    }
}

static void show_solid(const char *name, uint16_t color, TickType_t duration)
{
    ESP_LOGI(TAG, "solid %s", name);
    frame_fill(color);
    frame_present();
    vTaskDelay(duration);
}

static void lvgl_block(lv_obj_t *parent, int x, int y, int width, int height, uint32_t rgb)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_pos(block, x, y);
    lv_obj_set_size(block, width, height);
    lv_obj_set_style_bg_color(block, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(block, LV_OPA_COVER, LV_PART_MAIN);
}

static void show_lvgl_orientation_pattern(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = BOARD_LCD_WIDTH * 40U,
        .double_buffer = true,
        .hres = BOARD_LCD_WIDTH,
        .vres = BOARD_LCD_HEIGHT,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = BOARD_LCD_SWAP_XY,
            .mirror_x = BOARD_LCD_MIRROR_X,
            .mirror_y = BOARD_LCD_MIRROR_Y,
        },
        .flags = {
            .buff_dma = true,
            .sw_rotate = true,
            /* The ST7789 RAMCTRL is configured for little-endian RGB565 above. */
            .swap_bytes = false,
        },
    };
    lv_display_t *display = lvgl_port_add_disp(&display_cfg);
    if (display == NULL) {
        ESP_LOGE(TAG, "could not add the display to esp_lvgl_port");
        abort();
    }
    /* esp_lvgl_port has now replaced the direct-transfer callback on this IO handle. */
    vSemaphoreDelete(s_transfer_done);
    s_transfer_done = NULL;

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "could not acquire the LVGL lock");
        abort();
    }

    /* Keep hardware MV disabled: software rotation transforms partial flush areas with their pixels. */
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);

    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lvgl_block(screen, 4, 4, 36, 36, 0xFF0000);
    lvgl_block(screen, 200, 4, 36, 36, 0x00FF00);
    lvgl_block(screen, 4, 200, 36, 36, 0x0000FF);
    lvgl_block(screen, 200, 200, 36, 36, 0xFFFFFF);

    /* A deliberately asymmetric, large block digit 1 for mirror/orientation checks. */
    lvgl_block(screen, 91, 58, 25, 24, 0xFFFF00);
    lvgl_block(screen, 108, 42, 28, 156, 0xFFFF00);
    lvgl_block(screen, 78, 190, 88, 28, 0xFFFF00);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "esp_lvgl_port active: red TL, green TR, blue BL, white BR, yellow digit 1");
}

static void panel_init(void)
{
    backlight_init_off();
    shared_spi_devices_deselect();

    s_transfer_done = xSemaphoreCreateBinary();
    if (s_transfer_done == NULL) {
        ESP_LOGE(TAG, "could not allocate the LCD transfer semaphore");
        abort();
    }

    const spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)(FRAME_PIXELS * sizeof(uint16_t) + 8U),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .on_color_trans_done = direct_transfer_done,
        .user_ctx = s_transfer_done,
        .lcd_cmd_bits = BOARD_LCD_CMD_BITS,
        .lcd_param_bits = BOARD_LCD_PARAM_BITS,
        .spi_mode = BOARD_LCD_SPI_MODE,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST,
        &io_config,
        &s_panel_io));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RESET,
        .rgb_ele_order = BOARD_LCD_RGB_ELEMENT_ORDER,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, BOARD_LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, BOARD_LCD_INVERT_COLOR));

    s_frame = heap_caps_malloc(FRAME_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_frame == NULL) {
        ESP_LOGE(TAG, "could not allocate %u-byte DMA framebuffer", (unsigned int)(FRAME_PIXELS * sizeof(uint16_t)));
        abort();
    }

    /* Never expose uninitialized panel RAM to the backlight. */
    frame_fill(RGB565_BLACK);
    frame_present();
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    backlight_set_percent(BOARD_LCD_INITIAL_BRIGHTNESS);
}

void display_spike_run_forever(void)
{
    panel_init();
    ESP_LOGI(TAG, "panel ready: 240x240, SPI2 mode 0 at 12 MHz, backlight 25 percent");

    show_solid("RED", RGB565_RED, pdMS_TO_TICKS(1000));
    show_solid("GREEN", RGB565_GREEN, pdMS_TO_TICKS(1000));
    show_solid("BLUE", RGB565_BLUE, pdMS_TO_TICKS(1000));
    show_solid("WHITE", RGB565_WHITE, pdMS_TO_TICKS(1000));

    /* frame_present() has confirmed the final direct SPI transfer is complete. */
    heap_caps_free(s_frame);
    s_frame = NULL;

    show_lvgl_orientation_pattern();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
