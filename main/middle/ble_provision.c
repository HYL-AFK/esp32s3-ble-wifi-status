#include "ble_provision.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_log.h"
#include "cloud_service.h"
#include "esp_bt.h"
#include "esp_mac.h"
#include "wifi_manager.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/*
 * BLE 配网中间层。
 *
 * 职责：
 * - 启动 NimBLE Host。
 * - 注册自定义 GATT 服务 FFF0。
 * - 通过 FFF1 接收手机写入的配置命令。
 * - 通过 FFF2 Notify 返回二进制 ACK/ERR 或状态。
 * - 维护 BLE 广播名、Manufacturer Data 和状态位。
 *
 * 当前同时兼容两套协议：
 * 1. 文本命令：方便手机蓝牙调试助手手工测试，例如 CFG=ssid,password。
 * 2. 二进制帧：正式 App 推荐使用，支持拆包组包、MAC 校验和 CRC16。
 */

static const char *TAG = "ble_provision";

/* GATT UUID：FFF0 是服务，FFF1 写命令，FFF2 发通知。 */
#define BLE_SERVICE_UUID       0xFFF0
#define BLE_CHAR_CMD_UUID      0xFFF1
#define BLE_CHAR_NOTIFY_UUID   0xFFF2

/*
 * 二进制帧格式：
 * Header(AA55) + Length(2 LE) + MAC(6) + Cmd(1) + Seq(1) + Data(N) + CRC16(2 LE) + Footer(55AA)
 *
 * Length 只包含 MAC + Cmd + Seq + Data，不包含 Header/Length/CRC/Footer。
 * CRC16 计算范围是 Length 字段到 Data 结束。
 */
#define BMS_FRAME_HEADER_0     0xAA
#define BMS_FRAME_HEADER_1     0x55
#define BMS_FRAME_FOOTER_0     0x55
#define BMS_FRAME_FOOTER_1     0xAA
#define BMS_FRAME_MIN_LEN      14
#define BMS_FRAME_MAX_PAYLOAD  512
#define BMS_RX_BUFFER_SIZE     640
#define BMS_TX_FRAME_MAX_LEN   (2 + 2 + 6 + 1 + 1 + BMS_FRAME_MAX_PAYLOAD + 2 + 2)
#define BMS_GATT_WRITE_MAX_LEN BMS_RX_BUFFER_SIZE

/* 二进制帧命令字。文本协议也尽量和这些语义保持一致。 */
#define BMS_CMD_SET_WIFI       0x01
#define BMS_CMD_SET_BLE_NAME   0x02
#define BMS_CMD_SET_AP_NAME    0x03
#define BMS_CMD_SET_AP_PASS    0x04
#define BMS_CMD_CLEAR_WIFI     0x05
#define BMS_CMD_QUERY_STATUS   0x06
#define BMS_CMD_ACK            0x80
#define BMS_CMD_ERR            0x81

/*
 * 广播 Manufacturer Data，共 8 字节：
 * Byte0-1: 42 4D，ASCII "BM"
 * Byte2:   产品类型
 * Byte3:   协议版本
 * Byte4-6: BLE MAC 后 3 字节
 * Byte7:   状态位 flags
 */
#define BMS_MFG_PRODUCT_TYPE   0x01
#define BMS_MFG_PROTOCOL_VER   0x01
#define BMS_ADV_FLAG_HAS_WIFI  0x01
#define BMS_ADV_FLAG_STA_CONN  0x02
#define BMS_ADV_FLAG_CFG_EN    0x04
#define BMS_ADV_FLAG_FAULT     0x08
#define BMS_ADV_FLAG_AP_ON     0x10

