#include "mesh_audio_node.h"

#include "sdkconfig.h"

#if CONFIG_MESH_AUDIO_ENABLE

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "mesh/cfg.h"
#include "mesh/mesh.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "mesh_audio_board.h"

/*
 * NimBLE Mesh 公共节点层。
 *
 * 第一版采用“本地自配网”方式，三块板烧不同角色后，使用相同 NetKey/AppKey，
 * 并使用固定地址加入同一个 Mesh 网络。这样可以先验证控制链路。
 *
 * 正式产品建议改成：
 * - 网关作为 Provisioner。
 * - 控制器/音箱作为未入网节点广播 unprovisioned beacon。
 * - 网关分配地址、下发 AppKey、绑定 Vendor Model、配置订阅/发布地址。
 */

void ble_store_config_init(void);

static const char *TAG = "mesh_audio_node";

static mesh_audio_role_t s_role;
static uint8_t s_own_addr_type;
static mesh_audio_status_cb_t s_status_cb;

static struct bt_mesh_model_pub s_vendor_pub;
static struct bt_mesh_model *s_vendor_model;

static uint8_t s_dev_uuid[16] = {
    0x42, 0x4d, 0x53, 0x41, 0x55, 0x44, 0x49, 0x4f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t s_default_net_key[16] = {
    0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};

static const uint8_t s_default_app_key[16] = {
    0x20, 0x42, 0x64, 0x86, 0xa8, 0xca, 0xec, 0x0e,
    0x11, 0x33, 0x55, 0x77, 0x99, 0xbb, 0xdd, 0xff,
};

static const uint8_t s_default_dev_key[16] = {
    0x30, 0x52, 0x74, 0x96, 0xb8, 0xda, 0xfc, 0x1e,
    0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0xed, 0x0f,
};

static uint8_t clamp_volume_i16(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return (uint8_t)value;
}

static uint16_t role_default_addr(void)
{
    switch (s_role) {
    case MESH_AUDIO_ROLE_GATEWAY:
        return MESH_AUDIO_ADDR_GATEWAY;
    case MESH_AUDIO_ROLE_CONTROLLER:
        return MESH_AUDIO_ADDR_CONTROLLER;
    case MESH_AUDIO_ROLE_SPEAKER:
        return MESH_AUDIO_ADDR_SPEAKER;
    default:
        return MESH_AUDIO_ADDR_GATEWAY;
    }
}

static void init_role_uuid(void)
{
    /*
     * UUID 用于 Mesh 入网识别。
     * 前 8 字节固定为 "BMSAUDIO"，后面写入角色和本地测试地址，方便日志排查。
     */
    s_dev_uuid[8] = (uint8_t)s_role;
    s_dev_uuid[9] = (uint8_t)(role_default_addr() & 0xff);
    s_dev_uuid[10] = (uint8_t)(role_default_addr() >> 8);
}

static void mesh_audio_send_status(struct bt_mesh_model *model, struct bt_mesh_msg_ctx *ctx)
{
    mesh_audio_status_t status = mesh_audio_board_speaker_get_status();
    struct os_mbuf *msg = NET_BUF_SIMPLE(8);

    bt_mesh_model_msg_init(msg, BT_MESH_MODEL_OP_3(MESH_AUDIO_OP_STATUS, MESH_AUDIO_CID));
    net_buf_simple_add_u8(msg, status.volume);
    net_buf_simple_add_u8(msg, (uint8_t)status.play_state);

    if (bt_mesh_model_send(model, ctx, msg, NULL, NULL) != 0) {
        ESP_LOGW(TAG, "状态上报发送失败");
    }

    os_mbuf_free_chain(msg);
}

static int mesh_audio_volume_set(struct bt_mesh_model *model,
                                 struct bt_mesh_msg_ctx *ctx,
                                 struct os_mbuf *buf)
{
    /*
     * 收到“设置音量”命令。
     * payload: volume(0~100)
     */
    uint8_t volume = net_buf_simple_pull_u8(buf);

    ESP_LOGI(TAG, "收到设置音量：src=0x%04x volume=%u", ctx->addr, volume);
    mesh_audio_board_speaker_set_volume(clamp_volume_i16(volume));
    mesh_audio_send_status(model, ctx);
    return 0;
}

static int mesh_audio_volume_up(struct bt_mesh_model *model,
                                struct bt_mesh_msg_ctx *ctx,
                                struct os_mbuf *buf)
{
    /*
     * 收到“音量增加”命令。
     * payload: step
     */
    uint8_t step = net_buf_simple_pull_u8(buf);
    mesh_audio_status_t status = mesh_audio_board_speaker_get_status();

    ESP_LOGI(TAG, "收到音量增加：src=0x%04x step=%u", ctx->addr, step);
    mesh_audio_board_speaker_set_volume(clamp_volume_i16((int)status.volume + step));
    mesh_audio_send_status(model, ctx);
    return 0;
}

static int mesh_audio_volume_down(struct bt_mesh_model *model,
                                  struct bt_mesh_msg_ctx *ctx,
                                  struct os_mbuf *buf)
{
    /*
     * 收到“音量减少”命令。
     * payload: step
     */
    uint8_t step = net_buf_simple_pull_u8(buf);
    mesh_audio_status_t status = mesh_audio_board_speaker_get_status();

    ESP_LOGI(TAG, "收到音量减少：src=0x%04x step=%u", ctx->addr, step);
    mesh_audio_board_speaker_set_volume(clamp_volume_i16((int)status.volume - step));
    mesh_audio_send_status(model, ctx);
    return 0;
}

static int mesh_audio_play(struct bt_mesh_model *model,
                           struct bt_mesh_msg_ctx *ctx,
                           struct os_mbuf *buf)
{
    (void)buf;

    ESP_LOGI(TAG, "收到播放命令：src=0x%04x", ctx->addr);
    mesh_audio_board_speaker_set_playing(true);
    mesh_audio_send_status(model, ctx);
    return 0;
}

static int mesh_audio_pause(struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct os_mbuf *buf)
{
    (void)buf;

    ESP_LOGI(TAG, "收到暂停命令：src=0x%04x", ctx->addr);
    mesh_audio_board_speaker_set_playing(false);
    mesh_audio_send_status(model, ctx);
    return 0;
}

static int mesh_audio_play_pause_toggle(struct bt_mesh_model *model,
                                        struct bt_mesh_msg_ctx *ctx,
                                        struct os_mbuf *buf)
{
    (void)buf;

    mesh_audio_status_t status = mesh_audio_board_speaker_get_status();

    ESP_LOGI(TAG, "收到播放/暂停切换：src=0x%04x", ctx->addr);
    mesh_audio_board_speaker_set_playing(status.play_state != MESH_AUDIO_PLAY_STATE_PLAYING);
    mesh_audio_send_status(model, ctx);
    return 0;
}

static int mesh_audio_query_status(struct bt_mesh_model *model,
                                   struct bt_mesh_msg_ctx *ctx,
                                   struct os_mbuf *buf)
{
    (void)buf;

    ESP_LOGI(TAG, "收到状态查询：src=0x%04x", ctx->addr);
    mesh_audio_send_status(model, ctx);
    return 0;
}

static int mesh_audio_status_recv(struct bt_mesh_model *model,
                                  struct bt_mesh_msg_ctx *ctx,
                                  struct os_mbuf *buf)
{
    (void)model;

    /*
     * 收到音箱状态上报。
     * payload: volume + play_state
     */
    mesh_audio_status_t status = {
        .volume = net_buf_simple_pull_u8(buf),
        .play_state = (mesh_audio_play_state_t)net_buf_simple_pull_u8(buf),
    };

    ESP_LOGI(TAG, "收到音箱状态：src=0x%04x volume=%u play=%u",
             ctx->addr, status.volume, status.play_state);

    if (s_status_cb != NULL) {
        s_status_cb(ctx->addr, &status);
    }

    return 0;
}

static const struct bt_mesh_model_op s_vendor_ops[] = {
    { BT_MESH_MODEL_OP_3(MESH_AUDIO_OP_VOLUME_SET, MESH_AUDIO_CID), BT_MESH_LEN_EXACT(1), mesh_audio_volume_set },
    { BT_MESH_MODEL_OP_3(MESH_AUDIO_OP_VOLUME_UP, MESH_AUDIO_CID), BT_MESH_LEN_EXACT(1), mesh_audio_volume_up },
    { BT_MESH_MODEL_OP_3(MESH_AUDIO_OP_VOLUME_DOWN, MESH_AUDIO_CID), BT_MESH_LEN_EXACT(1), mesh_audio_volume_down },
    { BT_MESH_MODEL_OP_3(MESH_AUDIO_OP_PLAY, MESH_AUDIO_CID), BT_MESH_LEN_EXACT(0), mesh_audio_play },
    { BT_MESH_MODEL_OP_3(MESH_AUDIO_OP_PAUSE, MESH_AUDIO_CID), BT_MESH_LEN_EXACT(0), mesh_audio_pause },
    { BT_MESH_MODEL_OP_3(MESH_AUDIO_OP_PLAY_PAUSE_TOGGLE, MESH_AUDIO_CID), BT_MESH_LEN_EXACT(0), mesh_audio_play_pause_toggle },
    { BT_MESH_MODEL_OP_3(MESH_AUDIO_OP_QUERY_STATUS, MESH_AUDIO_CID), BT_MESH_LEN_EXACT(0), mesh_audio_query_status },
    { BT_MESH_MODEL_OP_3(MESH_AUDIO_OP_STATUS, MESH_AUDIO_CID), BT_MESH_LEN_EXACT(2), mesh_audio_status_recv },
    BT_MESH_MODEL_OP_END,
};

static struct bt_mesh_model s_root_models[] = {
    BT_MESH_MODEL_CFG_SRV,
};

static struct bt_mesh_model s_vendor_models[] = {
    BT_MESH_MODEL_VND(MESH_AUDIO_CID,
                      MESH_AUDIO_VENDOR_MODEL_ID_SERVER,
                      s_vendor_ops,
                      &s_vendor_pub,
                      NULL),
};

static struct bt_mesh_elem s_elements[] = {
    BT_MESH_ELEM(0, s_root_models, s_vendor_models),
};

static const struct bt_mesh_comp s_comp = {
    .cid = MESH_AUDIO_CID,
    .elem = s_elements,
    .elem_count = ARRAY_SIZE(s_elements),
};

static void prov_complete(uint16_t net_idx, uint16_t addr)
{
    ESP_LOGI(TAG, "本节点已入网：net_idx=0x%04x addr=0x%04x", net_idx, addr);
}

static void prov_reset(void)
{
    ESP_LOGW(TAG, "节点已复位，重新开启未入网广播");
    bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
}

static const struct bt_mesh_prov s_prov = {
    .uuid = s_dev_uuid,
    .complete = prov_complete,
    .reset = prov_reset,
};

static void configure_local_model_keys(void)
{
    /*
     * 本地自配网测试路径：
     * 手动把 AppKey 绑定到 Vendor Model，并订阅音频控制组播地址。
     *
     * 正式产品中，这些动作应由网关 Provisioner 的 Config Client 完成。
     */
    if (!bt_mesh_app_key_exists(0)) {
        uint8_t status = bt_mesh_app_key_add(0, 0, s_default_app_key);
        if (status != 0) {
            ESP_LOGW(TAG, "添加本地 AppKey 失败 status=%u", status);
        }
    }

    s_vendor_models[0].keys[0] = 0;
    s_vendor_models[0].groups[0] = MESH_AUDIO_GROUP_ADDR;
}

static void mesh_on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE Mesh reset, reason=%d", reason);
}

