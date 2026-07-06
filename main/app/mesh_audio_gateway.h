#pragma once

/*
 * BLE Mesh 网关角色。
 *
 * 网关负责手机入口、Mesh 网络管理和节点状态汇总。
 * 第一版先启动 Mesh 节点并接收音箱状态，手机 GATT 和自动 Provisioner 后续在本模块扩展。
 */

void mesh_audio_gateway_start(void);