static app_config_t s_cfg;
static ble_cfg_apply_cb_t s_apply_cb;
static uint8_t s_own_addr_type;
static uint8_t s_device_mac[6];
static char s_adv_name[BLE_NAME_MAX_LEN + 1];
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_cmd_handle;
static uint16_t s_notify_handle;
static bool s_notify_enabled;
static bool s_ble_synced;
static bool s_adv_restart_pending;
static bool s_binary_response_sent;
static uint8_t s_rx_buffer[BMS_RX_BUFFER_SIZE];
static uint8_t s_gatt_write_buffer[BMS_GATT_WRITE_MAX_LEN];
static char s_text_cmd_buffer[BMS_GATT_WRITE_MAX_LEN + 1];
static size_t s_rx_len;

static const ble_uuid16_t s_service_uuid = BLE_UUID16_INIT(BLE_SERVICE_UUID);
static const ble_uuid16_t s_cmd_uuid = BLE_UUID16_INIT(BLE_CHAR_CMD_UUID);
static const ble_uuid16_t s_notify_uuid = BLE_UUID16_INIT(BLE_CHAR_NOTIFY_UUID);

static void ble_start_advertising(void);
static void ble_restart_advertising(void);

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;

    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }

    return crc;
}

static void make_bms_adv_name(void)
{
    /*
     * 产品化广播名固定为 BMS + BLE MAC 后 3 字节。
     * 例如 BLE MAC=1C:DB:D4:BC:7E:62，广播名就是 BMSBC7E62。
     *
     * 这样 App 可以稳定按 BMS 前缀和 MAC 后缀过滤设备。
     * 用户通过 NAME 命令设置的名字只作为“显示名”保存，不覆盖这里的扫描名。
     */
    esp_read_mac(s_device_mac, ESP_MAC_BT);
    snprintf(s_adv_name, sizeof(s_adv_name), "BMS%02X%02X%02X",
             s_device_mac[3], s_device_mac[4], s_device_mac[5]);
}

static uint8_t build_adv_status_flags(void)
{
    /*
     * 广播状态位：
     * bit0: 已保存 STA Wi-Fi
     * bit1: STA 已连接
     * bit2: 允许 BLE 配置
     * bit4: AP 已开启
     */
    uint8_t flags = BMS_ADV_FLAG_CFG_EN;

    if (s_cfg.has_wifi && s_cfg.wifi_count > 0) {
        flags |= BMS_ADV_FLAG_HAS_WIFI;
    }

    if (wifi_manager_is_sta_connected()) {
        flags |= BMS_ADV_FLAG_STA_CONN;
    }

    if (wifi_manager_is_ap_started()) {
        flags |= BMS_ADV_FLAG_AP_ON;
    }

    return flags;
}

static void send_frame(uint8_t cmd, uint8_t seq, const uint8_t *data, uint16_t data_len)
{
    /*
     * 通过 FFF2 Notify 发送二进制帧。
     * 当前栈会按 ATT/MTU 自动拆成 BLE 空口包；上层看到的是一条 Notify 数据。
     */
    if (!s_notify_enabled || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    uint16_t payload_len = 6 + 1 + 1 + data_len;
    uint16_t frame_len = 2 + 2 + payload_len + 2 + 2;
    uint8_t frame[BMS_TX_FRAME_MAX_LEN];
    if (frame_len > sizeof(frame)) {
        APP_LOGW(TAG, "tx frame too large: %u", (unsigned)frame_len);
        return;
    }

    frame[0] = BMS_FRAME_HEADER_0;
    frame[1] = BMS_FRAME_HEADER_1;
    write_le16(&frame[2], payload_len);
    memcpy(&frame[4], s_device_mac, sizeof(s_device_mac));
    frame[10] = cmd;
    frame[11] = seq;
    if (data_len > 0 && data != NULL) {
        memcpy(&frame[12], data, data_len);
    }

    uint16_t crc = crc16_ccitt_false(&frame[2], 2 + payload_len);
    write_le16(&frame[12 + data_len], crc);
    frame[14 + data_len] = BMS_FRAME_FOOTER_0;
    frame[15 + data_len] = BMS_FRAME_FOOTER_1;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, frame_len);
    if (om != NULL) {
        int rc = ble_gatts_notify_custom(s_conn_handle, s_notify_handle, om);
        if (rc != 0) {
            APP_LOGW(TAG, "notify failed: %d", rc);
        } else {
            APP_LOGI(TAG, "notify frame cmd=0x%02X seq=%u len=%u", cmd, seq, (unsigned)frame_len);
        }
    } else {
        APP_LOGW(TAG, "ble_hs_mbuf_from_flat failed, len=%u", (unsigned)frame_len);
    }
}

