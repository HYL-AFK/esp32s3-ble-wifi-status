#include "cloud_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "app_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif_sntp.h"
#include "lwip/errno.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
/*
 * 某些 IDF 环境里，esp_crt_bundle.h 头文件不会自动暴露给当前组件，
 * 但证书绑定符号本身仍然由 mbedtls 组件提供。这里做前置声明，避免
 * 因为头文件搜索路径问题导致 cloud_service.c 直接编译失败。
 */
esp_err_t esp_crt_bundle_attach(void *conf);
#endif

/*
 * 方案 A 实现策略：
 * - 公网 IP：api.ipify.org
 * - 地理位置：ipapi.co
 * - 天气：open-meteo.com
 * - 时间：SNTP + 本地时区换算
 *
 * 刷新策略：
 * - STA got IP 后自动触发一次刷新
 * - 位置：仅在联网成功、IP 变化、手动触发时刷新
 * - 天气：周期刷新，默认 15 分钟
 * - 时间：SNTP 本地维护
 */

static const char *TAG = "cloud_service";

#define CLOUD_TASK_STACK_SIZE 8192
#define CLOUD_TASK_PRIORITY 4
#define CLOUD_EVT_REFRESH BIT0
#define CLOUD_EVT_WIFI_CHANGED BIT1
#define CLOUD_HTTP_BUFFER_SIZE 1024
#define CLOUD_WEATHER_REFRESH_SEC (15 * 60)
#define CLOUD_SNTP_WAIT_MS 15000
#define CLOUD_STATUS_JSON_MAX_LEN 512
#define CLOUD_INTERNET_RETRY_1_SEC 30
#define CLOUD_INTERNET_RETRY_2_SEC 60
#define CLOUD_INTERNET_RETRY_3_SEC 120
#define CLOUD_INTERNET_RECONNECT_THRESHOLD 3

static EventGroupHandle_t s_evt_group;
static TaskHandle_t s_task_handle;
static SemaphoreHandle_t s_snapshot_mutex;
static cloud_service_snapshot_t s_snapshot;
static bool s_inited;
static bool s_sntp_inited;
static char s_last_public_ip[CLOUD_PUBLIC_IP_MAX_LEN];
static time_t s_next_internet_retry_at;

typedef struct
{
    char *buf;
    size_t size;
    size_t len;
} http_buf_t;

typedef struct
{
    http_buf_t body;
    int tls_code;
    int tls_flags;
    int sock_errno;
    bool had_error_event;
} http_ctx_t;

static void cloud_snapshot_lock(void)
{
    if (s_snapshot_mutex != NULL)
    {
        xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    }
}

static void cloud_snapshot_unlock(void)
{
    if (s_snapshot_mutex != NULL)
    {
        xSemaphoreGive(s_snapshot_mutex);
    }
}

static void cloud_snapshot_set_error(cloud_service_snapshot_t *snap, const char *msg)
{
    if (snap == NULL)
    {
        return;
    }
    if (msg == NULL)
    {
        snap->last_error[0] = '\0';
        return;
    }
    strlcpy(snap->last_error, msg, sizeof(snap->last_error));
}

const char *cloud_service_weather_text(int weather_code)
{
    switch (weather_code)
    {
    case 0:
        return "CLEAR";
    case 1:
    case 2:
        return "PARTLY";
    case 3:
        return "CLOUDY";
    case 45:
    case 48:
        return "FOG";
    case 51:
    case 53:
    case 55:
        return "DRIZZLE";
    case 56:
    case 57:
        return "FREEZE";
    case 61:
    case 63:
    case 65:
    case 80:
    case 81:
    case 82:
        return "RAIN";
    case 66:
    case 67:
        return "ICE RAIN";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
        return "SNOW";
    case 95:
        return "THUNDER";
    case 96:
    case 99:
        return "STORM";
    default:
        return "UNKNOWN";
    }
}

