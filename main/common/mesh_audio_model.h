#pragma once

/*
 * BLE Mesh 音频控制协议定义。
 *
 * 这个文件只放公共常量、角色枚举和状态结构体，不放业务逻辑。
 * 后续新增命令时，先在这里增加 opcode，再到 mesh_audio_node.c 增加收发处理。
 */

#include <stdbool.h>
#include <stdint.h>

/* 临时 Company ID。正式产品如果申请了 Bluetooth SIG Company ID，需要替换这里。 */
#define MESH_AUDIO_CID                         0x02E5

/* Vendor Model ID。Client/Server 分开，便于后续工具或 App 识别方向。 */
#define MESH_AUDIO_VENDOR_MODEL_ID_CLIENT      0x0001
#define MESH_AUDIO_VENDOR_MODEL_ID_SERVER      0x0002

/*
 * 组播地址。
 * 控制器默认发到这个地址，音箱节点订阅这个地址。
 */
#define MESH_AUDIO_GROUP_ADDR                  0xC001

/*
 * 第一版本地自配网测试地址。
 * 正式产品应由网关 Provisioner 动态分配地址。
 */
#define MESH_AUDIO_ADDR_GATEWAY                0x0001
#define MESH_AUDIO_ADDR_CONTROLLER             0x0100
#define MESH_AUDIO_ADDR_SPEAKER                0x0200

/* Vendor Opcode 定义和 payload 格式。 */
#define MESH_AUDIO_OP_VOLUME_SET               0x01 /* data: volume(0~100) */
#define MESH_AUDIO_OP_VOLUME_UP                0x02 /* data: step */
#define MESH_AUDIO_OP_VOLUME_DOWN              0x03 /* data: step */
#define MESH_AUDIO_OP_PLAY                     0x04 /* data: none */
#define MESH_AUDIO_OP_PAUSE                    0x05 /* data: none */
#define MESH_AUDIO_OP_PLAY_PAUSE_TOGGLE        0x06 /* data: none */
#define MESH_AUDIO_OP_QUERY_STATUS             0x07 /* data: none */
#define MESH_AUDIO_OP_STATUS                   0x08 /* data: volume + play_state */

typedef enum {
    MESH_AUDIO_ROLE_GATEWAY = 0,
    MESH_AUDIO_ROLE_CONTROLLER,
    MESH_AUDIO_ROLE_SPEAKER,
} mesh_audio_role_t;

typedef enum {
    MESH_AUDIO_PLAY_STATE_PAUSED = 0,
    MESH_AUDIO_PLAY_STATE_PLAYING = 1,
} mesh_audio_play_state_t;

typedef struct {
    uint8_t volume;                       /* 当前音量，范围 0~100。 */
    mesh_audio_play_state_t play_state;   /* 当前播放状态。 */
} mesh_audio_status_t;
