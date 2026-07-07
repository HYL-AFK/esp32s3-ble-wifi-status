#include "wifi_manager.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_log.h"

/*
 * Wi-Fi APSTA 管理模块。
 *
 * 启动后固定工作在 WIFI_MODE_APSTA：
 * - AP 侧：始终提供设备热点，默认 ESP32S3-SETUP / 12345678。
 * - STA 侧：根据 NVS 中保存的 Wi-Fi 列表，扫描环境并连接信号最强的可用 SSID。
 *
 * BLE 配网模块收到新配置后，只需要更新 app_config，再调用 wifi_manager_connect_sta()。
 */

static const char *TAG = "wifi_manager";

/*
 * 当前仍然采用“扫描保存列表并选择最强 SSID”的策略。
 * 下面三个参数只做稳定性防抖：
 * - 弱信号门槛：优先忽略低于该 RSSI 的候选，除非没有更好的候选。
 * - 切换门槛：新候选必须明显强于上次成功连接的 SSID，才允许切走。
 * - 断线延迟：避免 AP 瞬时抖动时马上重扫拿到不完整结果。
 */
#define WIFI_WEAK_RSSI_THRESHOLD_DBM   (-82)
#define WIFI_RSSI_SWITCH_MARGIN_DB     (8)
#define WIFI_RECONNECT_DELAY_MS        (2000)

static bool s_inited = false;
static bool s_sta_reconnect_enabled = true;
static bool s_wifi_running = false;
static bool s_sta_connected = false;
static bool s_ap_started = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_sta_ip_str[16] = "0.0.0.0";
static char s_ap_ip_str[16] = "0.0.0.0";
static char s_sta_ssid[WIFI_SSID_MAX_LEN + 1] = "";
static char s_ap_name[AP_NAME_MAX_LEN + 1] = "ESP32S3-SETUP";
static char s_ap_pass[AP_PASS_MAX_LEN + 1] = "12345678";
static int s_sta_rssi = -127;
static int s_sta_channel = 0;
static int s_ap_channel = 6;
static char s_sta_phy[8] = "N/A";
static wifi_status_cb_t s_status_cb = NULL;
static app_config_t s_saved_cfg;
static char s_last_connected_ssid[WIFI_SSID_MAX_LEN + 1] = "";
static char s_preferred_fallback_ssid[WIFI_SSID_MAX_LEN + 1] = "";
static bool s_last_connected_seen_in_scan = false;
static wifi_ap_record_t s_last_connected_scan_ap;
static TaskHandle_t s_reconnect_task = NULL;

static void update_ap_ip_cache(void);

static const char *wifi_disconnect_reason_to_str(int reason)
{
    switch (reason) {
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "DISASSOC_DUE_TO_INACTIVITY";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:
        return "UNKNOWN";
    }
}

static const char *wifi_disconnect_reason_hint(int reason)
{
    switch (reason) {
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "AP dropped link or signal unstable";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "password mismatch, weak signal, or WPA handshake compatibility issue";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal too weak or AP not stable";
    case WIFI_REASON_NO_AP_FOUND:
        return "AP not seen in scan; hidden, offline, too weak, or 5GHz-only";
    case WIFI_REASON_AUTH_FAIL:
        return "authentication failed; check password or auth mode";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed; AP rejected station";
    case WIFI_REASON_CONNECTION_FAIL:
        return "generic connection failure";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "SSID exists but security mode is incompatible";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "SSID exists but below authmode threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "SSID exists but signal below RSSI threshold";
    default:
        return "-";
    }
}
/*
 * 统一状态通知入口。
 * 当前主要用于通知 BLE 模块刷新广播 Manufacturer Data 中的状态位。
 */
static void notify_status_changed(void)
{

    if (s_status_cb != NULL)
    {
        s_status_cb();
    }
}