static void log_cloud_snapshot(const cloud_service_snapshot_t *snap, const char *stage)
{
    if (snap == NULL)
    {
        return;
    }

    APP_LOG_STATE(TAG,
                  "[%s] sta=%d net=%d fail=%d sta_ip=%s pub_ip=%s city=%s region=%s country=%s tz=%s time=%s temp=%.1fC hum=%d%% code=%d wind=%.1fkm/h geo=%d weather=%d time_ok=%d err=%s",
                  stage != NULL ? stage : "snapshot",
                  snap->sta_connected ? 1 : 0,
                  snap->internet_ok ? 1 : 0,
                  snap->internet_fail_count,
                  snap->sta_ip,
                  snap->public_ip[0] != '\0' ? snap->public_ip : "-",
                  snap->city[0] != '\0' ? snap->city : "-",
                  snap->region[0] != '\0' ? snap->region : "-",
                  snap->country[0] != '\0' ? snap->country : "-",
                  snap->timezone[0] != '\0' ? snap->timezone : "-",
                  snap->local_time_str[0] != '\0' ? snap->local_time_str : "-",
                  (double)snap->temperature_c,
                  snap->humidity,
                  snap->weather_code,
                  (double)snap->wind_speed_kmh,
                  snap->geo_valid ? 1 : 0,
                  snap->weather_valid ? 1 : 0,
                  snap->time_valid ? 1 : 0,
                  snap->last_error[0] != '\0' ? snap->last_error : "-");
}

static int get_internet_retry_delay_sec(int fail_count)
{
    if (fail_count <= 1) {
        return CLOUD_INTERNET_RETRY_1_SEC;
    }
    if (fail_count == 2) {
        return CLOUD_INTERNET_RETRY_2_SEC;
    }
    return CLOUD_INTERNET_RETRY_3_SEC;
}

static void mark_internet_recovered(cloud_service_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }

    if (snap->internet_fail_count > 0) {
        APP_LOGI(TAG, "internet recovered after %d failure(s)", snap->internet_fail_count);
    }

    snap->internet_ok = true;
    snap->internet_fail_count = 0;
    s_next_internet_retry_at = 0;
}

static void mark_internet_failed(cloud_service_snapshot_t *snap, const char *reason)
{
    if (snap == NULL) {
        return;
    }

    snap->internet_ok = false;
    snap->internet_fail_count++;
    cloud_snapshot_set_error(snap, reason);

    time_t now = time(NULL);
    int delay_sec = get_internet_retry_delay_sec(snap->internet_fail_count);
    s_next_internet_retry_at = (now > 0) ? (now + delay_sec) : 0;

    APP_LOGW(TAG, "internet unavailable: reason=%s fail_count=%d next_retry=%ds",
             reason != NULL ? reason : "-",
             snap->internet_fail_count,
             delay_sec);

    if (snap->internet_fail_count == CLOUD_INTERNET_RECONNECT_THRESHOLD) {
        APP_LOGW(TAG, "internet failures reached threshold, trigger STA reconnect");
        wifi_manager_reconnect_sta();
    }
}

