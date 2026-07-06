#pragma once

/*
 * ST7789 SPI 屏幕驱动接口。
 *
 * 这个模块负责：
 * - 初始化 ST7789 屏幕。
 * - 提供基础绘图能力。
 * - 通过板载 GB2312 字库芯片显示宋体中文。
 *
 * 后续如果接 LVGL 或做复杂 UI，可以在 app/middle 层继续封装，
 * 不需要把业务逻辑写进这个底层驱动。
 */

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "font_gb2312.h"

esp_err_t display_st7789_init(void);
bool display_st7789_is_ready(void);

/* 整屏清空为指定 RGB565 颜色。 */
esp_err_t display_st7789_clear(uint16_t color);

/* 填充矩形区域，坐标和尺寸基于当前屏幕方向。 */
esp_err_t display_st7789_fill_rect(int x, int y, int w, int h, uint16_t color);

/* 使用内置 ASCII 字库绘制英文/数字。 */
esp_err_t display_st7789_draw_string_ascii(int x,
                                           int y,
                                           const char *text,
                                           uint16_t fg,
                                           uint16_t bg,
                                           int scale);

/* 使用板载 GB2312 字库芯片绘制中文。 */
esp_err_t display_st7789_draw_string_gb2312(int x,
                                            int y,
                                            const uint8_t *gb2312,
                                            font_gb2312_size_t size,
                                            uint16_t fg,
                                            uint16_t bg);

/* 启动页显示入口。当前实现主要用于清屏和过渡。 */
esp_err_t display_st7789_show_startup(void);

/* 提供给 LVGL 绑定底层面板句柄。 */
esp_lcd_panel_io_handle_t display_st7789_get_io_handle(void);
esp_lcd_panel_handle_t display_st7789_get_panel_handle(void);