static void send_ack(uint8_t seq, uint8_t source_cmd)
{
    send_frame(BMS_CMD_ACK, seq, &source_cmd, 1);
}

static void send_err(uint8_t seq, uint8_t source_cmd)
{
    send_frame(BMS_CMD_ERR, seq, &source_cmd, 1);
}

static int build_status_json(char *buf, size_t buf_size)
{
    int len = cloud_service_format_status_json(buf, buf_size);
    if (len < 0 || (size_t)len >= buf_size) {
        APP_LOGW(TAG, "status json build failed");
        return -1;
    }
    return len;
}

static bool parse_len_string(const uint8_t *data, uint16_t data_len, uint16_t *offset, char *out, size_t out_size)
{
    /*
     * 二进制 Data 中的字符串格式：
     * len(1 byte) + utf8/ascii bytes
     *
     * 这样 SSID、密码、中文显示名里包含逗号时也不会影响解析。
     */
    if (*offset >= data_len || out_size == 0) {
        return false;
    }

    uint8_t len = data[*offset];
    (*offset)++;
    if (*offset + len > data_len || len >= out_size) {
        return false;
    }

    memcpy(out, &data[*offset], len);
    out[len] = '\0';
    *offset += len;
    return true;
}

static bool apply_binary_command(uint8_t cmd, uint8_t seq, const uint8_t *data, uint16_t data_len)
{
    /*
     * 二进制命令分发入口。
     * 这里只处理“命令语义”，帧头、长度、MAC、CRC 校验已经在 process_binary_rx() 完成。
     */
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char pass[WIFI_PASS_MAX_LEN + 1];
    char name[BLE_NAME_MAX_LEN + 1];
    char ap_name[AP_NAME_MAX_LEN + 1];
    char ap_pass[AP_PASS_MAX_LEN + 1];
    uint16_t offset = 0;

    switch (cmd) {
    case BMS_CMD_SET_WIFI:
        if (!parse_len_string(data, data_len, &offset, ssid, sizeof(ssid)) ||
            !parse_len_string(data, data_len, &offset, pass, sizeof(pass))) {
            return false;
        }
        if (!app_config_add_or_update_wifi(&s_cfg, ssid, pass)) {
            return false;
        }
        app_config_save(&s_cfg);
        if (s_apply_cb != NULL) {
            s_apply_cb(&s_cfg);
        }
        return true;

    case BMS_CMD_SET_BLE_NAME:
        offset = 0;
        if (!parse_len_string(data, data_len, &offset, name, sizeof(name))) {
            return false;
        }
        /*
         * 这里保存的是用户显示名，例如 BMS-ble。
         * 广播发现名仍固定为 BMSxxxxxx，便于 App 按 BMS + MAC 后缀稳定过滤设备。
         */
        strlcpy(s_cfg.ble_name, name, sizeof(s_cfg.ble_name));
        s_cfg.has_ble_name = true;
        app_config_save(&s_cfg);
        ble_svc_gap_device_name_set(s_adv_name);
        ble_restart_advertising();
        return true;

    case BMS_CMD_SET_AP_NAME:
        offset = 0;
        if (!parse_len_string(data, data_len, &offset, ap_name, sizeof(ap_name))) {
            return false;
        }
        strlcpy(s_cfg.ap_name, ap_name, sizeof(s_cfg.ap_name));
        s_cfg.has_ap_name = true;
        app_config_save(&s_cfg);
        if (s_apply_cb != NULL) {
            s_apply_cb(&s_cfg);
        }
        return true;

    case BMS_CMD_SET_AP_PASS:
        offset = 0;
        if (!parse_len_string(data, data_len, &offset, ap_pass, sizeof(ap_pass)) || strlen(ap_pass) < 8) {
            return false;
        }
        strlcpy(s_cfg.ap_pass, ap_pass, sizeof(s_cfg.ap_pass));
        s_cfg.has_ap_pass = true;
        app_config_save(&s_cfg);
        wifi_manager_set_ap_password(s_cfg.ap_pass);
        return true;

    case BMS_CMD_CLEAR_WIFI:
        app_config_erase_wifi();
        s_cfg.has_wifi = false;
        s_cfg.wifi_count = 0;
        memset(s_cfg.wifi_profiles, 0, sizeof(s_cfg.wifi_profiles));
        app_config_save(&s_cfg);
        wifi_manager_disconnect_sta();
        return true;

    case BMS_CMD_QUERY_STATUS: {
        char status[512];
        int len = build_status_json(status, sizeof(status));
        if (len < 0) {
            return false;
        }
        send_frame(BMS_CMD_ACK, seq, (const uint8_t *)status, (uint16_t)len);
        s_binary_response_sent = true;
        return true;
    }

    default:
        return false;
    }
}

