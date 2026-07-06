#include "mesh_audio_speaker.h"

#include "esp_log.h"

#include "mesh_audio_model.h"
#include "mesh_audio_node.h"

/*
 * 音箱角色入口。
 *
 * 音箱收到控制器或网关发来的 Vendor Model 命令后，
 * mesh_audio_node.c 会分发命令，mesh_audio_board.c 会执行具体硬件动作。
 */

static const char *TAG = "mesh_audio_speaker";

void mesh_audio_speaker_start(void)
{
    ESP_LOGI(TAG, "启动 Mesh 音箱角色");
    mesh_audio_node_init(MESH_AUDIO_ROLE_SPEAKER);
}
