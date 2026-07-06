#include "mesh_audio_gateway.h"

#include "esp_log.h"

#include "mesh_audio_model.h"
#include "mesh_audio_node.h"

/*
 * 网关角色说明：
 *
 * 当前第一版做两件事：
 * 1. 作为 Mesh 节点启动，参与同一个 Mesh 网络。
 * 2. 接收音箱状态上报，后续可以转发给手机 App。
 *
 * 后续产品化时，手机 GATT 服务、节点列表、绑定关系、自动 provision 流程都建议放在本文件。
 */

static const char *TAG = "mesh_audio_gateway";

static void gateway_status_cb(uint16_t src_addr, const mesh_audio_status_t *status)
{
    /*
     * 音箱执行命令后会回报状态，网关在这里汇总。
     * 后续可以维护一张表：节点地址 -> 最新音量/播放状态/更新时间。
     */
    ESP_LOGI(TAG, "网关收到状态：src=0x%04x volume=%u play=%u",
             src_addr, status->volume, status->play_state);
}

void mesh_audio_gateway_start(void)
{
    ESP_LOGI(TAG, "启动 Mesh 网关角色");
    ESP_LOGI(TAG, "第一版使用本地自配网；正式产品需在这里扩展 Provisioner 和手机 GATT 管理入口");

    mesh_audio_node_set_status_cb(gateway_status_cb);
    mesh_audio_node_init(MESH_AUDIO_ROLE_GATEWAY);
}
