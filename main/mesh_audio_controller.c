#include "mesh_audio_controller.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mesh_audio_board.h"
#include "mesh_audio_model.h"
#include "mesh_audio_node.h"

static const char *TAG = "mesh_audio_controller";

static int s_pending_volume_delta;
/*
 * 旋转编码器事件入口：
 * volume_delta > 0 表示顺时针，volume_delta < 0 表示逆时针。
 * click 表示按下旋钮，long_press 表示长按旋钮。
 */
static void controller_input_cb(int volume_delta, bool click, bool long_press)
{

    s_pending_volume_delta += volume_delta;

    if (click)
    {
        mesh_audio_node_send_play_pause_toggle(MESH_AUDIO_GROUP_ADDR);
    }

    if (long_press)
    {
        mesh_audio_node_send_query_status(MESH_AUDIO_GROUP_ADDR);
    }
}

static void controller_task(void *arg)
{
    (void)arg;

    while (1)
    {
        /*
         * 每 50ms 聚合一次旋转步数，避免机械抖动或快速旋转时刷爆 Mesh 空口。
         */
        if (s_pending_volume_delta != 0)
        {
            int delta = s_pending_volume_delta;
            s_pending_volume_delta = 0;

            if (delta > 10)
            {
                delta = 10;
            }
            else if (delta < -10)
            {
                delta = -10;
            }

            ESP_LOGI(TAG, "发送音量变化 delta=%d", delta);
            mesh_audio_node_send_volume_delta(MESH_AUDIO_GROUP_ADDR, (int8_t)delta);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void mesh_audio_controller_start(void)
{
    ESP_LOGI(TAG, "启动 Mesh 控制器角色");
    mesh_audio_node_init(MESH_AUDIO_ROLE_CONTROLLER);
    mesh_audio_board_set_input_cb(controller_input_cb);
    xTaskCreate(controller_task, "mesh_audio_ctrl", 3072, NULL, 5, NULL);
}
