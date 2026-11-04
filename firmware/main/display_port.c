#include "display_port.h"

#include <stddef.h>

#include "board.h"
#include "display_port_flow.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"

#define RGB565_RED UINT16_C(0xF800)
#define RGB565_GREEN UINT16_C(0x07E0)
#define RGB565_BLUE UINT16_C(0x001F)
#define RGB565_WHITE UINT16_C(0xFFFF)
#define RGB565_BLACK UINT16_C(0x0000)
#define FRAME_PIXELS ((size_t)BOARD_LCD_WIDTH * (size_t)BOARD_LCD_HEIGHT)
#define FRAME_BYTES (FRAME_PIXELS * sizeof(uint16_t))
#define BACKLIGHT_MAX_DUTY ((1U << 10U) - 1U)
#define DIRECT_TRANSFER_TIMEOUT_MS 1000U
#define ACCEPTANCE_DELAY_MS 1000U

_Static_assert(BOARD_LCD_INITIAL_BRIGHTNESS <= 25,
               "first-light brightness must stay at or below 25 percent");

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_display;
static uint16_t *s_frame;
static SemaphoreHandle_t s_transfer_done;

typedef enum display_port_state {
    DISPLAY_PORT_NOT_STARTED = 0,
    DISPLAY_PORT_STARTING,
    DISPLAY_PORT_STARTED,
    DISPLAY_PORT_FAILED,
} display_port_state_t;

static display_port_state_t s_state = DISPLAY_PORT_NOT_STARTED;

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

static esp_err_t backlight_init_off(void)
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
    esp_err_t error = ledc_timer_config(&timer);
    if (error != ESP_OK) {
        return error;
    }
    error = ledc_channel_config(&channel);
    if (error != ESP_OK) {
        (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    }
    return error;
}

