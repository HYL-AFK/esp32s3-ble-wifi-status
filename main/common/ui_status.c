#include "ui_status.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "app_log.h"
#include "ble_provision.h"
#include "cloud_service.h"
#include "display_st7789.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "ota_service.h"
#include "wifi_manager.h"

/*
 * LVGL 状态面板。
 *
 * 作用只有一个：把当前 Wi-Fi / BLE 运行状态显示出来。
 * 不接管配网、不改协议、不改业务逻辑。
 *
 * 当前页面按 320x170 横屏设计，采用英文显示，避免中文字体和字库依赖。
 */

static const char *TAG = "ui_status";

typedef struct {
    lv_display_t *disp;
    lv_obj_t *root;
    lv_obj_t *header;
    lv_obj_t *headline;
    lv_obj_t *wifi_title;
    lv_obj_t *ble_title;
    lv_obj_t *wifi_sta;
    lv_obj_t *wifi_ssid;
    lv_obj_t *wifi_rssi;
    lv_obj_t *wifi_chan;
    lv_obj_t *wifi_phy;
    lv_obj_t *wifi_sta_ip;
    lv_obj_t *wifi_ap_name;
    lv_obj_t *wifi_ap_ip;
    lv_obj_t *ble_state;
    lv_obj_t *ble_name;
    lv_obj_t *ble_conn;
    lv_obj_t *ble_notify;
    lv_obj_t *ble_adv;
    lv_obj_t *ble_net;
    lv_obj_t *cloud_ip;
    lv_obj_t *cloud_city;
    lv_obj_t *cloud_weather;
    lv_obj_t *cloud_time;
    lv_obj_t *cloud_tz;
    lv_obj_t *ota_state;
    lv_obj_t *ota_progress;
    lv_timer_t *timer;
    bool ready;
} ui_status_ctx_t;

static ui_status_ctx_t s_ui;

#define UI_SCREEN_W             320
#define UI_SCREEN_H             240
#define UI_HEADER_H             28
#define UI_SECTION_TITLE_Y      38
#define UI_SECTION_BODY_Y       52
#define UI_LEFT_X               10
#define UI_RIGHT_X              168
#define UI_SECTION_W            142
#define UI_ROW_H                14
#define UI_CLOUD_ROW1_Y         164
#define UI_CLOUD_ROW2_Y         176
#define UI_CLOUD_ROW3_Y         188
#define UI_CLOUD_ROW4_Y         200
#define UI_CLOUD_ROW5_Y         212
#define UI_OTA_ROW1_Y           224
#define UI_CLOUD_FULL_W         300

static void set_label_text(lv_obj_t *obj, const char *fmt, ...)
{
    if (obj == NULL || fmt == NULL) {
        return;
    }

    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(obj, buf);
}

static const char *on_off(bool v)
{
    return v ? "ON" : "OFF";
}

static void format_time_text(const cloud_service_snapshot_t *cloud, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    if (cloud == NULL || !cloud->time_valid || cloud->local_time_str[0] == '\0') {
        strlcpy(out, "--", out_size);
        return;
    }

    strlcpy(out, cloud->local_time_str, out_size);
}

static void format_timezone_text(const cloud_service_snapshot_t *cloud, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    if (cloud == NULL || cloud->timezone[0] == '\0') {
        strlcpy(out, "-", out_size);
        return;
    }

    strlcpy(out, cloud->timezone, out_size);
}

static void format_area_text(const cloud_service_snapshot_t *cloud, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    if (cloud == NULL) {
        strlcpy(out, "-", out_size);
        return;
    }

    if (cloud->district[0] != '\0') {
        snprintf(out, out_size, "%s/%s/%s",
                 cloud->district,
                 cloud->city[0] != '\0' ? cloud->city : "-",
                 cloud->region[0] != '\0' ? cloud->region : "-");
        return;
    }

    if (cloud->city[0] != '\0' && cloud->region[0] != '\0') {
        snprintf(out, out_size, "%s/%s", cloud->city, cloud->region);
        return;
    }

    if (cloud->city[0] != '\0') {
        strlcpy(out, cloud->city, out_size);
        return;
    }

    if (cloud->region[0] != '\0') {
        strlcpy(out, cloud->region, out_size);
        return;
    }

    if (cloud->country[0] != '\0') {
        strlcpy(out, cloud->country, out_size);
        return;
    }

    strlcpy(out, "-", out_size);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (text != NULL) {
        lv_label_set_text(label, text);
    }
    return label;
}