static void format_mac_string(const uint8_t mac[6], char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    if (mac == NULL) {
        strlcpy(out, "-", out_size);
        return;
    }

    snprintf(out,
             out_size,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void log_ap_network_info(void)
{
    uint8_t ap_mac[6] = {0};
    char ap_mac_str[18] = "-";

    update_ap_ip_cache();
    if (esp_wifi_get_mac(WIFI_IF_AP, ap_mac) == ESP_OK) {
        format_mac_string(ap_mac, ap_mac_str, sizeof(ap_mac_str));
    }

    APP_LOG_STATE(TAG,
                  "AP net: ssid=%s ip=%s channel=%d mac=%s",
                  s_ap_name,
                  s_ap_ip_str,
                  s_ap_channel,
                  ap_mac_str);
}

static void log_sta_network_info(const ip_event_got_ip_t *event)
{
    if (event == NULL) {
        return;
    }

    uint8_t sta_mac[6] = {0};
    uint8_t sta_bssid[6] = {0};
    char sta_mac_str[18] = "-";
    char sta_bssid_str[18] = "-";
    char mask_str[16] = "0.0.0.0";
    char gw_str[16] = "0.0.0.0";
    char dns1_str[16] = "0.0.0.0";
    char dns2_str[16] = "0.0.0.0";

    esp_ip4addr_ntoa(&event->ip_info.netmask, mask_str, sizeof(mask_str));
    esp_ip4addr_ntoa(&event->ip_info.gw, gw_str, sizeof(gw_str));

    if (esp_wifi_get_mac(WIFI_IF_STA, sta_mac) == ESP_OK) {
        format_mac_string(sta_mac, sta_mac_str, sizeof(sta_mac_str));
    }

    if (wifi_manager_get_sta_bssid(sta_bssid)) {
        format_mac_string(sta_bssid, sta_bssid_str, sizeof(sta_bssid_str));
    }

    esp_netif_dns_info_t dns = {0};
    if (s_sta_netif != NULL &&
        esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, dns1_str, sizeof(dns1_str));
    }
    memset(&dns, 0, sizeof(dns));
    if (s_sta_netif != NULL &&
        esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
        esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, dns2_str, sizeof(dns2_str));
    }

    APP_LOG_STATE(TAG,
                  "STA wifi: ssid=%s bssid=%s rssi=%d channel=%d phy=%s sta_mac=%s",
                  s_sta_ssid[0] != '\0' ? s_sta_ssid : "-",
                  sta_bssid_str,
                  s_sta_rssi,
                  s_sta_channel,
                  s_sta_phy,
                  sta_mac_str);
    APP_LOG_STATE(TAG,
                  "STA net: ip=%s mask=%s gw=%s dns1=%s dns2=%s",
                  s_sta_ip_str,
                  mask_str,
                  gw_str,
                  dns1_str,
                  dns2_str);
}

static void sta_reconnect_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS));

    if (s_sta_reconnect_enabled) {
        if (s_saved_cfg.has_wifi && s_saved_cfg.wifi_count > 0) {
            APP_LOGI(TAG, "reconnect path: delayed re-evaluate saved wifi profiles");
            wifi_manager_connect_sta(&s_saved_cfg);
        } else {
            APP_LOGI(TAG, "reconnect path: delayed esp_wifi_connect only");
            esp_wifi_connect();
        }
    }

    s_reconnect_task = NULL;
    vTaskDelete(NULL);
}

static void schedule_sta_reconnect(void)
{
    if (s_reconnect_task != NULL) {
        APP_LOGW(TAG, "STA reconnect already scheduled");
        return;
    }

    BaseType_t ok = xTaskCreate(sta_reconnect_task,
                                "sta_reconn",
                                4096,
                                NULL,
                                5,
                                &s_reconnect_task);
    if (ok != pdPASS) {
        s_reconnect_task = NULL;
        APP_LOGE(TAG, "create STA reconnect task failed");
    }
}
/*
 * 组装 SoftAP 配置。
 * AP 密码长度 >= 8 时使用 WPA2-PSK，否则按开放热点处理。
 */
