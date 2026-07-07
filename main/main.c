#include "esp_err.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "app_log.h"
#include "ble_provision.h"
#include "cloud_service.h"
#include "display_st7789.h"
#include "ui_status.h"
#include "mesh_audio_controller.h"
#include "mesh_audio_gateway.h"
#include "mesh_audio_speaker.h"
#include "ota_service.h"
#include "wifi_manager.h"

static const char *TAG = "main";

/*
 * Temporary BluFi verification switch:
 * clear only the app-saved STA Wi-Fi profiles on each boot.
 * Disable after ESPARK provisioning is verified.
 */
#define APP_TEST_CLEAR_SAVED_WIFI_ON_BOOT 0

/*
 * 应用 BLE 下发的新配置。
 *
 * 当前策略：
 * 1. 先同步 AP 名称和密码；
 * 2. 如果已经保存了 STA Wi-Fi，则立即发起连接。
 */
static void apply_wifi_cfg(const app_config_t *cfg)
{
    wifi_manager_set_ap_name(cfg->ap_name);
    wifi_manager_set_ap_password(cfg->ap_pass);
    if (cfg->has_wifi && cfg->wifi_count > 0) {
        wifi_manager_connect_sta(cfg);
    }
}

/*
 * Wi-Fi 状态变化后的统一入口。
 *
 * 用途：
 * 1. 通知 BLE 刷新广播相关状态；
 * 2. 通知云端服务重新判断是否需要刷新；
 * 3. 通知屏幕刷新显示。
 */
static void handle_wifi_status_changed(void)
{
    ble_provision_refresh_advertising();
    cloud_service_notify_wifi_state();
    ota_service_notify_wifi_state();
    ui_status_refresh();
}

void app_main(void)
{
    /* 初始化 NVS；如果分区版本变化或页已满，则先擦除再重新初始化。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        APP_LOGW(TAG, "nvs needs erase, reinit");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    /* 加载应用持久化配置，包括 STA Wi-Fi 列表、BLE 显示名、AP 配置。 */
    app_config_t cfg;
    app_config_load(&cfg);
#if APP_TEST_CLEAR_SAVED_WIFI_ON_BOOT
    APP_LOGW(TAG, "test mode: erase saved STA Wi-Fi profiles on boot");
    app_config_erase_wifi();
    app_config_load(&cfg);
#endif


    /* 初始化云端状态服务。该模块负责 SNTP、公网 IP、地区和天气。 */
    ret = cloud_service_init();
    if (ret != ESP_OK) {
        APP_LOGW(TAG, "cloud service init failed: %s", esp_err_to_name(ret));
    }

    ret = ota_service_init();
    if (ret != ESP_OK) {
        APP_LOGW(TAG, "OTA service init failed: %s", esp_err_to_name(ret));
    }

    /*
     * 先尝试点亮 ST7789 屏幕。
     * 屏幕调试阶段不让它影响主功能：初始化失败只打印日志，后续 Wi-Fi/BLE 仍继续启动。
     */
    ret = display_st7789_init();
    if (ret != ESP_OK) {
        APP_LOGW(TAG, "ST7789 display init failed: %s", esp_err_to_name(ret));
    } else {
        ret = ui_status_init();
        if (ret != ESP_OK) {
            APP_LOGW(TAG, "UI init failed: %s", esp_err_to_name(ret));
        }
    }

#if CONFIG_MESH_AUDIO_ENABLE
    /* Mesh 角色模式：根据 menuconfig 选择的角色进入对应业务。 */
#if CONFIG_MESH_AUDIO_ROLE_GATEWAY
    mesh_audio_gateway_start();
#elif CONFIG_MESH_AUDIO_ROLE_CONTROLLER
    mesh_audio_controller_start();
#elif CONFIG_MESH_AUDIO_ROLE_SPEAKER
    mesh_audio_speaker_start();
#endif
#else
    /*
     * 当前默认主流程：
     * 1. 启动 BLE 配网；
     * 2. 启动 Wi-Fi 管理；
     * 3. 如果本地已有 STA 配置，则尝试自动连接。
     */
    wifi_manager_set_status_cb(handle_wifi_status_changed);
    wifi_manager_init(&cfg);
#if APP_TEST_CLEAR_SAVED_WIFI_ON_BOOT
    wifi_manager_disconnect_sta();
#endif
    ble_provision_init(&cfg, apply_wifi_cfg);
    ui_status_refresh();

    if (cfg.has_wifi && cfg.wifi_count > 0) {
        APP_LOGI(TAG, "boot auto-connect with %d saved wifi profile(s)", cfg.wifi_count);
        wifi_manager_connect_sta(&cfg);
    } else {
        APP_LOGI(TAG, "boot with no saved wifi profile");
    }

    ota_service_mark_app_valid_if_needed();
#endif
}
