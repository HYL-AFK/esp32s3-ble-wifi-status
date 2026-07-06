#include "mesh_audio_board.h"

#include <stddef.h>

#include "esp_log.h"

/*
 * 本文件是硬件适配层。
 *
 * 现在先做可编译、可打日志、可维护的占位实现：
 * - 控制器：预留旋转编码器 GPIO 初始化位置。
 * - 音箱：预留真实播放器/功放/音频芯片接口位置。
 *
 * 后续接入真实硬件时，优先改这里，不要把 GPIO 或播放器代码写进 Mesh 协议层。
 */

static const char *TAG = "mesh_audio_board";

static mesh_audio_input_cb_t s_input_cb;
static mesh_audio_status_t s_speaker_status = {
    .volume = 50,
    .play_state = MESH_AUDIO_PLAY_STATE_PAUSED,
};

static void init_controller_hardware(void)
{
    /*
     * TODO: 在这里初始化旋转编码器和按键 GPIO。
     *
     * 建议做法：
     * 1. A/B 相接 GPIO 中断，解析方向。
     * 2. 按键 GPIO 做消抖，区分短按和长按。
     * 3. 检测到事件后调用 mesh_audio_board_simulate_input()。
     *
     * 这样 mesh_audio_controller.c 不需要知道具体 GPIO 号。
     */
    ESP_LOGI(TAG, "控制器硬件初始化占位：后续在这里接入旋转编码器和按键");
}

static void init_speaker_hardware(void)
{
    /*
     * TODO: 在这里初始化音箱相关硬件。
     *
     * 例如：
     * - 初始化音频解码芯片。
     * - 初始化 I2S。
     * - 初始化功放使能 GPIO。
     * - 初始化播放器状态机。
     */
    ESP_LOGI(TAG, "音箱硬件初始化占位：后续在这里接入播放器/功放/音频芯片");
}

void mesh_audio_board_init(mesh_audio_role_t role)
{
    ESP_LOGI(TAG, "硬件适配层初始化，role=%d", role);

    if (role == MESH_AUDIO_ROLE_CONTROLLER) {
        init_controller_hardware();
    } else if (role == MESH_AUDIO_ROLE_SPEAKER) {
        init_speaker_hardware();
    }
}

void mesh_audio_board_set_input_cb(mesh_audio_input_cb_t cb)
{
    /*
     * 控制器模块注册输入回调。
     * 真实编码器驱动检测到旋转/短按/长按后，通过这个回调把事件交给控制器业务层。
     */
    s_input_cb = cb;
}

void mesh_audio_board_simulate_input(int volume_delta, bool click, bool long_press)
{
    if (s_input_cb != NULL) {
        s_input_cb(volume_delta, click, long_press);
    }
}

void mesh_audio_board_speaker_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }

    s_speaker_status.volume = volume;

    /*
     * TODO: 对接真实音量接口。
     * 例如 audio_hal_set_volume(volume) 或 player_set_volume(volume)。
     */
    ESP_LOGI(TAG, "音箱音量设置为 %u", volume);
}

void mesh_audio_board_speaker_set_playing(bool playing)
{
    s_speaker_status.play_state = playing ? MESH_AUDIO_PLAY_STATE_PLAYING : MESH_AUDIO_PLAY_STATE_PAUSED;

    /*
     * TODO: 对接真实播放控制接口。
     * 例如 playing=true 调用 player_play()，playing=false 调用 player_pause()。
     */
    ESP_LOGI(TAG, "音箱播放状态：%s", playing ? "播放" : "暂停");
}

mesh_audio_status_t mesh_audio_board_speaker_get_status(void)
{
    return s_speaker_status;
}
