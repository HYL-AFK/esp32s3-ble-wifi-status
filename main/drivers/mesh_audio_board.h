#pragma once

/*
 * BLE Mesh 音频控制硬件适配层。
 *
 * Mesh 协议层不直接操作 GPIO、功放、音频芯片或播放器。
 * 后续更换旋转编码器引脚、音频芯片、功放控制方式时，只改这个模块。
 */

#include <stdbool.h>
#include <stdint.h>

#include "mesh_audio_model.h"

/*
 * 控制器输入事件回调。
 * volume_delta > 0 表示顺时针，volume_delta < 0 表示逆时针。
 * click 表示短按，long_press 表示长按。
 */
typedef void (*mesh_audio_input_cb_t)(int volume_delta, bool click, bool long_press);

void mesh_audio_board_init(mesh_audio_role_t role);
void mesh_audio_board_set_input_cb(mesh_audio_input_cb_t cb);
void mesh_audio_board_speaker_set_volume(uint8_t volume);
void mesh_audio_board_speaker_set_playing(bool playing);
mesh_audio_status_t mesh_audio_board_speaker_get_status(void);

/*
 * 调试入口：没有接旋转编码器时，可以在代码里主动调用它模拟输入。
 * 真实量产逻辑中，这个函数通常只由 GPIO/编码器驱动调用。
 */
void mesh_audio_board_simulate_input(int volume_delta, bool click, bool long_press);
