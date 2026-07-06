#pragma once

/*
 * 板载 GB2312 宋体字库芯片驱动接口。
 *
 * 这类 2.0 寸 ST7789 SPI 模块常见做法是：
 * - ST7789 屏幕控制器和字库芯片共用 SCL/SDA 这条 SPI 总线。
 * - 屏幕使用一个片选脚，例如 SDC/LCD_CS。
 * - 字库芯片使用另一个片选脚，例如 CSF/FONT_CS。
 *
 * 本模块只负责“按 GB2312 编码和字号读取字模点阵”，不负责把点阵画到屏幕。
 */

#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

typedef enum {
    FONT_GB2312_SIZE_12 = 12,
    FONT_GB2312_SIZE_14 = 14,
    FONT_GB2312_SIZE_16 = 16,
    FONT_GB2312_SIZE_18 = 18,
    FONT_GB2312_SIZE_20 = 20,
    FONT_GB2312_SIZE_22 = 22,
    FONT_GB2312_SIZE_24 = 24,
    FONT_GB2312_SIZE_26 = 26,
} font_gb2312_size_t;

esp_err_t font_gb2312_init(spi_host_device_t host);

size_t font_gb2312_glyph_bytes(font_gb2312_size_t size);

esp_err_t font_gb2312_read_glyph(uint8_t gb_high,
                                 uint8_t gb_low,
                                 font_gb2312_size_t size,
                                 uint8_t *out_bitmap,
                                 size_t out_size);