static void consume_rx_bytes(size_t count)
{
    if (count >= s_rx_len) {
        s_rx_len = 0;
        return;
    }

    memmove(s_rx_buffer, s_rx_buffer + count, s_rx_len - count);
    s_rx_len -= count;
}

static void process_binary_rx(void)
{
    /*
     * 二进制拆包组包状态机。
     *
     * BLE GATT 单次写入可能只有 20 字节，也可能协商 MTU 后更大；
     * 因此这里把每次写入都当作“字节流片段”，追加到 s_rx_buffer 后再尝试解析完整帧。
     */
    while (s_rx_len >= BMS_FRAME_MIN_LEN) {
        size_t header_pos = 0;
        while (header_pos + 1 < s_rx_len &&
               !(s_rx_buffer[header_pos] == BMS_FRAME_HEADER_0 && s_rx_buffer[header_pos + 1] == BMS_FRAME_HEADER_1)) {
            header_pos++;
        }

        if (header_pos > 0) {
            consume_rx_bytes(header_pos);
        }
        if (s_rx_len < BMS_FRAME_MIN_LEN) {
            return;
        }
        if (s_rx_buffer[0] != BMS_FRAME_HEADER_0 || s_rx_buffer[1] != BMS_FRAME_HEADER_1) {
            consume_rx_bytes(1);
            continue;
        }

        uint16_t payload_len = read_le16(&s_rx_buffer[2]);
        if (payload_len < 8 || payload_len > (6 + 1 + 1 + BMS_FRAME_MAX_PAYLOAD)) {
            consume_rx_bytes(1);
            continue;
        }

        size_t frame_len = 2 + 2 + payload_len + 2 + 2;
        if (s_rx_len < frame_len) {
            return;
        }

        if (s_rx_buffer[frame_len - 2] != BMS_FRAME_FOOTER_0 || s_rx_buffer[frame_len - 1] != BMS_FRAME_FOOTER_1) {
            consume_rx_bytes(1);
            continue;
        }

        uint16_t recv_crc = read_le16(&s_rx_buffer[4 + payload_len]);
        uint16_t calc_crc = crc16_ccitt_false(&s_rx_buffer[2], 2 + payload_len);
        if (recv_crc != calc_crc) {
            APP_LOGW(TAG, "drop frame: crc mismatch recv=0x%04X calc=0x%04X", recv_crc, calc_crc);
            consume_rx_bytes(1);
            continue;
        }

        uint8_t *payload = &s_rx_buffer[4];
        uint8_t cmd = payload[6];
        uint8_t seq = payload[7];
        uint8_t *data = &payload[8];
        uint16_t data_len = payload_len - 8;

        if (memcmp(payload, s_device_mac, sizeof(s_device_mac)) != 0) {
            APP_LOGW(TAG, "drop frame: mac mismatch dst=%02X:%02X:%02X:%02X:%02X:%02X",
                     payload[0], payload[1], payload[2], payload[3], payload[4], payload[5]);
            consume_rx_bytes(frame_len);
            continue;
        }

        APP_LOGI(TAG, "rx frame cmd=0x%02X seq=%u payload_len=%u data_len=%u",
                 cmd, seq, payload_len, data_len);
        s_binary_response_sent = false;
        bool ok = apply_binary_command(cmd, seq, data, data_len);
        if (ok) {
            if (!s_binary_response_sent) {
                send_ack(seq, cmd);
            }
        } else {
            send_err(seq, cmd);
        }

        consume_rx_bytes(frame_len);
    }
}

