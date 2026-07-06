#pragma once

/*
 * BLE Mesh 音箱角色。
 *
 * 音箱作为 Vendor Model Server 接收音量和播放控制命令。
 * 命令真正的处理回调在 mesh_audio_node.c 中，硬件动作在 mesh_audio_board.c 中。
 */

void mesh_audio_speaker_start(void);
