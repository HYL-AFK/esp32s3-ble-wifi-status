#include "display_st7789.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/*
 * ST7789 SPI LCD 驱动。
 *
 * 按你这块 ESP32-S3FH4R2 小板和 2.0 寸 ST7789 模块，默认接线如下：
 * - SCL -> GPIO12，SPI SCLK。
 * - SDA -> GPIO11，SPI MOSI。
 * - SDO -> GPIO10，SPI MISO，字库芯片读取字模时返回数据。
 * - RST -> GPIO13，屏幕复位。
 * - DC  -> GPIO9，命令/数据选择。
 * - CS  -> GPIO8，ST7789 屏幕片选 LCD_CS。
 * - CSF -> GPIO7，板载 GB2312 字库芯片片选 FONT_CS。
 * - BL  -> 3V3，默认不占 GPIO；如果要软件调光再改 CONFIG_DISPLAY_ST7789_BL_GPIO。
 */

static const char *TAG = "display_st7789";

#if CONFIG_DISPLAY_ST7789_ENABLE

#define LCD_HOST            SPI2_HOST
#define LCD_CMD_BITS        8
#define LCD_PARAM_BITS      8
#define LCD_BITS_PER_PIXEL  16

#define COLOR_BLACK         0x0000
#define COLOR_WHITE         0xFFFF
#define COLOR_RED           0xF800
#define COLOR_GREEN         0x07E0
#define COLOR_BLUE          0x001F
#define COLOR_YELLOW        0xFFE0
#define COLOR_CYAN          0x07FF
#define COLOR_MAGENTA       0xF81F
#define STARTUP_BG          0x0000

#ifdef CONFIG_DISPLAY_ST7789_BGR
#define DISPLAY_ST7789_BGR_ENABLED true
#else
#define DISPLAY_ST7789_BGR_ENABLED false
#endif

#ifdef CONFIG_DISPLAY_ST7789_INVERT_COLOR
#define DISPLAY_ST7789_INVERT_ENABLED true
#else
#define DISPLAY_ST7789_INVERT_ENABLED false
#endif

#ifdef CONFIG_DISPLAY_ST7789_SWAP_XY
#define DISPLAY_ST7789_SWAP_XY_ENABLED true
#else
#define DISPLAY_ST7789_SWAP_XY_ENABLED false
#endif

#ifdef CONFIG_DISPLAY_ST7789_MIRROR_X
#define DISPLAY_ST7789_MIRROR_X_ENABLED true
#else
#define DISPLAY_ST7789_MIRROR_X_ENABLED false
#endif

#ifdef CONFIG_DISPLAY_ST7789_MIRROR_Y
#define DISPLAY_ST7789_MIRROR_Y_ENABLED true
#else
#define DISPLAY_ST7789_MIRROR_Y_ENABLED false
#endif

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io_handle;
static bool s_display_ready;

typedef struct {
    uint8_t gb_high;
    uint8_t gb_low;
    uint8_t bitmap[32];
} fallback_hz16_t;

/*
 * 内置 16x16 中文兜底字模。
 *
 * 用途：
 * - 当前屏幕模块的板载 GB2312 字库芯片已经能读出数据，但地址表和代码里的默认 base address
 *   还没有完全对齐，所以会出现“有中文形状，但不是目标文字”的情况。
 * - 启动页只需要固定显示“中文宋体测试”，先用这几个内置字模保证调试界面可读。
 * - 后续拿到字库芯片型号/地址表后，只需要校准 font_gb2312.c 的地址计算，业务显示仍走
 *   display_st7789_draw_string_gb2312() 这个统一接口。
 */
