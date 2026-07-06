#pragma once

/*
 * 屏幕状态面板。
 * 这一层只负责“把 Wi-Fi / BLE 状态画出来”，不处理连接逻辑。
 */

#include "esp_err.h"

esp_err_t ui_status_init(void);
void ui_status_refresh(void);