static void clear_if_invalid(char *text, size_t size)
{
    if (text == NULL || size == 0)
    {
        return;
    }
    if (text[0] == '\0')
    {
        strlcpy(text, "-", size);
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (ctx == NULL)
    {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ERROR)
    {
        ctx->had_error_event = true;
        esp_http_client_get_and_clear_last_tls_error(evt->client, &ctx->tls_code, &ctx->tls_flags);
        ctx->sock_errno = esp_http_client_get_errno(evt->client);
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != NULL && evt->data_len > 0)
    {
        size_t copy_len = (size_t)evt->data_len;
        if (ctx->body.len + copy_len >= ctx->body.size)
        {
            copy_len = ctx->body.size - ctx->body.len - 1;
        }
        if (copy_len > 0)
        {
            memcpy(ctx->body.buf + ctx->body.len, evt->data, copy_len);
            ctx->body.len += copy_len;
            ctx->body.buf[ctx->body.len] = '\0';
        }
    }

    return ESP_OK;
}

static esp_err_t https_get_json(const char *url, char *out, size_t out_size)
{
    if (url == NULL || out == NULL || out_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    http_ctx_t ctx = {
        .body = {
            .buf = out,
            .size = out_size,
            .len = 0,
        },
        .tls_code = 0,
        .tls_flags = 0,
        .sock_errno = 0,
        .had_error_event = false,
    };
    out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = 10000,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL)
    {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300)
        {
            APP_LOGW(TAG, "http non-200 url=%s status=%d transport=%d",
                     url, status, (int)esp_http_client_get_transport_type(client));
            err = ESP_FAIL;
        }
    }
    else
    {
        APP_LOGW(TAG,
                 "http perform failed url=%s err=%s(0x%x) errno=%d tls_code=%d tls_flags=0x%x transport=%d had_error=%d",
                 url,
                 esp_err_to_name(err),
                 (unsigned int)err,
                 ctx.sock_errno,
                 ctx.tls_code,
                 ctx.tls_flags,
                 (int)esp_http_client_get_transport_type(client),
                 ctx.had_error_event ? 1 : 0);
    }

    esp_http_client_cleanup(client);
    return err;
}

static bool json_get_string(cJSON *obj, const char *key, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
        return false;
    }
    strlcpy(out, item->valuestring, out_size);
    return true;
}

static bool json_get_double(cJSON *obj, const char *key, double *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item))
    {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static bool json_get_int(cJSON *obj, const char *key, int *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item))
    {
        return false;
    }
    *out = item->valueint;
    return true;
}

static void update_local_time_snapshot(cloud_service_snapshot_t *snap)
{
    if (snap == NULL)
    {
        return;
    }

    time_t now = time(NULL);
    snap->utc_epoch = now;
    snap->local_epoch = now;

    if (now > 1600000000)
    {
        struct tm timeinfo = {0};
        localtime_r(&now, &timeinfo);
        strftime(snap->local_time_str, sizeof(snap->local_time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
        snap->time_valid = true;
    }
    else
    {
        strlcpy(snap->local_time_str, "-", sizeof(snap->local_time_str));
        snap->time_valid = false;
    }
}

static void set_timezone_from_offset(int offset_seconds)
{
    char tz_env[32];
    int west = -offset_seconds;
    int hours = west / 3600;
    int minutes = abs((west % 3600) / 60);
    snprintf(tz_env, sizeof(tz_env), "UTC%+d:%02d", hours, minutes);
    setenv("TZ", tz_env, 1);
    tzset();
}

static bool parse_utc_offset_string(const char *off, int *offset_seconds)
{
    if (off == NULL || offset_seconds == NULL)
    {
        return false;
    }
    if (strlen(off) < 5)
    {
        return false;
    }

    if ((off[0] != '+' && off[0] != '-') ||
        off[1] < '0' || off[1] > '9' ||
        off[2] < '0' || off[2] > '9' ||
        off[3] < '0' || off[3] > '9' ||
        off[4] < '0' || off[4] > '9')
    {
        return false;
    }

    int sign = (off[0] == '-') ? -1 : 1;
    int hh = ((off[1] - '0') * 10) + (off[2] - '0');
    int mm = ((off[3] - '0') * 10) + (off[4] - '0');
    *offset_seconds = sign * (hh * 3600 + mm * 60);
    return true;
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;

    cloud_snapshot_lock();
    s_snapshot.last_time_sync = time(NULL);
    update_local_time_snapshot(&s_snapshot);
    cloud_snapshot_unlock();

    APP_LOGI(TAG, "SNTP synced");
}

/* 确保 SNTP 服务已启动；只初始化一次。 */
static void ensure_sntp_started(void)
{
    if (s_sntp_inited)
    {
        return;
    }

#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 2
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "ntp.aliyun.com"));
#else
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    APP_LOGW(TAG, "CONFIG_LWIP_SNTP_MAX_SERVERS=%d, fallback to single SNTP server",
             CONFIG_LWIP_SNTP_MAX_SERVERS);
#endif
    cfg.sync_cb = sntp_sync_cb;
    cfg.renew_servers_after_new_IP = false;

    if (esp_netif_sntp_init(&cfg) == ESP_OK)
    {
        s_sntp_inited = true;
    }
    else
    {
        APP_LOGE(TAG, "SNTP init failed");
    }
}