static void ui_status_build(void)
{
    if (s_ui.root == NULL) {
        return;
    }

    lv_obj_clean(s_ui.root);
    lv_obj_set_style_bg_color(s_ui.root, lv_color_hex(0x07111C), 0);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ui.root, UI_SCREEN_W, UI_SCREEN_H);

    s_ui.header = lv_obj_create(s_ui.root);
    lv_obj_set_pos(s_ui.header, 0, 0);
    lv_obj_set_size(s_ui.header, UI_SCREEN_W, UI_HEADER_H);
    lv_obj_set_style_radius(s_ui.header, 0, 0);
    lv_obj_set_style_border_width(s_ui.header, 0, 0);
    lv_obj_set_style_pad_all(s_ui.header, 0, 0);
    lv_obj_set_style_bg_color(s_ui.header, lv_color_hex(0x0D1C2A), 0);

    make_label(s_ui.header, 10, 6, 160, "SYSTEM STATUS", 0xFFFFFF);
    s_ui.headline = make_label(s_ui.header, 184, 6, 126, "", 0x7ED6DF);

    s_ui.wifi_title = make_label(s_ui.root, UI_LEFT_X, UI_SECTION_TITLE_Y, UI_SECTION_W, "WIFI", 0x4CC9F0);
    s_ui.ble_title = make_label(s_ui.root, UI_RIGHT_X, UI_SECTION_TITLE_Y, UI_SECTION_W, "BLE", 0x4CC9F0);

    s_ui.wifi_sta = make_label(s_ui.root, UI_LEFT_X, UI_SECTION_BODY_Y + UI_ROW_H * 0, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.wifi_ssid = make_label(s_ui.root, UI_LEFT_X, UI_SECTION_BODY_Y + UI_ROW_H * 1, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.wifi_rssi = make_label(s_ui.root, UI_LEFT_X, UI_SECTION_BODY_Y + UI_ROW_H * 2, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.wifi_chan = make_label(s_ui.root, UI_LEFT_X, UI_SECTION_BODY_Y + UI_ROW_H * 3, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.wifi_phy = make_label(s_ui.root, UI_LEFT_X, UI_SECTION_BODY_Y + UI_ROW_H * 4, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.wifi_sta_ip = make_label(s_ui.root, UI_LEFT_X, UI_SECTION_BODY_Y + UI_ROW_H * 5, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.wifi_ap_name = make_label(s_ui.root, UI_LEFT_X, UI_SECTION_BODY_Y + UI_ROW_H * 6, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.wifi_ap_ip = make_label(s_ui.root, UI_LEFT_X, UI_SECTION_BODY_Y + UI_ROW_H * 7, UI_SECTION_W, "", 0xD8E1EA);

    s_ui.ble_state = make_label(s_ui.root, UI_RIGHT_X, UI_SECTION_BODY_Y + UI_ROW_H * 0, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.ble_name = make_label(s_ui.root, UI_RIGHT_X, UI_SECTION_BODY_Y + UI_ROW_H * 1, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.ble_conn = make_label(s_ui.root, UI_RIGHT_X, UI_SECTION_BODY_Y + UI_ROW_H * 2, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.ble_notify = make_label(s_ui.root, UI_RIGHT_X, UI_SECTION_BODY_Y + UI_ROW_H * 3, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.ble_adv = make_label(s_ui.root, UI_RIGHT_X, UI_SECTION_BODY_Y + UI_ROW_H * 4, UI_SECTION_W, "", 0xD8E1EA);
    s_ui.ble_net = make_label(s_ui.root, UI_RIGHT_X, UI_SECTION_BODY_Y + UI_ROW_H * 5, UI_SECTION_W, "", 0xD8E1EA);

    s_ui.cloud_ip = make_label(s_ui.root, 10, UI_CLOUD_ROW1_Y, UI_CLOUD_FULL_W, "", 0xD8E1EA);
    s_ui.cloud_city = make_label(s_ui.root, 10, UI_CLOUD_ROW2_Y, UI_CLOUD_FULL_W, "", 0xD8E1EA);
    s_ui.cloud_weather = make_label(s_ui.root, 10, UI_CLOUD_ROW3_Y, UI_CLOUD_FULL_W, "", 0xD8E1EA);
    s_ui.cloud_time = make_label(s_ui.root, 10, UI_CLOUD_ROW4_Y, UI_CLOUD_FULL_W, "", 0xD8E1EA);
    s_ui.cloud_tz = make_label(s_ui.root, 10, UI_CLOUD_ROW5_Y, UI_CLOUD_FULL_W, "", 0xD8E1EA);
    s_ui.ota_state = make_label(s_ui.root, 10, UI_OTA_ROW1_Y, 150, "", 0xD8E1EA);
    s_ui.ota_progress = make_label(s_ui.root, 168, UI_OTA_ROW1_Y, 142, "", 0xD8E1EA);
}

static void ui_status_refresh_values(void)
{
    char area_text[64];
    char time_text[32];
    char tz_text[64];
    cloud_service_snapshot_t cloud;
    ota_service_snapshot_t ota;
    bool sta_connected = wifi_manager_is_sta_connected();
    bool ble_connected = ble_provision_is_connected();
    uint16_t conn_handle = ble_provision_get_conn_handle();

    cloud_service_get_snapshot(&cloud);
    ota_service_get_snapshot(&ota);

    set_label_text(s_ui.headline, "%s | %s",
                   sta_connected ? "WIFI OK" : "WIFI IDLE",
                   ble_connected ? "BLE OK" : "BLE ADV");

    set_label_text(s_ui.wifi_sta, "STA: %s", sta_connected ? "CONNECTED" : "DISCONNECTED");
    set_label_text(s_ui.wifi_ssid, "SSID: %s", wifi_manager_get_sta_ssid()[0] ? wifi_manager_get_sta_ssid() : "-");
    set_label_text(s_ui.wifi_rssi, "RSSI: %d dBm", wifi_manager_get_sta_rssi());
    set_label_text(s_ui.wifi_chan, "CH: %d", wifi_manager_get_sta_channel());
    set_label_text(s_ui.wifi_phy, "PHY: %s", wifi_manager_get_sta_phy_string());
    set_label_text(s_ui.wifi_sta_ip, "STA IP: %s", wifi_manager_get_sta_ip_str());
    set_label_text(s_ui.wifi_ap_name, "AP: %s", wifi_manager_get_ap_name());
    set_label_text(s_ui.wifi_ap_ip, "AP IP: %s", wifi_manager_get_ap_ip_str());

    set_label_text(s_ui.ble_state, "BLE: %s", ble_connected ? "CONNECTED" : "ADVERTISING");
    set_label_text(s_ui.ble_name, "NAME: %s", ble_provision_get_adv_name());
    if (ble_connected && conn_handle != 0xFFFF) {
        set_label_text(s_ui.ble_conn, "CONN: %u", (unsigned)conn_handle);
    } else {
        set_label_text(s_ui.ble_conn, "CONN: NONE");
    }
    set_label_text(s_ui.ble_notify, "NOTIFY: %s", on_off(ble_provision_is_notify_enabled()));
    set_label_text(s_ui.ble_adv, "ADV: %s", on_off(ble_provision_is_advertising()));
    set_label_text(s_ui.ble_net, "NET: %s (%d)",
                   cloud.internet_ok ? "OK" : "FAIL",
                   cloud.internet_fail_count);
    set_label_text(s_ui.cloud_ip, "PUB IP: %s", cloud.public_ip_valid ? cloud.public_ip : "-");
    format_area_text(&cloud, area_text, sizeof(area_text));
    set_label_text(s_ui.cloud_city, "AREA: %s", area_text);
    if (cloud.weather_valid) {
        set_label_text(s_ui.cloud_weather, "WEATHER: %s %.1fC",
                       cloud_service_weather_text(cloud.weather_code),
                       (double)cloud.temperature_c);
    } else {
        set_label_text(s_ui.cloud_weather, "WEATHER: --");
    }
    format_time_text(&cloud, time_text, sizeof(time_text));
    set_label_text(s_ui.cloud_time, "TIME: %s", time_text);
    format_timezone_text(&cloud, tz_text, sizeof(tz_text));
    set_label_text(s_ui.cloud_tz, "TZ: %s", tz_text);
    set_label_text(s_ui.ota_state, "OTA: %s", ota_service_state_to_string(ota.state));
    set_label_text(s_ui.ota_progress, "OTA PROG: %d%%", ota.progress);
}

static void ui_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ui.ready) {
        return;
    }

    lvgl_port_lock(0);
    ui_status_refresh_values();
    lvgl_port_unlock();
}

esp_err_t ui_status_init(void)
{
    if (s_ui.ready) {
        return ESP_OK;
    }

    if (!display_st7789_is_ready()) {
        APP_LOGW(TAG, "display not ready");
        return ESP_FAIL;
    }

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = display_st7789_get_io_handle(),
        .panel_handle = display_st7789_get_panel_handle(),
        .control_handle = display_st7789_get_panel_handle(),
        .buffer_size = 320 * 16,
        .double_buffer = false,
        .trans_size = 320 * 16,
        .hres = 320,
        .vres = 240,
        .monochrome = false,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    s_ui.disp = lvgl_port_add_disp(&disp_cfg);
    if (s_ui.disp == NULL) {
        APP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    lvgl_port_lock(0);
    s_ui.root = lv_screen_active();
    ui_status_build();
    ui_status_refresh_values();
    lvgl_port_unlock();

    s_ui.timer = lv_timer_create(ui_status_timer_cb, 1000, NULL);
    s_ui.ready = true;
    APP_LOGI(TAG, "ui status panel ready");
    return ESP_OK;
}

void ui_status_refresh(void)
{
    if (!s_ui.ready) {
        return;
    }

    lvgl_port_lock(0);
    ui_status_refresh_values();
    lvgl_port_unlock();
}