static void build_ap_config(wifi_config_t *ap_cfg)
{

    memset(ap_cfg, 0, sizeof(*ap_cfg));
    strlcpy((char *)ap_cfg->ap.ssid, s_ap_name, sizeof(ap_cfg->ap.ssid));
    ap_cfg->ap.ssid_len = 0;
    ap_cfg->ap.ssid_hidden = 0;
    ap_cfg->ap.channel = 6;
    ap_cfg->ap.max_connection = 4;
    ap_cfg->ap.beacon_interval = 100;
    strlcpy((char *)ap_cfg->ap.password, s_ap_pass, sizeof(ap_cfg->ap.password));
    ap_cfg->ap.authmode = (strlen(s_ap_pass) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

static void update_ap_ip_cache(void)
{
    /* 缓存 AP 接口 IP，方便日志、状态查询和后续 App 查询。 */
    if (s_ap_netif == NULL)
    {
        return;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK)
    {
        esp_ip4addr_ntoa(&ip_info.ip, s_ap_ip_str, sizeof(s_ap_ip_str));
    }
}

static void update_ap_channel_from_config(const wifi_config_t *ap_cfg)
{
    if (ap_cfg == NULL) {
        return;
    }
    s_ap_channel = ap_cfg->ap.channel;
}

static bool refresh_sta_runtime_info(wifi_ap_record_t *ap)
{
    if (!s_sta_connected) {
        return false;
    }

    wifi_ap_record_t local_ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&local_ap) != ESP_OK) {
        return false;
    }

    s_sta_rssi = local_ap.rssi;
    s_sta_channel = local_ap.primary;
    strlcpy(s_sta_ssid, (const char *)local_ap.ssid, sizeof(s_sta_ssid));
    if (local_ap.phy_11n) {
        strlcpy(s_sta_phy, "11n", sizeof(s_sta_phy));
    } else if (local_ap.phy_11g) {
        strlcpy(s_sta_phy, "11g", sizeof(s_sta_phy));
    } else if (local_ap.phy_11b) {
        strlcpy(s_sta_phy, "11b", sizeof(s_sta_phy));
    } else {
        strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));
    }

    if (ap != NULL) {
        *ap = local_ap;
    }

    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        /* STA 驱动已启动，不代表已经连接路由器。 */
        APP_LOGI(TAG, "STA start");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        /*
         * STA 断开：清掉连接状态和 IP 缓存。
         * 如果 s_sta_reconnect_enabled=true，则让 ESP-IDF 继续重连。
         */
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        APP_LOGW(TAG, "STA disconnected, reason=%d(%s), hint=%s, reconnect=%d, saved=%d",
                 event != NULL ? event->reason : -1,
                 event != NULL ? wifi_disconnect_reason_to_str(event->reason) : "UNKNOWN",
                 event != NULL ? wifi_disconnect_reason_hint(event->reason) : "-",
                 s_sta_reconnect_enabled ? 1 : 0,
                 (s_saved_cfg.has_wifi && s_saved_cfg.wifi_count > 0) ? 1 : 0);
        s_sta_connected = false;
        s_sta_rssi = -127;
        s_sta_channel = 0;
        strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));
        s_sta_ssid[0] = '\0';
        strlcpy(s_sta_ip_str, "0.0.0.0", sizeof(s_sta_ip_str));
        notify_status_changed();
        if (s_sta_reconnect_enabled)
        {
            APP_LOGI(TAG, "STA reconnect scheduled in %d ms", WIFI_RECONNECT_DELAY_MS);
            schedule_sta_reconnect();
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START)
    {
        /* AP 热点已经启动，手机应能扫描到 s_ap_name。 */
        s_ap_started = true;
        APP_LOGI(TAG, "AP start, ssid=%s", s_ap_name);
        log_ap_network_info();
        notify_status_changed();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP)
    {
        /* AP 热点停止，通常不应该在正常 APSTA 模式下频繁出现。 */
        s_ap_started = false;
        APP_LOGW(TAG, "AP stop");
        notify_status_changed();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        /* STA 已经从上级路由器/手机热点拿到内网 IP，说明 STA 连接成功。 */
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_connected = true;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_sta_ip_str, sizeof(s_sta_ip_str));
        APP_LOGI(TAG, "STA got IP: %s", s_sta_ip_str);
        refresh_sta_runtime_info(NULL);
        strlcpy(s_last_connected_ssid, s_sta_ssid, sizeof(s_last_connected_ssid));
        strlcpy(s_preferred_fallback_ssid, s_sta_ssid, sizeof(s_preferred_fallback_ssid));
        log_sta_network_info(event);
        notify_status_changed();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED)
    {
        /* 有手机/电脑连到设备 AP，并由设备 DHCP 分配到 192.168.4.x。 */
        APP_LOGI(TAG, "AP client got IP");
    }
}