/* 通过公网 HTTPS 接口获取当前出口公网 IP。 */
static esp_err_t refresh_public_ip(cloud_service_snapshot_t *snap)
{
    if (snap == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char response[CLOUD_HTTP_BUFFER_SIZE];
    static const char *public_ip_urls[] = {
        "https://api.ipify.org?format=json",
        "https://api64.ipify.org?format=json",
        "https://ifconfig.co/json",
    };

    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(public_ip_urls) / sizeof(public_ip_urls[0]); ++i)
    {
        err = https_get_json(public_ip_urls[i], response, sizeof(response));
        if (err == ESP_OK)
        {
            break;
        }
        APP_LOGW(TAG, "public ip request failed: %s err=%s(0x%x)",
                 public_ip_urls[i], esp_err_to_name(err), (unsigned int)err);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "public ip request failed");

    cJSON *root = cJSON_Parse(response);
    if (root == NULL)
    {
        return ESP_FAIL;
    }

    bool ok = json_get_string(root, "ip", snap->public_ip, sizeof(snap->public_ip));
    cJSON_Delete(root);

    if (!ok || snap->public_ip[0] == '\0')
    {
        return ESP_FAIL;
    }

    snap->public_ip_valid = true;
    APP_LOGI(TAG, "public ip: %s", snap->public_ip);
    APP_LOG_STATE(TAG,
                  "WAN net: lan_ip=%s public_ip=%s",
                  snap->sta_ip[0] != '\0' ? snap->sta_ip : "-",
                  snap->public_ip);
    return ESP_OK;
}