static const fallback_hz16_t s_fallback_hz16[] = {
    /* 中 D6D0 */
    {0xD6, 0xD0, {0x01,0x00,0x01,0x00,0x01,0x00,0x7F,0xFC,0x41,0x04,0x41,0x04,0x41,0x04,0x41,0x04,
                  0x41,0x04,0x41,0x04,0x7F,0xFC,0x01,0x00,0x01,0x00,0x01,0x00,0x01,0x00,0x00,0x00}},
    /* 文 CEC4 */
    {0xCE, 0xC4, {0x01,0x00,0x01,0x00,0x7F,0xFE,0x02,0x00,0x02,0x00,0x05,0x00,0x09,0x00,0x10,0x80,
                  0x20,0x40,0x40,0x20,0x20,0x40,0x10,0x80,0x09,0x00,0x06,0x00,0x19,0x80,0x60,0x60}},
    /* 宋 CBCE */
    {0xCB, 0xCE, {0x02,0x00,0x01,0x00,0x7F,0xFE,0x40,0x02,0x80,0x04,0x1F,0xF0,0x01,0x00,0x01,0x00,
                  0x7F,0xFC,0x03,0x80,0x05,0x40,0x09,0x20,0x11,0x10,0x21,0x08,0x41,0x04,0x00,0x00}},
    /* 体 CCE5 */
    {0xCC, 0xE5, {0x08,0x80,0x08,0x80,0x10,0x80,0x10,0x80,0x20,0x80,0x47,0xFC,0x88,0x80,0x10,0x80,
                  0x11,0x80,0x12,0xA0,0x14,0x90,0x18,0x88,0x10,0x84,0x10,0x80,0x10,0x80,0x00,0x00}},
    /* 测 B2E2 */
    {0xB2, 0xE2, {0x20,0x10,0x10,0x10,0x00,0x10,0x47,0xD0,0x34,0x50,0x14,0x50,0x84,0x50,0x64,0x50,
                  0x24,0x50,0x24,0x50,0x24,0x50,0x27,0xD0,0x20,0x10,0x22,0x10,0x21,0xF0,0x00,0x00}},
    /* 试 CAD4 */
    {0xCA, 0xD4, {0x20,0x40,0x10,0x40,0x00,0x40,0x7C,0x40,0x04,0x7C,0x04,0x40,0x04,0x40,0x04,0x40,
                  0x7C,0x40,0x04,0x44,0x04,0x48,0x04,0x50,0x04,0x60,0x1C,0x42,0xE4,0x3E,0x00,0x00}},
};

static const uint8_t *find_fallback_hz16(uint8_t gb_high, uint8_t gb_low)
{
    for (size_t i = 0; i < sizeof(s_fallback_hz16) / sizeof(s_fallback_hz16[0]); ++i) {
        if (s_fallback_hz16[i].gb_high == gb_high && s_fallback_hz16[i].gb_low == gb_low) {
            return s_fallback_hz16[i].bitmap;
        }
    }
    return NULL;
}

/*
 * 内置 5x7 ASCII 字库，只用于英文、数字和调试字符。
 * 中文走板载 GB2312 字库芯片，不把完整中文点阵塞进固件。
 */
static const uint8_t s_ascii_5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x10,0x08,0x08,0x10,0x08}, {0x00,0x00,0x00,0x00,0x00},
};

/*
 * 自定义 7 段数字字库，只包含 0-9。
 * 每个数字用 7 个段组合出来，不依赖 ROM、不依赖外部 GB2312 字库。
 */
static esp_err_t init_backlight(void)
{
#if CONFIG_DISPLAY_ST7789_BL_GPIO >= 0
    gpio_config_t bk_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_DISPLAY_ST7789_BL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_cfg), TAG, "config backlight gpio failed");
    gpio_set_level(CONFIG_DISPLAY_ST7789_BL_GPIO, CONFIG_DISPLAY_ST7789_BL_OFF_LEVEL);
#endif
    return ESP_OK;
}

static void set_backlight(bool on)
{
#if CONFIG_DISPLAY_ST7789_BL_GPIO >= 0
    gpio_set_level(CONFIG_DISPLAY_ST7789_BL_GPIO,
                   on ? CONFIG_DISPLAY_ST7789_BL_ON_LEVEL : CONFIG_DISPLAY_ST7789_BL_OFF_LEVEL);
#else
    (void)on;
#endif
}

