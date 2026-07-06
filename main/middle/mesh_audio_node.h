#pragma once

/*
 * BLE Mesh 公共节点层。
 *
 * 三种角色都通过这个模块初始化 NimBLE Mesh、注册 Vendor Model、发送控制命令。
 * 角色差异放在 gateway/controller/speaker 模块中，硬件差异放在 board 模块中。
 */

#include <stdint.h>

#include "mesh_audio_model.h"

typedef void (*mesh_audio_status_cb_t)(uint16_t src_addr, const mesh_audio_status_t *status);

void mesh_audio_node_init(mesh_audio_role_t role);
void mesh_audio_node_set_status_cb(mesh_audio_status_cb_t cb);

void mesh_audio_node_send_volume_set(uint16_t dst_addr, uint8_t volume);
void mesh_audio_node_send_volume_delta(uint16_t dst_addr, int8_t delta);
void mesh_audio_node_send_play(uint16_t dst_addr);
void mesh_audio_node_send_pause(uint16_t dst_addr);
void mesh_audio_node_send_play_pause_toggle(uint16_t dst_addr);
void mesh_audio_node_send_query_status(uint16_t dst_addr);