/* 基于公网 IP 获取地区、时区和经纬度。 */
static esp_err_t refresh_geo_location(cloud_service_snapshot_t *snap)
{
    if (snap == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char url[192];
    char response[CLOUD_HTTP_BUFFER_SIZE];
    // 获取地区信息
    snprintf(url, sizeof(url),
             "https://ipapi.co/%s/json/",
             snap->public_ip_valid ? snap->public_ip : "");

    ESP_RETURN_ON_ERROR(https_get_json(url, response, sizeof(response)),
                        TAG, "geo request failed");

    cJSON *root = cJSON_Parse(response);
    if (root == NULL)
    {
        return ESP_FAIL;
    }

    json_get_string(root, "country_name", snap->country, sizeof(snap->country));
    json_get_string(root, "region", snap->region, sizeof(snap->region));
    json_get_string(root, "city", snap->city, sizeof(snap->city));
    json_get_string(root, "timezone", snap->timezone, sizeof(snap->timezone));

    snap->district[0] = '\0';
    json_get_double(root, "latitude", &snap->latitude);
    json_get_double(root, "longitude", &snap->longitude);

    cJSON *utc_offset = cJSON_GetObjectItemCaseSensitive(root, "utc_offset");
    if (cJSON_IsString(utc_offset) && utc_offset->valuestring != NULL)
    {
        parse_utc_offset_string(utc_offset->valuestring, &snap->utc_offset_seconds);
    }

    cJSON_Delete(root);

    set_timezone_from_offset(snap->utc_offset_seconds);
    snap->geo_valid = true;
    snap->last_geo_refresh = time(NULL);
    update_local_time_snapshot(snap);
    APP_LOGI(TAG, "geo: country=%s region=%s city=%s timezone=%s lat=%.6f lon=%.6f utc_offset=%d",
             snap->country[0] != '\0' ? snap->country : "-",
             snap->region[0] != '\0' ? snap->region : "-",
             snap->city[0] != '\0' ? snap->city : "-",
             snap->timezone[0] != '\0' ? snap->timezone : "-",
             snap->latitude,
             snap->longitude,
             snap->utc_offset_seconds);
    return ESP_OK;
}

/* 基于经纬度获取当前天气。 */
static esp_err_t refresh_weather(cloud_service_snapshot_t *snap)
{
    if (snap == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char url[256];
    char response[CLOUD_HTTP_BUFFER_SIZE];
    // 天气
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto",
             snap->latitude, snap->longitude);

    ESP_RETURN_ON_ERROR(https_get_json(url, response, sizeof(response)),
                        TAG, "weather request failed");

    cJSON *root = cJSON_Parse(response);
    if (root == NULL)
    {
        return ESP_FAIL;
    }

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!cJSON_IsObject(current))
    {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    double temp = 0;
    double wind = 0;
    int humidity = 0;
    int code = 0;

    bool ok = json_get_double(current, "temperature_2m", &temp) &&
              json_get_double(current, "wind_speed_10m", &wind) &&
              json_get_int(current, "relative_humidity_2m", &humidity) &&
              json_get_int(current, "weather_code", &code);
    cJSON_Delete(root);

    if (!ok)
    {
        return ESP_FAIL;
    }

    snap->temperature_c = (float)temp;
    snap->wind_speed_kmh = (float)wind;
    snap->humidity = humidity;
    snap->weather_code = code;
    snap->weather_valid = true;
    snap->last_weather_refresh = time(NULL);
    APP_LOGI(TAG, "weather: temp=%.1fC humidity=%d%% code=%d wind=%.1fkm/h text=%s",
             (double)snap->temperature_c,
             snap->humidity,
             snap->weather_code,
             (double)snap->wind_speed_kmh,
             cloud_service_weather_text(snap->weather_code));
    return ESP_OK;
}

/* 刷新整份云端快照：公网 IP -> 地区 -> 天气 -> 时间。 */
static void refresh_cloud_info(bool force_geo)
{
    cloud_service_snapshot_t snap;
    cloud_service_get_snapshot(&snap);

    snap.sta_connected = wifi_manager_is_sta_connected();
    strlcpy(snap.sta_ip, wifi_manager_get_sta_ip_str(), sizeof(snap.sta_ip));

    if (!snap.sta_connected)
    {
        snap.internet_ok = false;
        snap.internet_fail_count = 0;
        s_next_internet_retry_at = 0;
        update_local_time_snapshot(&snap);
        cloud_snapshot_lock();
        s_snapshot = snap;
        cloud_snapshot_unlock();
        return;
    }

    ensure_sntp_started();
    snap.update_in_progress = true;
    cloud_snapshot_set_error(&snap, NULL);
    APP_LOGI(TAG, "refresh start: sta_ip=%s force_geo=%d", snap.sta_ip, force_geo ? 1 : 0);

    APP_LOGI(TAG, "refresh step: public ip");
    if (refresh_public_ip(&snap) != ESP_OK)
    {
        mark_internet_failed(&snap, "public ip failed");
        goto done;
    }

    mark_internet_recovered(&snap);

    bool ip_changed = strncmp(s_last_public_ip, snap.public_ip, sizeof(s_last_public_ip)) != 0;
    if (ip_changed)
    {
        strlcpy(s_last_public_ip, snap.public_ip, sizeof(s_last_public_ip));
    }

    if (force_geo || ip_changed || !snap.geo_valid)
    {
        APP_LOGI(TAG, "refresh step: geo");
        if (refresh_geo_location(&snap) != ESP_OK)
        {
            cloud_snapshot_set_error(&snap, "geo failed");
            goto done;
        }
    }
    else
    {
        APP_LOGI(TAG, "refresh step: geo skipped, cached");
        update_local_time_snapshot(&snap);
    }

    if (!snap.time_valid && s_sntp_inited)
    {
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(CLOUD_SNTP_WAIT_MS));
        update_local_time_snapshot(&snap);
    }

    time_t now = time(NULL);
    if (!snap.weather_valid ||
        (now > 0 && (now - snap.last_weather_refresh) >= CLOUD_WEATHER_REFRESH_SEC))
    {
        APP_LOGI(TAG, "refresh step: weather");
        if (refresh_weather(&snap) != ESP_OK)
        {
            cloud_snapshot_set_error(&snap, "weather failed");
            goto done;
        }
    }
    else
    {
        APP_LOGI(TAG, "refresh step: weather skipped, cached");
    }

done:
    clear_if_invalid(snap.country, sizeof(snap.country));
    clear_if_invalid(snap.region, sizeof(snap.region));
    clear_if_invalid(snap.city, sizeof(snap.city));
    clear_if_invalid(snap.timezone, sizeof(snap.timezone));
    update_local_time_snapshot(&snap);
    snap.update_in_progress = false;
    log_cloud_snapshot(&snap, "refresh_done");

    cloud_snapshot_lock();
    s_snapshot = snap;
    cloud_snapshot_unlock();
}