esp_err_t display_st7789_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    ESP_RETURN_ON_FALSE(s_display_ready && s_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "display not ready");
    ESP_RETURN_ON_FALSE(w > 0 && h > 0, ESP_ERR_INVALID_ARG, TAG, "bad rect size");

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x >= CONFIG_DISPLAY_ST7789_H_RES || y >= CONFIG_DISPLAY_ST7789_V_RES || w <= 0 || h <= 0) {
        return ESP_OK;
    }
    if (x + w > CONFIG_DISPLAY_ST7789_H_RES) {
        w = CONFIG_DISPLAY_ST7789_H_RES - x;
    }
    if (y + h > CONFIG_DISPLAY_ST7789_V_RES) {
        h = CONFIG_DISPLAY_ST7789_V_RES - y;
    }

    /*
     * 按多行分块刷屏，减少 SPI 事务数量。
     * max_transfer_sz 在初始化 SPI 总线时按 40 行设置，这里保持一致。
     */
    const int max_chunk_h = 40;
    int chunk_h = (h > max_chunk_h) ? max_chunk_h : h;
    uint16_t *chunk = heap_caps_malloc((size_t)w * chunk_h * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(chunk != NULL, ESP_ERR_NO_MEM, TAG, "alloc rect chunk failed");

    for (int i = 0; i < w * chunk_h; ++i) {
        chunk[i] = color;
    }

    int drawn = 0;
    while (drawn < h) {
        int this_h = h - drawn;
        if (this_h > chunk_h) {
            this_h = chunk_h;
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y + drawn, x + w, y + drawn + this_h, chunk);
        if (err != ESP_OK) {
            free(chunk);
            return err;
        }
        drawn += this_h;
    }

    free(chunk);
    return ESP_OK;
}

esp_err_t display_st7789_clear(uint16_t color)
{
    return display_st7789_fill_rect(0, 0, CONFIG_DISPLAY_ST7789_H_RES, CONFIG_DISPLAY_ST7789_V_RES, color);
}

static esp_err_t draw_ascii_char(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) {
        scale = 1;
    }

    if (ch < 0x20 || ch > 0x7F) {
        ch = '?';
    }

    int cell_w = 6 * scale;
    int cell_h = 8 * scale;
    uint16_t *cell = heap_caps_malloc((size_t)cell_w * cell_h * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(cell != NULL, ESP_ERR_NO_MEM, TAG, "alloc ascii cell failed");

    for (int i = 0; i < cell_w * cell_h; ++i) {
        cell[i] = bg;
    }

    const uint8_t *glyph = s_ascii_5x7[(uint8_t)ch - 0x20];
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if ((bits & (1U << row)) != 0) {
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        int px = col * scale + sx;
                        int py = row * scale + sy;
                        cell[py * cell_w + px] = fg;
                    }
                }
            }
        }
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + cell_w, y + cell_h, cell);
    free(cell);
    return err;
}

esp_err_t display_st7789_draw_string_ascii(int x,
                                           int y,
                                           const char *text,
                                           uint16_t fg,
                                           uint16_t bg,
                                           int scale)
{
    ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, TAG, "ascii text is null");

    int cursor_x = x;
    while (*text != '\0') {
        if (*text == '\n') {
            y += 9 * scale;
            cursor_x = x;
        } else {
            ESP_RETURN_ON_ERROR(draw_ascii_char(cursor_x, y, *text, fg, bg, scale), TAG, "draw ascii char failed");
            cursor_x += 6 * scale;
        }
        ++text;
    }

    return ESP_OK;
}