static void append_binary_rx(const uint8_t *data, size_t len)
{
    /*
     * 追加一段 GATT 写入数据。
     * 如果缓冲区溢出，直接清空并等待下一次从帧头重新同步。
     */
    if (len > sizeof(s_rx_buffer)) {
        s_rx_len = 0;
        return;
    }

    if (s_rx_len + len > sizeof(s_rx_buffer)) {
        s_rx_len = 0;
    }

    memcpy(s_rx_buffer + s_rx_len, data, len);
    s_rx_len += len;
    process_binary_rx();
}

static int handle_text_command(char *buf)
{
    /*
     * 文本命令入口，主要用于蓝牙调试助手手工测试。
     *
     * 支持格式：
     * - CFG=ssid,password      保存 STA Wi-Fi 并触发连接
     * - NAME=xxx               保存用户显示名，不改 BLE 广播名
     * - APNAME=xxx             修改 SoftAP 名称
     * - APPASS=xxxxxxxx        修改 SoftAP 密码，至少 8 位
     * - CLEAR_WIFI             只清空 STA Wi-Fi
     * - CLEAR                  清空全部应用配置
     */
    APP_LOGI(TAG, "text cmd: %s", buf);

    if (strncmp(buf, "NAME=", 5) == 0) {
        /* NAME 命令只修改用户显示名，不修改广播 Local Name。 */
        strlcpy(s_cfg.ble_name, buf + 5, sizeof(s_cfg.ble_name));
        s_cfg.has_ble_name = true;
        app_config_save(&s_cfg);
        ble_svc_gap_device_name_set(s_adv_name);
        ble_restart_advertising();
        return 0;
    }

    if (strncmp(buf, "APNAME=", 7) == 0) {
        strlcpy(s_cfg.ap_name, buf + 7, sizeof(s_cfg.ap_name));
        s_cfg.has_ap_name = true;
        app_config_save(&s_cfg);
        if (s_apply_cb != NULL) {
            s_apply_cb(&s_cfg);
        }
        return 0;
    }

    if (strncmp(buf, "APPASS=", 7) == 0) {
        const char *pass = buf + 7;
        if (strlen(pass) < 8) {
            APP_LOGW(TAG, "AP password too short");
            return BLE_ATT_ERR_UNLIKELY;
        }
        strlcpy(s_cfg.ap_pass, pass, sizeof(s_cfg.ap_pass));
        s_cfg.has_ap_pass = true;
        app_config_save(&s_cfg);
        wifi_manager_set_ap_password(s_cfg.ap_pass);
        return 0;
    }

    if (strcmp(buf, "CLEAR") == 0) {
        app_config_erase_all();
        app_config_set_default(&s_cfg);
        make_bms_adv_name();
        app_config_save(&s_cfg);
        wifi_manager_disconnect_sta();
        if (s_apply_cb != NULL) {
            s_apply_cb(&s_cfg);
        }
        ble_svc_gap_device_name_set(s_adv_name);
        ble_restart_advertising();
        return 0;
    }

    if (strcmp(buf, "CLEAR_WIFI") == 0) {
        app_config_erase_wifi();
        s_cfg.has_wifi = false;
        s_cfg.wifi_count = 0;
        memset(s_cfg.wifi_profiles, 0, sizeof(s_cfg.wifi_profiles));
        app_config_save(&s_cfg);
        wifi_manager_disconnect_sta();
        return 0;
    }

    if (strcmp(buf, "REFRESH_CLOUD") == 0) {
        cloud_service_request_refresh();
        return 0;
    }

    if (strcmp(buf, "STATUS") == 0) {
        char status[512];
        int len = build_status_json(status, sizeof(status));
        if (len <= 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (s_notify_enabled) {
            send_frame(BMS_CMD_ACK, 0, (const uint8_t *)status, (uint16_t)len);
        } else {
            APP_LOGI(TAG, "status: %s", status);
        }
        return 0;
    }

    if (strncmp(buf, "CFG=", 4) == 0) {
        char *payload = buf + 4;
        char *ssid = strtok(payload, ",");
        char *pass = strtok(NULL, ",");

        if (app_config_add_or_update_wifi(&s_cfg, ssid, pass)) {
            app_config_save(&s_cfg);
            if (s_apply_cb != NULL) {
                s_apply_cb(&s_cfg);
            }
            return 0;
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /*
     * GATT 特征访问回调。
     * 手机向 FFF1 写入时会进入这里：
     * - 如果数据以 AA 55 开头，按二进制帧处理。
     * - 否则按文本命令处理。
     */
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == s_cmd_handle) {
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len <= 0 || len > (int)sizeof(s_gatt_write_buffer)) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        os_mbuf_copydata(ctxt->om, 0, len, s_gatt_write_buffer);

        if (len >= 2 && s_gatt_write_buffer[0] == BMS_FRAME_HEADER_0 && s_gatt_write_buffer[1] == BMS_FRAME_HEADER_1) {
            append_binary_rx(s_gatt_write_buffer, (size_t)len);
            return 0;
        }

        memcpy(s_text_cmd_buffer, s_gatt_write_buffer, (size_t)len);
        s_text_cmd_buffer[len] = '\0';
        return handle_text_command(s_text_cmd_buffer);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_chr_defs[] = {
    {
        .uuid = &s_cmd_uuid.u,
        .access_cb = ble_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE,
        .val_handle = &s_cmd_handle,
    },
    {
        .uuid = &s_notify_uuid.u,
        .access_cb = ble_access_cb,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_notify_handle,
    },
    { 0 }
};

static const struct ble_gatt_svc_def s_svc_defs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = s_chr_defs,
    },
    { 0 }
};

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    /*
     * GAP 事件回调。
     * 连接、断开、Notify 订阅状态、广播结束都会从这里进入。
     */
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_rx_len = 0;
        } else {
            ble_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        s_rx_len = 0;
        ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_notify_handle) {
            s_notify_enabled = event->subscribe.cur_notify;
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_adv_restart_pending) {
            s_adv_restart_pending = false;
        }
        ble_start_advertising();
        return 0;

    default:
        return 0;
    }
}