static esp_err_t backlight_set_percent(uint8_t percent)
{
    if (percent > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t duty = (BACKLIGHT_MAX_DUTY * (uint32_t)percent) / 100U;
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (error != ESP_OK) {
        return error;
    }
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t shared_spi_devices_deselect(void)
{
    /* Drive CS high before SCLK/MOSI become outputs too. */
    esp_err_t error = gpio_set_level(BOARD_SD_PIN_CS, 1);
    if (error != ESP_OK) {
        return error;
    }
    const gpio_config_t sd_cs = {
        .pin_bit_mask = UINT64_C(1) << BOARD_SD_PIN_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&sd_cs);
    if (error != ESP_OK) {
        return error;
    }
    return gpio_set_level(BOARD_SD_PIN_CS, 1);
}

static void frame_fill(uint16_t color)
{
    for (size_t index = 0U; index < FRAME_PIXELS; ++index) {
        s_frame[index] = color;
    }
}

static esp_err_t frame_present(void)
{
    (void)xSemaphoreTake(s_transfer_done, 0);
    esp_err_t error = esp_lcd_panel_draw_bitmap(
        s_panel, 0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, s_frame);
    if (error != ESP_OK) {
        return error;
    }
    if (xSemaphoreTake(s_transfer_done,
                       pdMS_TO_TICKS(DIRECT_TRANSFER_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t show_solid(uint16_t color)
{
    frame_fill(color);
    esp_err_t error = frame_present();
    if (error != ESP_OK) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(ACCEPTANCE_DELAY_MS));
    return ESP_OK;
}

static esp_err_t lvgl_block(lv_obj_t *parent,
                            int x,
                            int y,
                            int width,
                            int height,
                            uint32_t rgb)
{
    lv_obj_t *block = lv_obj_create(parent);
    if (block == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(block);
    lv_obj_set_pos(block, x, y);
    lv_obj_set_size(block, width, height);
    lv_obj_set_style_bg_color(block, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(block, LV_OPA_COVER, LV_PART_MAIN);
    return ESP_OK;
}

static esp_err_t create_orientation_pattern(void)
{
    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    const struct block_spec {
        int x;
        int y;
        int width;
        int height;
        uint32_t rgb;
    } blocks[] = {
        {4, 4, 36, 36, 0xFF0000},
        {200, 4, 36, 36, 0x00FF00},
        {4, 200, 36, 36, 0x0000FF},
        {200, 200, 36, 36, 0xFFFFFF},
        {91, 58, 25, 24, 0xFFFF00},
        {108, 42, 28, 156, 0xFFFF00},
        {78, 190, 88, 28, 0xFFFF00},
    };
    for (size_t index = 0U; index < sizeof(blocks) / sizeof(blocks[0]); ++index) {
        esp_err_t error = lvgl_block(screen,
                                     blocks[index].x,
                                     blocks[index].y,
                                     blocks[index].width,
                                     blocks[index].height,
                                     blocks[index].rgb);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

static esp_err_t init_lvgl(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    return lvgl_port_init(&lvgl_cfg);
}

static esp_err_t add_lvgl_display(void)
{
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
            .swap_bytes = false,
        },
    };
    s_display = lvgl_port_add_disp(&display_cfg);
    return s_display == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static int32_t display_port_execute(void *context,
                                    display_port_flow_operation_t operation)
{
    (void)context;
    switch (operation) {
    case DISPLAY_FLOW_BACKLIGHT_INIT_OFF:
        return backlight_init_off();
    case DISPLAY_FLOW_SHARED_SD_CS_HIGH:
        return shared_spi_devices_deselect();
    case DISPLAY_FLOW_CREATE_SEMAPHORE:
        s_transfer_done = xSemaphoreCreateBinary();
        return s_transfer_done == NULL ? ESP_ERR_NO_MEM : ESP_OK;
    case DISPLAY_FLOW_SPI_BUS_INIT: {
        const spi_bus_config_t bus = {
            .sclk_io_num = BOARD_LCD_PIN_SCLK,
            .mosi_io_num = BOARD_LCD_PIN_MOSI,
            .miso_io_num = -1,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = (int)(FRAME_BYTES + 8U),
        };
        return spi_bus_initialize(BOARD_LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    }
    case DISPLAY_FLOW_PANEL_IO_CREATE: {
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
        return esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_config, &s_panel_io);
    }
    case DISPLAY_FLOW_ST7789_CREATE: {
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = BOARD_LCD_PIN_RESET,
            .rgb_ele_order = BOARD_LCD_RGB_ELEMENT_ORDER,
            .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
            .bits_per_pixel = 16,
        };
        return esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel);
    }
    case DISPLAY_FLOW_PANEL_RESET:
        return esp_lcd_panel_reset(s_panel);
    case DISPLAY_FLOW_PANEL_INIT:
        return esp_lcd_panel_init(s_panel);
    case DISPLAY_FLOW_PANEL_SET_GAP:
        return esp_lcd_panel_set_gap(s_panel, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y);
    case DISPLAY_FLOW_PANEL_SWAP_XY:
        return esp_lcd_panel_swap_xy(s_panel, BOARD_LCD_SWAP_XY);
    case DISPLAY_FLOW_PANEL_MIRROR:
        return esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y);
    case DISPLAY_FLOW_PANEL_INVERT:
        return esp_lcd_panel_invert_color(s_panel, BOARD_LCD_INVERT_COLOR);
    case DISPLAY_FLOW_ALLOCATE_FRAME:
        s_frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        return s_frame == NULL ? ESP_ERR_NO_MEM : ESP_OK;
    case DISPLAY_FLOW_PRESENT_BLACK:
        frame_fill(RGB565_BLACK);
        return frame_present();
    case DISPLAY_FLOW_PANEL_DISPLAY_ON:
        return esp_lcd_panel_disp_on_off(s_panel, true);
    case DISPLAY_FLOW_SET_INITIAL_BRIGHTNESS:
        return backlight_set_percent(BOARD_LCD_INITIAL_BRIGHTNESS);
    case DISPLAY_FLOW_PRESENT_RED:
        return show_solid(RGB565_RED);
    case DISPLAY_FLOW_PRESENT_GREEN:
        return show_solid(RGB565_GREEN);
    case DISPLAY_FLOW_PRESENT_BLUE:
        return show_solid(RGB565_BLUE);
    case DISPLAY_FLOW_PRESENT_WHITE:
        return show_solid(RGB565_WHITE);
    case DISPLAY_FLOW_FREE_FRAME:
        heap_caps_free(s_frame);
        s_frame = NULL;
        return ESP_OK;
    case DISPLAY_FLOW_LVGL_INIT:
        return init_lvgl();
    case DISPLAY_FLOW_LVGL_READY_FOR_HANDOFF:
        return ESP_OK;
    case DISPLAY_FLOW_LVGL_ADD_DISPLAY:
        return add_lvgl_display();
    case DISPLAY_FLOW_DELETE_DIRECT_SEMAPHORE:
        vSemaphoreDelete(s_transfer_done);
        s_transfer_done = NULL;
        return ESP_OK;
    case DISPLAY_FLOW_LVGL_LOCK:
        return lvgl_port_lock(0) ? ESP_OK : ESP_ERR_TIMEOUT;
    case DISPLAY_FLOW_LVGL_ROTATE_90:
        lv_display_set_rotation(s_display, LV_DISPLAY_ROTATION_90);
        return ESP_OK;
    case DISPLAY_FLOW_ACTIVE_SCREEN_BLACK: {
        lv_obj_t *screen = lv_display_get_screen_active(s_display);
        lv_obj_remove_style_all(screen);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
        return ESP_OK;
    }
    case DISPLAY_FLOW_ACCEPTANCE_CARD:
        return create_orientation_pattern();
    case DISPLAY_FLOW_LVGL_UNLOCK:
        lvgl_port_unlock();
        return ESP_OK;
    case DISPLAY_FLOW_PUBLISH_DISPLAY:
        return ESP_OK;
    case DISPLAY_FLOW_CLEANUP_BACKLIGHT_OFF_BEST_EFFORT:
        return backlight_set_percent(0);
    case DISPLAY_FLOW_CLEANUP_PANEL_OFF_BEST_EFFORT:
        return esp_lcd_panel_disp_on_off(s_panel, false);
    case DISPLAY_FLOW_CLEANUP_LVGL_UNLOCK:
        lvgl_port_unlock();
        return ESP_OK;
    case DISPLAY_FLOW_CLEANUP_LVGL_DEINIT:
        return lvgl_port_deinit();
    case DISPLAY_FLOW_CLEANUP_FREE_FRAME:
        heap_caps_free(s_frame);
        s_frame = NULL;
        return ESP_OK;
    case DISPLAY_FLOW_CLEANUP_PANEL_DELETE: {
        esp_err_t error = esp_lcd_panel_del(s_panel);
        if (error == ESP_OK) {
            s_panel = NULL;
        }
        return error;
    }
    case DISPLAY_FLOW_CLEANUP_PANEL_IO_DELETE: {
        esp_err_t error = esp_lcd_panel_io_del(s_panel_io);
        if (error == ESP_OK) {
            s_panel_io = NULL;
        }
        return error;
    }
    case DISPLAY_FLOW_CLEANUP_SPI_BUS_FREE:
        return spi_bus_free(BOARD_LCD_HOST);
    case DISPLAY_FLOW_CLEANUP_SEMAPHORE_DELETE:
        vSemaphoreDelete(s_transfer_done);
        s_transfer_done = NULL;
        return ESP_OK;
    case DISPLAY_FLOW_CLEANUP_LEDC_STOP_BEST_EFFORT:
        return ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t display_port_start(display_port_t *port, bool run_acceptance_sequence)
{
    if (port == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != DISPLAY_PORT_NOT_STARTED) {
        return ESP_ERR_INVALID_STATE;
    }
    port->display = NULL;
    s_state = DISPLAY_PORT_STARTING;
    const int32_t result = display_port_flow_run(
        run_acceptance_sequence, display_port_execute, NULL);
    if (result != ESP_OK) {
        s_state = DISPLAY_PORT_FAILED;
        port->display = NULL;
        return (esp_err_t)result;
    }
    port->display = s_display;
    s_state = DISPLAY_PORT_STARTED;
    return ESP_OK;
}

esp_err_t display_port_set_brightness(uint8_t percent)
{
    if (percent > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != DISPLAY_PORT_STARTED) {
        return ESP_ERR_INVALID_STATE;
    }
    return backlight_set_percent(percent);
}