void wifi_manager_init(const app_config_t *cfg)
{
    /*
     * Wi-Fi 初始化只执行一次。
     *
     * 初始化顺序：
     * 1. 从配置恢复 AP 名称/密码。
     * 2. 初始化 esp-netif 和默认事件循环。
     * 3. 创建 STA/AP 两个 netif。
     * 4. 注册 Wi-Fi/IP 事件回调。
     * 5. 设置 APSTA 模式并启动 Wi-Fi。
     */
    if (s_inited)
    {
        return;
    }

    if (cfg != NULL && cfg->has_ap_name && cfg->ap_name[0] != '\0')
    {
        strlcpy(s_ap_name, cfg->ap_name, sizeof(s_ap_name));
    }
    if (cfg != NULL && cfg->has_ap_pass && cfg->ap_pass[0] != '\0')
    {
        strlcpy(s_ap_pass, cfg->ap_pass, sizeof(s_ap_pass));
    }
    if (cfg != NULL) {
        s_saved_cfg = *cfg;
        APP_LOGI(TAG, "wifi manager init: saved_profiles=%d has_wifi=%d ap_name=%s",
                 cfg->wifi_count, cfg->has_wifi ? 1 : 0, s_ap_name);
    } else {
        memset(&s_saved_cfg, 0, sizeof(s_saved_cfg));
        APP_LOGW(TAG, "wifi manager init without config");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    wifi_config_t ap_cfg = {0};
    build_ap_config(&ap_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    update_ap_channel_from_config(&ap_cfg);
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_running = true;
    update_ap_ip_cache();
    s_inited = true;
}
/*
 * 动态修改 AP 名称。
 * 注意：如果手机已经连着旧 AP，改名后可能需要手机重新扫描/重连。
 */
void wifi_manager_set_ap_name(const char *ap_name)
{

    if (ap_name == NULL || ap_name[0] == '\0')
    {
        return;
    }

    if (strncmp(s_ap_name, ap_name, sizeof(s_ap_name)) == 0)
    {
        return;
    }

    strlcpy(s_ap_name, ap_name, sizeof(s_ap_name));

    wifi_config_t ap_cfg = {0};
    build_ap_config(&ap_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    update_ap_channel_from_config(&ap_cfg);

    if (!s_wifi_running)
    {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_running = true;
    }

    update_ap_ip_cache();
}

void wifi_manager_set_ap_password(const char *ap_pass)
{
    /*
     * 动态修改 AP 密码。
     * WPA/WPA2 密码要求至少 8 位，因此短密码直接忽略。
     */
    if (ap_pass == NULL || strlen(ap_pass) < 8)
    {
        return;
    }

    if (strncmp(s_ap_pass, ap_pass, sizeof(s_ap_pass)) == 0)
    {
        return;
    }

    strlcpy(s_ap_pass, ap_pass, sizeof(s_ap_pass));

    wifi_config_t ap_cfg = {0};
    build_ap_config(&ap_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

static int score_scan_result(const wifi_ap_record_t *ap)
{
    /* 当前评分只看 RSSI。后续可以叠加认证方式、黑名单、历史成功率等因素。 */
    if (ap == NULL)
    {
        return INT_MIN;
    }

    return ap->rssi;
}

static bool find_best_saved_profile(const app_config_t *cfg, wifi_ap_record_t *best_ap, wifi_profile_t *best_profile)
{
    /*
     * 选择 STA 要连接的 Wi-Fi：
     * 1. 主动扫描当前环境里的全部 AP。
     * 2. 将扫描结果和 NVS 中保存的 SSID 列表做匹配。
     * 3. 在匹配项里选择 RSSI 最大的一项。
     */
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK)
    {
        return false;
    }

    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0)
    {
        return false;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (records == NULL)
    {
        return false;
    }

    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK)
    {
        free(records);
        return false;
    }

    int best_score = INT_MIN;
    int best_usable_score = INT_MIN;
    bool found = false;
    bool found_usable = false;
    bool preferred_found = false;
    wifi_ap_record_t fallback_ap = {0};
    wifi_profile_t fallback_profile = {0};
    wifi_ap_record_t preferred_ap = {0};
    wifi_profile_t preferred_profile = {0};
    s_last_connected_seen_in_scan = false;
    memset(&s_last_connected_scan_ap, 0, sizeof(s_last_connected_scan_ap));

    for (int i = 0; i < cfg->wifi_count; ++i)
    {
        if (!cfg->wifi_profiles[i].valid || cfg->wifi_profiles[i].wifi_ssid[0] == '\0')
        {
            continue;
        }

        for (int j = 0; j < ap_count; ++j)
        {
            if (strcmp((char *)records[j].ssid, cfg->wifi_profiles[i].wifi_ssid) != 0)
            {
                continue;
            }

            if (s_last_connected_ssid[0] != '\0' &&
                strcmp((char *)records[j].ssid, s_last_connected_ssid) == 0)
            {
                if (!s_last_connected_seen_in_scan ||
                    records[j].rssi > s_last_connected_scan_ap.rssi) {
                    s_last_connected_scan_ap = records[j];
                }
                s_last_connected_seen_in_scan = true;
            }

            int score = score_scan_result(&records[j]);
            if (s_preferred_fallback_ssid[0] != '\0' &&
                strcmp((char *)records[j].ssid, s_preferred_fallback_ssid) == 0) {
                if (!preferred_found || score > score_scan_result(&preferred_ap)) {
                    preferred_ap = records[j];
                    preferred_profile = cfg->wifi_profiles[i];
                    preferred_found = true;
                }
            }

            if (!found || score > best_score)
            {
                best_score = score;
                fallback_ap = records[j];
                fallback_profile = cfg->wifi_profiles[i];
                found = true;
            }

            if (records[j].rssi >= WIFI_WEAK_RSSI_THRESHOLD_DBM &&
                (!found_usable || score > best_usable_score))
            {
                best_usable_score = score;
                *best_ap = records[j];
                *best_profile = cfg->wifi_profiles[i];
                found_usable = true;
            }
        }
    }

    if (preferred_found) {
        APP_LOGI(TAG,
                 "prefer current target SSID=%s over strongest saved candidate",
                 preferred_profile.wifi_ssid);
        *best_ap = preferred_ap;
        *best_profile = preferred_profile;
        free(records);
        return true;
    }

    if (!found_usable && found) {
        APP_LOGW(TAG, "all matched Wi-Fi signals are weak, fallback best SSID=%s RSSI=%d threshold=%d",
                 fallback_profile.wifi_ssid,
                 fallback_ap.rssi,
                 WIFI_WEAK_RSSI_THRESHOLD_DBM);
        *best_ap = fallback_ap;
        *best_profile = fallback_profile;
    }

    free(records);
    return found;
}

void wifi_manager_connect_specific_sta(const app_config_t *cfg,
                                       const char *ssid,
                                       const char *pass,
                                       const uint8_t *bssid,
                                       bool bssid_set)
{
    if (cfg != NULL) {
        s_saved_cfg = *cfg;
    }
    if (ssid != NULL && ssid[0] != '\0') {
        strlcpy(s_preferred_fallback_ssid, ssid, sizeof(s_preferred_fallback_ssid));
    }

    if (ssid == NULL || pass == NULL || ssid[0] == '\0') {
        APP_LOGW(TAG, "skip specific STA connect, invalid credentials");
        return;
    }

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    if (bssid_set && bssid != NULL) {
        memcpy(sta_cfg.sta.bssid, bssid, sizeof(sta_cfg.sta.bssid));
        sta_cfg.sta.bssid_set = 1;
    }

    APP_LOGI(TAG,
             "connecting STA directly to SSID=%s bssid_set=%d",
             ssid,
             bssid_set ? 1 : 0);
    s_sta_reconnect_enabled = true;
    strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
    s_sta_rssi = -127;
    s_sta_channel = 0;
    strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
}

void wifi_manager_connect_sta(const app_config_t *cfg)
{
    /*
     * 发起 STA 连接。
     * cfg 可以保存多组 Wi-Fi；这里不会按保存顺序盲连，而是先扫描，再选择信号最强的已保存热点。
     */
    if (cfg == NULL || !cfg->has_wifi || cfg->wifi_count <= 0)
    {
        APP_LOGW(TAG, "skip STA connect, no wifi profile");
        return;
    }

    s_saved_cfg = *cfg;

    wifi_ap_record_t best_ap = {0};
    wifi_profile_t best_profile = {0};
    if (!find_best_saved_profile(cfg, &best_ap, &best_profile))
    {
        /*
         * 启动期如果扫描暂时没命中，不要直接放弃。
         * 先回退到第一条有效保存配置做一次直连，让驱动侧继续完成后续扫描和重连。
         */
        for (int i = 0; i < cfg->wifi_count; ++i) {
            if (s_preferred_fallback_ssid[0] != '\0' &&
                cfg->wifi_profiles[i].valid &&
                strcmp(cfg->wifi_profiles[i].wifi_ssid, s_preferred_fallback_ssid) == 0) {
                best_profile = cfg->wifi_profiles[i];
                APP_LOGW(TAG,
                         "scan miss, fallback prefer SSID=%s; AP may be hidden/offline/weak or 5GHz-only",
                         best_profile.wifi_ssid);
                goto connect_sta;
            }
        }

        for (int i = 0; i < cfg->wifi_count; ++i) {
            if (cfg->wifi_profiles[i].valid && cfg->wifi_profiles[i].wifi_ssid[0] != '\0') {
                best_profile = cfg->wifi_profiles[i];
                APP_LOGW(TAG,
                         "scan miss, fallback connect SSID=%s; AP may be hidden/offline/weak or 5GHz-only",
                         best_profile.wifi_ssid);
                goto connect_sta;
            }
        }

        APP_LOGW(TAG, "no valid saved wifi profile available");
        return;
    }

    if (s_last_connected_ssid[0] != '\0' &&
        s_last_connected_seen_in_scan &&
        strcmp(best_profile.wifi_ssid, s_last_connected_ssid) != 0 &&
        best_ap.rssi < s_last_connected_scan_ap.rssi + WIFI_RSSI_SWITCH_MARGIN_DB)
    {
        for (int i = 0; i < cfg->wifi_count; ++i) {
            if (cfg->wifi_profiles[i].valid &&
                strcmp(cfg->wifi_profiles[i].wifi_ssid, s_last_connected_ssid) == 0) {
                APP_LOGI(TAG,
                         "keep previous SSID=%s, candidate=%s rssi=%d previous_rssi=%d margin=%d",
                         s_last_connected_ssid,
                         best_profile.wifi_ssid,
                         best_ap.rssi,
                         s_last_connected_scan_ap.rssi,
                         WIFI_RSSI_SWITCH_MARGIN_DB);
                best_profile = cfg->wifi_profiles[i];
                best_ap = s_last_connected_scan_ap;
                break;
            }
        }
    }

connect_sta:
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, best_profile.wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, best_profile.wifi_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    APP_LOGI(TAG, "connecting STA to SSID=%s RSSI=%d channel=%d",
             best_profile.wifi_ssid,
             best_ap.rssi != 0 ? best_ap.rssi : -127,
             best_ap.primary);
    s_sta_rssi = (best_ap.rssi != 0) ? best_ap.rssi : -127;
    s_sta_channel = best_ap.primary;
    if (best_ap.phy_11n) {
        strlcpy(s_sta_phy, "11n", sizeof(s_sta_phy));
    } else if (best_ap.phy_11g) {
        strlcpy(s_sta_phy, "11g", sizeof(s_sta_phy));
    } else if (best_ap.phy_11b) {
        strlcpy(s_sta_phy, "11b", sizeof(s_sta_phy));
    } else {
        strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));
    }
    strlcpy(s_sta_ssid, best_profile.wifi_ssid, sizeof(s_sta_ssid));
    s_sta_reconnect_enabled = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
}

void wifi_manager_enable_sta_reconnect(bool enable)
{
    s_sta_reconnect_enabled = enable;
}

void wifi_manager_set_status_cb(wifi_status_cb_t cb)
{
    s_status_cb = cb;
}

void wifi_manager_disconnect_sta(void)
{
    /*
     * 断开 STA 并清空驱动里的 STA 配置。
     * 这样 CLEAR_WIFI 后不会因为 ESP-IDF 内部仍保留旧配置而自动连回旧热点。
     */
    s_sta_reconnect_enabled = false;
    s_sta_connected = false;
    s_sta_rssi = -127;
    s_sta_channel = 0;
    strlcpy(s_sta_phy, "N/A", sizeof(s_sta_phy));
    s_sta_ssid[0] = '\0';
    s_last_connected_ssid[0] = '\0';
    s_preferred_fallback_ssid[0] = '\0';
    strlcpy(s_sta_ip_str, "0.0.0.0", sizeof(s_sta_ip_str));
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &(wifi_config_t){0}));
}

