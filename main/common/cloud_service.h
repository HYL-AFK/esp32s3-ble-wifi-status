#pragma once

/*
 * 云端信息服务模块。
 *
 * 作用：
 * - 在 STA 联网成功后获取公网 IP
 * - 根据公网 IP 获取地理位置和时区偏移
 * - 根据经纬度获取天气
 * - 通过 SNTP 维护系统 UTC 时间
 * - 向 BLE、UI 等上层提供统一状态快照
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define CLOUD_PUBLIC_IP_MAX_LEN   48
#define CLOUD_TEXT_SHORT_MAX_LEN  32
#define CLOUD_TEXT_MID_MAX_LEN    48
#define CLOUD_ERROR_MAX_LEN       96

typedef struct {
    bool sta_connected;
    bool internet_ok;
    bool time_valid;
    bool public_ip_valid;
    bool geo_valid;
    bool weather_valid;
    bool update_in_progress;

    char sta_ip[16];
    char public_ip[CLOUD_PUBLIC_IP_MAX_LEN];
    char country[CLOUD_TEXT_SHORT_MAX_LEN];
    char region[CLOUD_TEXT_SHORT_MAX_LEN];
    char city[CLOUD_TEXT_SHORT_MAX_LEN];
    char district[CLOUD_TEXT_SHORT_MAX_LEN];
    char timezone[CLOUD_TEXT_MID_MAX_LEN];
    char local_time_str[24];
    char last_error[CLOUD_ERROR_MAX_LEN];

    int internet_fail_count;
    int utc_offset_seconds;
    double latitude;
    double longitude;

    float temperature_c;
    float wind_speed_kmh;
    int humidity;
    int weather_code;

    time_t utc_epoch;
    time_t local_epoch;
    time_t last_geo_refresh;
    time_t last_weather_refresh;
    time_t last_time_sync;
} cloud_service_snapshot_t;

/* 初始化云端状态服务任务和内部快照。 */
esp_err_t cloud_service_init(void);

/* 通知云端服务当前 Wi-Fi 状态已变化，用于触发自动刷新判断。 */
void cloud_service_notify_wifi_state(void);

/* 手动请求一次云端刷新。 */
void cloud_service_request_refresh(void);

/* 读取当前云端状态快照。 */
void cloud_service_get_snapshot(cloud_service_snapshot_t *out);

/* 将当前快照格式化为 JSON，供 BLE STATUS/QUERY_STATUS 回读。 */
int cloud_service_format_status_json(char *out, size_t out_size);

/* 将 Open-Meteo weather_code 映射成适合小屏显示的英文短词。 */
const char *cloud_service_weather_text(int weather_code);
