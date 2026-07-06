#pragma once

/*
 * 应用公共配置定义。
 *
 * 这个模块只负责“内存中的配置结构”和“NVS 持久化接口”的定义。
 * Wi-Fi 连接、BLE 广播、Mesh 控制都不应该直接操作 NVS，统一通过 app_config.c 读写。
 */

#include <stdbool.h>

#define WIFI_SSID_MAX_LEN      32
#define WIFI_PASS_MAX_LEN      64
#define BLE_NAME_MAX_LEN       32
#define AP_NAME_MAX_LEN        32
#define AP_PASS_MAX_LEN        64
#define WIFI_PROFILE_MAX_NUM    5

typedef struct {
    /* 单个 STA Wi-Fi 配置。当前最多保存 WIFI_PROFILE_MAX_NUM 组。 */
    char wifi_ssid[WIFI_SSID_MAX_LEN + 1];
    char wifi_pass[WIFI_PASS_MAX_LEN + 1];
    bool valid;
} wifi_profile_t;

typedef struct {
    /* STA 可连接 Wi-Fi 列表，wifi_manager 会扫描环境并优先连接信号最强的已保存 SSID。 */
    wifi_profile_t wifi_profiles[WIFI_PROFILE_MAX_NUM];
    int wifi_count;

    /* 用户显示名。当前 BLE 扫描名固定为 BMSxxxxxx，这个名字主要留给 App 查询显示。 */
    char ble_name[BLE_NAME_MAX_LEN + 1];

    /* SoftAP 热点名称和密码。AP 密码小于 8 位时 ESP-IDF 会按开放热点处理。 */
    char ap_name[AP_NAME_MAX_LEN + 1];
    char ap_pass[AP_PASS_MAX_LEN + 1];

    /* has_* 用于区分“用户确实配置过”和“当前只是默认值”。 */
    bool has_wifi;
    bool has_ble_name;
    bool has_ap_name;
    bool has_ap_pass;
} app_config_t;

/* 从 NVS 读取应用配置；如果没有有效配置，则先填默认值。 */
void app_config_load(app_config_t *cfg);

/* 将当前配置完整保存到 NVS。 */
void app_config_save(const app_config_t *cfg);

/* 生成一份默认配置到内存，不访问 NVS。 */
void app_config_set_default(app_config_t *cfg);

/* 新增或更新一条 STA Wi-Fi 配置；同名 SSID 会直接更新密码。 */
bool app_config_add_or_update_wifi(app_config_t *cfg, const char *ssid, const char *pass);

/* 清空整个应用配置命名空间。 */
void app_config_erase_all(void);

/* 仅清空 STA Wi-Fi 配置，不影响 BLE 名称和 AP 配置。 */
void app_config_erase_wifi(void);