static esp_err_t draw_gb2312_glyph(int x,
                                   int y,
                                   uint8_t gb_high,
                                   uint8_t gb_low,
                                   font_gb2312_size_t size,
                                   uint16_t fg,
                                   uint16_t bg)
{
    size_t glyph_size = font_gb2312_glyph_bytes(size);
    ESP_RETURN_ON_FALSE(glyph_size > 0 && glyph_size <= 128, ESP_ERR_INVALID_ARG, TAG, "bad gb2312 glyph size");

    uint8_t bitmap[128];
    const uint8_t *fallback = (size == FONT_GB2312_SIZE_16) ? find_fallback_hz16(gb_high, gb_low) : NULL;
    if (fallback != NULL) {
        memcpy(bitmap, fallback, 32);
    } else {
        ESP_RETURN_ON_ERROR(font_gb2312_read_glyph(gb_high, gb_low, size, bitmap, sizeof(bitmap)),
                            TAG,
                            "read gb2312 glyph failed");
    }

    uint16_t *cell = heap_caps_malloc((size_t)size * size * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(cell != NULL, ESP_ERR_NO_MEM, TAG, "alloc gb2312 cell failed");
    for (int i = 0; i < size * size; ++i) {
        cell[i] = bg;
    }

    int bytes_per_row = (size + 7) / 8;
    for (int row = 0; row < size; ++row) {
        for (int col = 0; col < size; ++col) {
            uint8_t byte = bitmap[row * bytes_per_row + col / 8];
            uint8_t mask = (uint8_t)(0x80U >> (col % 8));
            if ((byte & mask) != 0) {
                cell[row * size + col] = fg;
            }
        }
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + size, y + size, cell);
    free(cell);
    return err;
}

esp_err_t display_st7789_draw_string_gb2312(int x,
                                            int y,
                                            const uint8_t *gb2312,
                                            font_gb2312_size_t size,
                                            uint16_t fg,
                                            uint16_t bg)
{
    ESP_RETURN_ON_FALSE(gb2312 != NULL, ESP_ERR_INVALID_ARG, TAG, "gb2312 text is null");

    int cursor_x = x;
    while (*gb2312 != '\0') {
        if (*gb2312 < 0x80) {
            ESP_RETURN_ON_ERROR(draw_ascii_char(cursor_x, y, (char)*gb2312, fg, bg, 2),
                                TAG, "draw mixed ascii failed");
            cursor_x += 12;
            gb2312 += 1;
        } else {
            ESP_RETURN_ON_FALSE(gb2312[1] != '\0', ESP_ERR_INVALID_ARG, TAG, "truncated gb2312 text");
            ESP_RETURN_ON_ERROR(draw_gb2312_glyph(cursor_x, y, gb2312[0], gb2312[1], size, fg, bg),
                                TAG, "draw gb2312 glyph failed");
            cursor_x += size;
            gb2312 += 2;
        }
    }

    return ESP_OK;
}

static esp_err_t draw_color_bars(void) __attribute__((unused));
static esp_err_t draw_color_bars(void)
{
    const int width = CONFIG_DISPLAY_ST7789_H_RES;
    const int height = CONFIG_DISPLAY_ST7789_V_RES;
    const uint16_t colors[] = {
        COLOR_RED,
        COLOR_GREEN,
        COLOR_BLUE,
        COLOR_YELLOW,
        COLOR_MAGENTA,
        COLOR_CYAN,
        COLOR_WHITE,
        COLOR_BLACK,
    };
    const int color_count = sizeof(colors) / sizeof(colors[0]);
    const int bar_h = height / color_count;

    for (int i = 0; i < color_count; ++i) {
        int y = i * bar_h;
        int h = (i == color_count - 1) ? (height - y) : bar_h;
        ESP_RETURN_ON_ERROR(display_st7789_fill_rect(0, y, width, h, colors[i]), TAG, "draw color bar failed");
    }

    return ESP_OK;
}

esp_err_t display_st7789_show_startup(void)
{
    /*
     * 当前阶段只验证“自定义数字字库 0-9”。
     * 不显示中文，不读取板载 GB2312 字库，避免字库地址表未校准造成乱码。
     */
    return display_st7789_clear(STARTUP_BG);
}

esp_err_t display_st7789_init(void)
{
    ESP_LOGI(TAG, "init ST7789 SPI display");
    ESP_LOGI(TAG, "pins: SCLK=%d MOSI=%d MISO=%d LCD_CS=%d DC=%d RST=%d BL=%d FONT_CSF=%d",
             CONFIG_DISPLAY_ST7789_SCLK_GPIO,
             CONFIG_DISPLAY_ST7789_MOSI_GPIO,
             CONFIG_DISPLAY_ST7789_MISO_GPIO,
             CONFIG_DISPLAY_ST7789_CS_GPIO,
             CONFIG_DISPLAY_ST7789_DC_GPIO,
             CONFIG_DISPLAY_ST7789_RST_GPIO,
             CONFIG_DISPLAY_ST7789_BL_GPIO,
             CONFIG_FONT_GB2312_CSF_GPIO);

    ESP_RETURN_ON_ERROR(init_backlight(), TAG, "init backlight failed");
    set_backlight(false);

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = CONFIG_DISPLAY_ST7789_SCLK_GPIO,
        .mosi_io_num = CONFIG_DISPLAY_ST7789_MOSI_GPIO,
        .miso_io_num = CONFIG_DISPLAY_ST7789_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CONFIG_DISPLAY_ST7789_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "init spi bus failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_DISPLAY_ST7789_DC_GPIO,
        .cs_gpio_num = CONFIG_DISPLAY_ST7789_CS_GPIO,
        .pclk_hz = CONFIG_DISPLAY_ST7789_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle),
                        TAG, "create lcd panel io failed");
    s_io_handle = io_handle;

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_DISPLAY_ST7789_RST_GPIO,
        .rgb_ele_order = DISPLAY_ST7789_BGR_ENABLED ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel), TAG, "create st7789 panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, DISPLAY_ST7789_INVERT_ENABLED), TAG, "panel invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, DISPLAY_ST7789_SWAP_XY_ENABLED), TAG, "panel swap xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel,
                                             DISPLAY_ST7789_MIRROR_X_ENABLED,
                                             DISPLAY_ST7789_MIRROR_Y_ENABLED),
                        TAG, "panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel,
                                              CONFIG_DISPLAY_ST7789_X_GAP,
                                              CONFIG_DISPLAY_ST7789_Y_GAP),
                        TAG, "panel gap failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");
    s_display_ready = true;
    set_backlight(true);

    ESP_RETURN_ON_ERROR(display_st7789_show_startup(), TAG, "show startup failed");
    ESP_LOGI(TAG, "ST7789 display init done");
    return ESP_OK;
}

