#include "font_gb2312.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

/*
 * GB2312 字库芯片读取实现。
 *
 * 当前实现假设字库芯片支持常见 SPI Flash 读命令：
 *   0x03 + 24bit address + data...
 *
 * 字库地址按“区位码顺序连续存放”计算：
 *   index = (区号 - 1) * 94 + (位号 - 1)
 *   address = 字号基地址 + index * 单字字模字节数
 *
 * 如果你手上的字库芯片手册给出了不同的地址公式，只需要修改
 * font_gb2312_glyph_address() 或 menuconfig 里的各字号 base address。
 */

static const char *TAG = "font_gb2312";

#if CONFIG_FONT_GB2312_ENABLE

static spi_device_handle_t s_font_spi;

typedef struct {
    font_gb2312_size_t size;
    uint32_t base_addr;
} font_table_t;

static const font_table_t s_font_tables[] = {
    {FONT_GB2312_SIZE_12, CONFIG_FONT_GB2312_BASE_12},
    {FONT_GB2312_SIZE_14, CONFIG_FONT_GB2312_BASE_14},
    {FONT_GB2312_SIZE_16, CONFIG_FONT_GB2312_BASE_16},
    {FONT_GB2312_SIZE_18, CONFIG_FONT_GB2312_BASE_18},
    {FONT_GB2312_SIZE_20, CONFIG_FONT_GB2312_BASE_20},
    {FONT_GB2312_SIZE_22, CONFIG_FONT_GB2312_BASE_22},
    {FONT_GB2312_SIZE_24, CONFIG_FONT_GB2312_BASE_24},
    {FONT_GB2312_SIZE_26, CONFIG_FONT_GB2312_BASE_26},
};

static bool font_size_valid(font_gb2312_size_t size)
{
    for (size_t i = 0; i < sizeof(s_font_tables) / sizeof(s_font_tables[0]); ++i) {
        if (s_font_tables[i].size == size) {
            return true;
        }
    }
    return false;
}

static uint32_t font_base_addr(font_gb2312_size_t size)
{
    for (size_t i = 0; i < sizeof(s_font_tables) / sizeof(s_font_tables[0]); ++i) {
        if (s_font_tables[i].size == size) {
            return s_font_tables[i].base_addr;
        }
    }
    return 0;
}

size_t font_gb2312_glyph_bytes(font_gb2312_size_t size)
{
    if (!font_size_valid(size)) {
        return 0;
    }

    /*
     * 单个汉字点阵按“每行向上取整到整字节”存放。
     * 例如 16x16 是 2 * 16 = 32 字节，18x18 是 3 * 18 = 54 字节。
     */
    return ((size + 7) / 8) * size;
}

static esp_err_t font_gb2312_glyph_address(uint8_t gb_high,
                                           uint8_t gb_low,
                                           font_gb2312_size_t size,
                                           uint32_t *out_addr)
{
    ESP_RETURN_ON_FALSE(out_addr != NULL, ESP_ERR_INVALID_ARG, TAG, "address output is null");
    ESP_RETURN_ON_FALSE(font_size_valid(size), ESP_ERR_INVALID_ARG, TAG, "unsupported font size");

    /*
     * GB2312 汉字编码范围一般是 A1A1-FEFE。
     * 这里按区位码 94x94 表计算，符号区和汉字区都可以读。
     */
    ESP_RETURN_ON_FALSE(gb_high >= 0xA1 && gb_high <= 0xFE, ESP_ERR_INVALID_ARG, TAG, "bad gb high");
    ESP_RETURN_ON_FALSE(gb_low >= 0xA1 && gb_low <= 0xFE, ESP_ERR_INVALID_ARG, TAG, "bad gb low");

    uint32_t zone = (uint32_t)(gb_high - 0xA1);
    uint32_t pos = (uint32_t)(gb_low - 0xA1);
    uint32_t index = zone * 94U + pos;
    *out_addr = font_base_addr(size) + index * font_gb2312_glyph_bytes(size);
    return ESP_OK;
}

esp_err_t font_gb2312_init(spi_host_device_t host)
{
    if (s_font_spi != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "init GB2312 font chip, CSF GPIO=%d", CONFIG_FONT_GB2312_CSF_GPIO);

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = CONFIG_FONT_GB2312_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_FONT_GB2312_CSF_GPIO,
        .queue_size = 1,
    };

    ESP_RETURN_ON_ERROR(spi_bus_add_device(host, &dev_cfg, &s_font_spi), TAG, "add font spi device failed");
    return ESP_OK;
}

esp_err_t font_gb2312_read_glyph(uint8_t gb_high,
                                 uint8_t gb_low,
                                 font_gb2312_size_t size,
                                 uint8_t *out_bitmap,
                                 size_t out_size)
{
    ESP_RETURN_ON_FALSE(s_font_spi != NULL, ESP_ERR_INVALID_STATE, TAG, "font chip is not initialized");
    ESP_RETURN_ON_FALSE(out_bitmap != NULL, ESP_ERR_INVALID_ARG, TAG, "bitmap output is null");

    size_t glyph_size = font_gb2312_glyph_bytes(size);
    ESP_RETURN_ON_FALSE(glyph_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid glyph size");
    ESP_RETURN_ON_FALSE(out_size >= glyph_size, ESP_ERR_INVALID_SIZE, TAG, "bitmap buffer too small");

    uint32_t addr = 0;
    ESP_RETURN_ON_ERROR(font_gb2312_glyph_address(gb_high, gb_low, size, &addr), TAG, "calc glyph address failed");

    uint8_t *tx = heap_caps_calloc(1, glyph_size + 4, MALLOC_CAP_DMA);
    uint8_t *rx = heap_caps_calloc(1, glyph_size + 4, MALLOC_CAP_DMA);
    if (tx == NULL || rx == NULL) {
        free(tx);
        free(rx);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "alloc font spi buffer failed");
    }

    tx[0] = 0x03;
    tx[1] = (uint8_t)((addr >> 16) & 0xFF);
    tx[2] = (uint8_t)((addr >> 8) & 0xFF);
    tx[3] = (uint8_t)(addr & 0xFF);

    /*
     * ESP-IDF SPI 是全双工传输。字库芯片在收到 4 字节读命令和地址后，
     * 后续时钟才会吐出字模数据，所以这里发送 4 + glyph_size 字节，
     * 最后从 rx[4] 开始拷贝有效字模。
     */
    spi_transaction_t trans = {
        .length = (glyph_size + 4) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t err = spi_device_transmit(s_font_spi, &trans);
    if (err == ESP_OK) {
        memcpy(out_bitmap, rx + 4, glyph_size);
    }

    free(tx);
    free(rx);
    ESP_RETURN_ON_ERROR(err, TAG, "read glyph failed");
    return ESP_OK;
}

#else

esp_err_t font_gb2312_init(spi_host_device_t host)
{
    (void)host;
    return ESP_OK;
}

size_t font_gb2312_glyph_bytes(font_gb2312_size_t size)
{
    return ((size + 7) / 8) * size;
}

esp_err_t font_gb2312_read_glyph(uint8_t gb_high,
                                 uint8_t gb_low,
                                 font_gb2312_size_t size,
                                 uint8_t *out_bitmap,
                                 size_t out_size)
{
    (void)gb_high;
    (void)gb_low;
    (void)size;
    (void)out_bitmap;
    (void)out_size;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