void wifi_manager_reconnect_sta(void)
{
    /*
     * 主动触发一次 STA 重连。
     * 适用于：STA 还在，但公网访问连续失败，希望重新挂一次 Wi-Fi 链路。
     */
    if (!s_inited || !s_wifi_running) {
        APP_LOGW(TAG, "skip force reconnect, wifi not ready");
        return;
    }

    if (!s_saved_cfg.has_wifi || s_saved_cfg.wifi_count <= 0) {
        APP_LOGW(TAG, "skip force reconnect, no saved wifi profile");
        return;
    }

    APP_LOGW(TAG, "force STA reconnect requested");
    s_sta_reconnect_enabled = true;

    if (s_sta_connected) {
        esp_wifi_disconnect();
    } else {
        wifi_manager_connect_sta(&s_saved_cfg);
    }
}

bool wifi_manager_is_sta_connected(void)
{
    return s_sta_connected;
}

bool wifi_manager_is_ap_started(void)
{
    return s_ap_started;
}

int wifi_manager_get_sta_rssi(void)
{
    refresh_sta_runtime_info(NULL);
    return s_sta_connected ? s_sta_rssi : -127;
}

int wifi_manager_get_sta_channel(void)
{
    refresh_sta_runtime_info(NULL);
    return s_sta_connected ? s_sta_channel : 0;
}