static void mesh_on_sync(void)
{
    int err;
    ble_addr_t addr;

    ESP_LOGI(TAG, "NimBLE Host 已同步，开始初始化 Mesh");

    err = ble_hs_id_gen_rnd(1, &addr);
    assert(err == 0);
    err = ble_hs_id_set_rnd(addr.val);
    assert(err == 0);
    s_own_addr_type = addr.type;

    err = bt_mesh_init(s_own_addr_type, &s_prov, &s_comp);
    if (err != 0) {
        ESP_LOGE(TAG, "bt_mesh_init failed: %d", err);
        return;
    }

    s_vendor_model = &s_vendor_models[0];

    if (!bt_mesh_is_provisioned()) {
        /*
         * 本地自配网：不用网关 provision，就能让三块板先进入同一个测试网络。
         * 缺点是密钥和地址固定，不适合量产安全要求。
         */
        err = bt_mesh_provision(s_default_net_key, 0, 0, 0, role_default_addr(), s_default_dev_key);
        if (err != 0) {
            ESP_LOGW(TAG, "本地自配网失败，转为未入网广播 err=%d", err);
            bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
        }
    }

    if (bt_mesh_is_provisioned()) {
        configure_local_model_keys();
    }

    ESP_LOGI(TAG, "Mesh 初始化完成，role=%d addr=0x%04x", s_role, role_default_addr());
}