/* 云端后台任务：等待 Wi-Fi 状态变化或定时条件，再触发刷新。 */
static void cloud_service_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        EventBits_t bits = xEventGroupWaitBits(
            s_evt_group,
            CLOUD_EVT_REFRESH | CLOUD_EVT_WIFI_CHANGED,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(1000));

        if (wifi_manager_is_sta_connected())
        {
            cloud_service_snapshot_t snap;
            cloud_service_get_snapshot(&snap);
            time_t now = time(NULL);
            bool periodic_weather =
                snap.weather_valid &&
                now > 0 &&
                (now - snap.last_weather_refresh) >= CLOUD_WEATHER_REFRESH_SEC;
            bool retry_due =
                !snap.internet_ok &&
                snap.internet_fail_count > 0 &&
                s_next_internet_retry_at > 0 &&
                now > 0 &&
                now >= s_next_internet_retry_at;

            if ((bits & CLOUD_EVT_REFRESH) || (bits & CLOUD_EVT_WIFI_CHANGED) || periodic_weather || retry_due)
            {
                if (retry_due) {
                    APP_LOGI(TAG, "retry cloud refresh after backoff, fail_count=%d", snap.internet_fail_count);
                }
                refresh_cloud_info((bits & CLOUD_EVT_WIFI_CHANGED) != 0);
            }
            else
            {
                update_local_time_snapshot(&snap);
                cloud_snapshot_lock();
                s_snapshot = snap;
                cloud_snapshot_unlock();
            }
        }
        else
        {
            cloud_service_snapshot_t snap;
            cloud_service_get_snapshot(&snap);
            snap.sta_connected = false;
            snap.internet_ok = false;
            snap.internet_fail_count = 0;
            s_next_internet_retry_at = 0;
            strlcpy(snap.sta_ip, wifi_manager_get_sta_ip_str(), sizeof(snap.sta_ip));
            update_local_time_snapshot(&snap);
            cloud_snapshot_lock();
            s_snapshot = snap;
            cloud_snapshot_unlock();
        }
    }
}