bool display_st7789_is_ready(void)
{
    return s_display_ready && s_panel != NULL && s_io_handle != NULL;
}

esp_lcd_panel_io_handle_t display_st7789_get_io_handle(void)
{
    return s_io_handle;
}

esp_lcd_panel_handle_t display_st7789_get_panel_handle(void)
{
    return s_panel;
}

#else

esp_err_t display_st7789_init(void)
{
    return ESP_OK;
}

esp_err_t display_st7789_clear(uint16_t color)
{
    (void)color;
    return ESP_OK;
}

esp_err_t display_st7789_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
    return ESP_OK;
}

esp_err_t display_st7789_draw_string_ascii(int x,
                                           int y,
                                           const char *text,
                                           uint16_t fg,
                                           uint16_t bg,
                                           int scale)
{
    (void)x;
    (void)y;
    (void)text;
    (void)fg;
    (void)bg;
    (void)scale;
    return ESP_OK;
}

esp_err_t display_st7789_draw_string_gb2312(int x,
                                            int y,
                                            const uint8_t *gb2312,
                                            font_gb2312_size_t size,
                                            uint16_t fg,
                                            uint16_t bg)
{
    (void)x;
    (void)y;
    (void)gb2312;
    (void)size;
    (void)fg;
    (void)bg;
    return ESP_OK;
}

esp_err_t display_st7789_show_startup(void)
{
    return ESP_OK;
}

#endif