const char *wifi_manager_get_sta_phy_string(void)
{
    refresh_sta_runtime_info(NULL);
    return s_sta_phy;
}

int wifi_manager_get_ap_channel(void)
{
    return s_ap_started ? s_ap_channel : 0;
}

bool wifi_manager_get_sta_ip(esp_ip4_addr_t *ip)
{
    if (ip == NULL || s_sta_netif == NULL)
    {
        return false;
    }

    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK)
    {
        return false;
    }

    *ip = info.ip;
    return true;
}

bool wifi_manager_get_ap_ip(esp_ip4_addr_t *ip)
{
    if (ip == NULL || s_ap_netif == NULL)
    {
        return false;
    }

    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &info) != ESP_OK)
    {
        return false;
    }

    *ip = info.ip;
    return true;
}

bool wifi_manager_get_sta_mac(uint8_t mac[6])
{
    return mac != NULL && esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK;
}

bool wifi_manager_get_ap_mac(uint8_t mac[6])
{
    return mac != NULL && esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK;
}

bool wifi_manager_get_sta_bssid(uint8_t mac[6])
{
    if (mac == NULL || !s_sta_connected)
    {
        return false;
    }

    wifi_ap_record_t ap = { 0 };
    if (!refresh_sta_runtime_info(&ap))
    {
        return false;
    }

    memcpy(mac, ap.bssid, sizeof(ap.bssid));
    return true;
}

const char *wifi_manager_get_sta_ssid(void)
{
    refresh_sta_runtime_info(NULL);
    return s_sta_ssid;
}

const char *wifi_manager_get_ap_name(void)
{
    return s_ap_name;
}

const char *wifi_manager_get_sta_ip_str(void)
{
    return s_sta_ip_str;
}

const char *wifi_manager_get_ap_ip_str(void)
{
    return s_ap_ip_str;
}
