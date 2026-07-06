#pragma once

/*
 * BLE Mesh 控制器角色。
 *
 * 控制器读取旋转编码器：
 * - 顺时针：音量增加
 * - 逆时针：音量减少
 * - 短按：播放/暂停切换
 * - 长按：查询音箱状态
 */

void mesh_audio_controller_start(void);