static void mesh_host_task(void *param)
{
    (void)param;

    /*
     * NimBLE Host 线程。
     * sync_cb 触发时表示蓝牙 Host 可以安全初始化 Mesh。
     */
    ble_hs_cfg.reset_cb = mesh_on_reset;
    ble_hs_cfg.sync_cb = mesh_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void mesh_send_u8(uint16_t dst_addr, uint8_t op, uint8_t value, bool has_value)
{
    struct bt_mesh_msg_ctx ctx = {
        .net_idx = 0,
        .app_idx = 0,
        .addr = dst_addr,
        .send_ttl = BT_MESH_TTL_DEFAULT,
    };
    struct os_mbuf *msg = NET_BUF_SIMPLE(8);

    if (s_vendor_model == NULL) {
        ESP_LOGW(TAG, "Mesh 尚未初始化，不能发送 opcode=0x%02x", op);
        os_mbuf_free_chain(msg);
        return;
    }

    bt_mesh_model_msg_init(msg, BT_MESH_MODEL_OP_3(op, MESH_AUDIO_CID));
    if (has_value) {
        net_buf_simple_add_u8(msg, value);
    }

    int err = bt_mesh_model_send(s_vendor_model, &ctx, msg, NULL, NULL);
    if (err != 0) {
        ESP_LOGW(TAG, "发送 Mesh 命令失败 opcode=0x%02x err=%d", op, err);
    } else {
        ESP_LOGI(TAG, "发送 Mesh 命令 opcode=0x%02x dst=0x%04x", op, dst_addr);
    }

    os_mbuf_free_chain(msg);
}

void mesh_audio_node_init(mesh_audio_role_t role)
{
    s_role = role;
    init_role_uuid();
    mesh_audio_board_init(role);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gatt_init();

    bt_mesh_register_gatt();
    ble_store_config_init();
    nimble_port_freertos_init(mesh_host_task);
}

void mesh_audio_node_set_status_cb(mesh_audio_status_cb_t cb)
{
    s_status_cb = cb;
}

void mesh_audio_node_send_volume_set(uint16_t dst_addr, uint8_t volume)
{
    mesh_send_u8(dst_addr, MESH_AUDIO_OP_VOLUME_SET, clamp_volume_i16(volume), true);
}

void mesh_audio_node_send_volume_delta(uint16_t dst_addr, int8_t delta)
{
    if (delta > 0) {
        mesh_send_u8(dst_addr, MESH_AUDIO_OP_VOLUME_UP, (uint8_t)delta, true);
    } else if (delta < 0) {
        mesh_send_u8(dst_addr, MESH_AUDIO_OP_VOLUME_DOWN, (uint8_t)(-delta), true);
    }
}

void mesh_audio_node_send_play(uint16_t dst_addr)
{
    mesh_send_u8(dst_addr, MESH_AUDIO_OP_PLAY, 0, false);
}

void mesh_audio_node_send_pause(uint16_t dst_addr)
{
    mesh_send_u8(dst_addr, MESH_AUDIO_OP_PAUSE, 0, false);
}

void mesh_audio_node_send_play_pause_toggle(uint16_t dst_addr)
{
    mesh_send_u8(dst_addr, MESH_AUDIO_OP_PLAY_PAUSE_TOGGLE, 0, false);
}

void mesh_audio_node_send_query_status(uint16_t dst_addr)
{
    mesh_send_u8(dst_addr, MESH_AUDIO_OP_QUERY_STATUS, 0, false);
}

#else

/*
 * 未启用 CONFIG_MESH_AUDIO_ENABLE 时提供空实现。
 * 这样新增 Mesh 文件不会影响原来的 Wi-Fi + BLE 配网固件。
 */

void mesh_audio_node_init(mesh_audio_role_t role)
{
    (void)role;
}

void mesh_audio_node_set_status_cb(mesh_audio_status_cb_t cb)
{
    (void)cb;
}

void mesh_audio_node_send_volume_set(uint16_t dst_addr, uint8_t volume)
{
    (void)dst_addr;
    (void)volume;
}

void mesh_audio_node_send_volume_delta(uint16_t dst_addr, int8_t delta)
{
    (void)dst_addr;
    (void)delta;
}

void mesh_audio_node_send_play(uint16_t dst_addr)
{
    (void)dst_addr;
}

void mesh_audio_node_send_pause(uint16_t dst_addr)
{
    (void)dst_addr;
}

void mesh_audio_node_send_play_pause_toggle(uint16_t dst_addr)
{
    (void)dst_addr;
}

void mesh_audio_node_send_query_status(uint16_t dst_addr)
{
    (void)dst_addr;
}

#endif