/* 初始化云端服务资源和后台任务。 */
esp_err_t cloud_service_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    strlcpy(s_snapshot.sta_ip, "0.0.0.0", sizeof(s_snapshot.sta_ip));
    strlcpy(s_snapshot.local_time_str, "-", sizeof(s_snapshot.local_time_str));

    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_evt_group = xEventGroupCreate();
    if (s_evt_group == NULL)
    {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(
        cloud_service_task,
        "cloud_service",
        CLOUD_TASK_STACK_SIZE,
        NULL,
        CLOUD_TASK_PRIORITY,
        &s_task_handle);
    if (ok != pdPASS)
    {
        vEventGroupDelete(s_evt_group);
        s_evt_group = NULL;
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    return ESP_OK;
}

/* 通知云端模块当前 Wi-Fi 状态已发生变化。 */
void cloud_service_notify_wifi_state(void)
{
    if (!s_inited || s_evt_group == NULL)
    {
        return;
    }
    xEventGroupSetBits(s_evt_group, CLOUD_EVT_WIFI_CHANGED);
}

/* 手动请求一次云端刷新。 */
void cloud_service_request_refresh(void)
{
    if (!s_inited || s_evt_group == NULL)
    {
        return;
    }
    xEventGroupSetBits(s_evt_group, CLOUD_EVT_REFRESH);
}

/* 读取当前快照，供 UI / BLE / 日志使用。 */
void cloud_service_get_snapshot(cloud_service_snapshot_t *out)
{
    if (out == NULL)
    {
        return;
    }

    cloud_snapshot_lock();
    *out = s_snapshot;
    cloud_snapshot_unlock();
}

/* 将当前快照格式化为 JSON 状态字符串。 */
int cloud_service_format_status_json(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0)
    {
        return -1;
    }

    cloud_service_snapshot_t snap;
    cloud_service_get_snapshot(&snap);

    int len = snprintf(
        out,
        out_size,
        "{\"sta_connected\":%d,\"internet_ok\":%d,\"internet_fail_count\":%d,\"sta_ip\":\"%s\",\"public_ip\":\"%s\",\"country\":\"%s\","
        "\"region\":\"%s\",\"city\":\"%s\",\"district\":\"%s\",\"timezone\":\"%s\","
        "\"local_time\":\"%s\",\"temp_c\":%.1f,\"humidity\":%d,\"weather_code\":%d,"
        "\"wind_kmh\":%.1f,\"geo_valid\":%d,\"weather_valid\":%d,\"time_valid\":%d,"
        "\"updating\":%d,\"error\":\"%s\"}",
        snap.sta_connected ? 1 : 0,
        snap.internet_ok ? 1 : 0,
        snap.internet_fail_count,
        snap.sta_ip,
        snap.public_ip,
        snap.country,
        snap.region,
        snap.city,
        snap.district,
        snap.timezone,
        snap.local_time_str,
        (double)snap.temperature_c,
        snap.humidity,
        snap.weather_code,
        (double)snap.wind_speed_kmh,
        snap.geo_valid ? 1 : 0,
        snap.weather_valid ? 1 : 0,
        snap.time_valid ? 1 : 0,
        snap.update_in_progress ? 1 : 0,
        snap.last_error);

    if (len < 0)
    {
        return -1;
    }

    if ((size_t)len >= out_size)
    {
        if (out_size >= CLOUD_STATUS_JSON_MAX_LEN)
        {
            int fallback = snprintf(
                out,
                out_size,
                "{\"sta_connected\":%d,\"internet_ok\":%d,\"internet_fail_count\":%d,\"sta_ip\":\"%s\",\"public_ip\":\"%s\","
                "\"city\":\"%s\",\"timezone\":\"%s\",\"local_time\":\"%s\","
                "\"weather_code\":%d,\"error\":\"status too long\"}",
                snap.sta_connected ? 1 : 0,
                snap.internet_ok ? 1 : 0,
                snap.internet_fail_count,
                snap.sta_ip,
                snap.public_ip,
                snap.city,
                snap.timezone,
                snap.local_time_str,
                snap.weather_code);
            if (fallback > 0 && (size_t)fallback < out_size)
            {
                return fallback;
            }
        }
        return -1;
    }

    return len;
}