static void ble_start_advertising(void)
{
    /*
     * 启动 BLE 广播。
     *
     * 广播内容：
     * - Local Name: BMSxxxxxx
     * - Service UUID: FFF0
     * - Manufacturer Data: BM + 产品类型 + 协议版本 + MAC 后缀 + 状态位
     */
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_gap_adv_params adv_params = { 0 };
    // Manufacturer Data:
    // 42 4D("BM") + 产品类型 + 协议版本 + MAC 后 3 字节 + 状态位。
    uint8_t mfg_data[8] = {
        0x42,
        0x4d,
        BMS_MFG_PRODUCT_TYPE,
        BMS_MFG_PROTOCOL_VER,
        s_device_mac[3],
        s_device_mac[4],
        s_device_mac[5],
        build_adv_status_flags(),
    };

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_adv_name;
    fields.name_len = strlen(s_adv_name);
    fields.name_is_complete = 1;
    fields.uuids16 = &s_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.mfg_data = mfg_data;
    fields.mfg_data_len = sizeof(mfg_data);
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    if (ble_gap_adv_active()) {
        return;
    }

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    /*
     * 这里明确使用 legacy ADV_IND：
     * - BLE_GAP_CONN_MODE_UND：可连接、非定向广播。
     * - direct_addr = NULL：不指定中心设备地址，确认不是定向广播。
     * - BLE_GAP_DISC_MODE_GEN：通用可发现，手机蓝牙调试助手可以扫描到。
     */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        APP_LOGE(TAG, "ble_gap_adv_start ADV_IND failed: %d", rc);
    } else {
        APP_LOGI(TAG, "advertising start ok, name=%s flags=0x%02X", s_adv_name, build_adv_status_flags());
    }
}

