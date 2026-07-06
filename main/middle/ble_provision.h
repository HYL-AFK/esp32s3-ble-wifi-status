#pragma once

/*
 * BLE 配网接口。
 *
 * 本模块基于 NimBLE GATT：
 * - 上电后启动 BLE 广播。
 * - 手机连接后向 FFF1 写入配置命令。
 * - 设备通过 FFF2 Notify 返回 ACK/ERR 或状态数据。
 *
 * 注意：当前 BLE 扫描名固定为 BMS + BLE MAC 后 3 字节。
 * NAME 命令保存的是“用户显示名”，不是扫描广播名。
 */

#include <stdint.h>

#include "app_config.h"

/* BLE 收到配置并保存后，通过这个回调通知 main.c 重新应用 Wi-Fi/AP 配置。 */
typedef void (*ble_cfg_apply_cb_t)(const app_config_t *cfg);

void ble_provision_init(const app_config_t *cfg, ble_cfg_apply_cb_t apply_cb);

/* Wi-Fi 状态变化时调用，用于刷新广播 Manufacturer Data 中的状态位。 */
void ble_provision_refresh_advertising(void);

/* 提供给 UI 层查询当前 BLE 运行状态。 */
bool ble_provision_is_ready(void);
bool ble_provision_is_advertising(void);
bool ble_provision_is_connected(void);
bool ble_provision_is_notify_enabled(void);

/* 当前连接句柄；未连接时通常为 0xFFFF。 */
uint16_t ble_provision_get_conn_handle(void);

/* 当前广播名，固定为 BMS + BLE MAC 后 3 字节。 */
const char *ble_provision_get_adv_name(void);