static void ble_restart_advertising(void)
{
    /*
     * 安全刷新广播。
     *
     * NimBLE 的 stop advertising 是 GAP 流程，不能在广播 active 时 stop 后立刻 start，
     * 否则容易触发 ble_gap_adv_start 返回 2 这类状态冲突错误。
     *
     * 做法：
     * - 连接中：不重启广播，避免影响当前 BLE 配置连接。
     * - 正在广播：先 stop，并标记 s_adv_restart_pending，等 ADV_COMPLETE 再 start。
     * - 未广播：直接 start。
     */
    if (!s_ble_synced || s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    if (ble_gap_adv_active()) {
        APP_LOGI(TAG, "advertising already active, keep current ADV");
        return;
    }

    ble_start_advertising();
}

static void ble_stack_reset(int reason)
{
    APP_LOGW(TAG, "nimble reset: %d", reason);
}

static void ble_stack_sync(void)
{
    /*
     * NimBLE Host 同步完成后触发。
     * 只有到这里以后，才能安全设置 GAP 名称并启动广播。
     */
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    make_bms_adv_name();
    s_ble_synced = true;
    ble_svc_gap_device_name_set(s_adv_name);
    ble_start_advertising();
}

static void ble_host_task(void *param)
{
    /* NimBLE Host 线程入口。nimble_port_run() 会一直运行，直到协议栈停止。 */
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_provision_init(const app_config_t *cfg, ble_cfg_apply_cb_t apply_cb)
{
    /*
     * BLE 配网初始化入口。
     *
     * 初始化顺序：
     * 1. 保存当前配置和应用回调。
     * 2. 释放经典蓝牙内存，只保留 BLE。
     * 3. 初始化 NimBLE、GAP、GATT。
     * 4. 注册 FFF0/FFF1/FFF2 GATT 服务。
     * 5. 启动 NimBLE Host 线程，等待 sync 后开始广播。
     */
    if (cfg != NULL) {
        s_cfg = *cfg;
    } else {
        app_config_set_default(&s_cfg);
    }

    s_apply_cb = apply_cb;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(nimble_port_init());

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_hs_cfg.reset_cb = ble_stack_reset;
    ble_hs_cfg.sync_cb = ble_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    int rc = ble_gatts_count_cfg(s_svc_defs);
    if (rc != 0) {
        APP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(s_svc_defs);
    if (rc != 0) {
        APP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }

    nimble_port_freertos_init(ble_host_task);
}

void ble_provision_refresh_advertising(void)
{
    /*
     * Wi-Fi 状态变化后刷新广播。
     * 如果当前手机已经连接 BLE，就不重启广播，避免影响连接中的配置流程。
     */
    if (!s_ble_synced || s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    ble_restart_advertising();
}

bool ble_provision_is_ready(void)
{
    return s_ble_synced;
}

bool ble_provision_is_advertising(void)
{
    return ble_gap_adv_active();
}

bool ble_provision_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_provision_is_notify_enabled(void)
{
    return s_notify_enabled;
}

uint16_t ble_provision_get_conn_handle(void)
{
    return s_conn_handle;
}

const char *ble_provision_get_adv_name(void)
{
    return s_adv_name;
}
