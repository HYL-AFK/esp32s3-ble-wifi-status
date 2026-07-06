# BLE Mesh 音频控制维护说明

## 1. 当前默认运行模式

当前工程默认仍然走原来的 Wi-Fi + BLE 配网流程。

默认配置在 `sdkconfig.defaults` 中保持：

```text
# CONFIG_MESH_AUDIO_ENABLE is not set
# CONFIG_BT_NIMBLE_MESH is not set
```

也就是说，现在烧录这块板时，它不会启动 Mesh 网关逻辑；Mesh 网关代码只是提前写好，后续测试时再通过 `menuconfig` 打开。

## 2. 分层结构

当前 `main` 目录按下面方式整理：

```text
main/
  main.c
  common/
    app_config.c/.h
    mesh_audio_model.h
  drivers/
    wifi_manager.c/.h
    mesh_audio_board.c/.h
  middle/
    ble_provision.c/.h
    mesh_audio_node.c/.h
  app/
    mesh_audio_gateway.c/.h
    mesh_audio_controller.c/.h
    mesh_audio_speaker.c/.h
```

各层职责：

- `main.c`：只做系统启动和模式选择。
- `common`：公共配置、协议常量、公共数据结构。
- `drivers`：硬件和底层能力适配，例如 Wi-Fi 管理、旋转编码器、音箱播放接口。
- `middle`：中间层协议封装，例如 BLE 配网、NimBLE Mesh 节点收发。
- `app`：应用层角色逻辑，例如网关、控制器、音箱。

## 3. Mesh 三种角色

Mesh 代码预留了三种角色：

- `Gateway`：网关。后续手机只和网关交互，网关管理 Mesh 网络。
- `Controller`：控制器。读取旋转编码器并发送音量/播放控制。
- `Speaker`：音箱。接收控制命令，执行音量和播放/暂停动作。

当前这块板后续要作为网关测试时，选择 `Gateway` 角色。

## 4. 后续如何切到 Mesh 网关

执行：

```powershell
idf.py menuconfig
```

进入：

```text
Mesh Audio Control
```

打开：

```text
Enable BLE Mesh audio control mode
```

选择：

```text
Gateway
```

打开后，`app_main()` 会走：

```c
mesh_audio_gateway_start();
```

关闭 `CONFIG_MESH_AUDIO_ENABLE` 时，`app_main()` 会继续走原来的：

```c
ble_provision_init(...)
wifi_manager_init(...)
```

## 5. 当前 Mesh 第一版入网方式

第一版为了方便三块板快速联调，使用本地自配网：

- 网关地址：`0x0001`
- 控制器地址：`0x0100`
- 音箱地址：`0x0200`
- 组播地址：`0xC001`
- 三种角色使用相同测试 `NetKey/AppKey`

这只适合早期联调。正式产品建议改成：

- 网关作为 Provisioner。
- 控制器和音箱作为未入网节点广播。
- 网关扫描并添加节点。
- 网关分配地址、下发 AppKey、绑定 Vendor Model。
- 网关配置音箱订阅地址和控制器发布地址。

## 6. 控制器旋钮映射

- 顺时针旋转：发送 `MESH_AUDIO_OP_VOLUME_UP`。
- 逆时针旋转：发送 `MESH_AUDIO_OP_VOLUME_DOWN`。
- 短按旋钮：发送 `MESH_AUDIO_OP_PLAY_PAUSE_TOGGLE`。
- 长按旋钮：发送 `MESH_AUDIO_OP_QUERY_STATUS`。

旋转事件每 50ms 聚合一次，避免频繁发送 Mesh 消息。

## 7. 音箱接口怎么对接

真实音箱控制只需要改 `drivers/mesh_audio_board.c`：

```c
mesh_audio_board_speaker_set_volume()
mesh_audio_board_speaker_set_playing()
```

Mesh 协议层不直接操作播放器、功放或音频芯片。

## 8. 新增命令该改哪里

1. 在 `common/mesh_audio_model.h` 增加 opcode。
2. 在 `middle/mesh_audio_node.c` 的 `s_vendor_ops` 注册 opcode。
3. 在 `middle/mesh_audio_node.c` 增加 opcode 处理函数。
4. 如果控制器或网关需要主动发送该命令，在 `middle/mesh_audio_node.h/.c` 增加发送函数。
5. 如果涉及硬件动作，只改 `drivers/mesh_audio_board.c`。

## 9. 和原 BLE 配网的关系

`middle/ble_provision.c` 和 `middle/ble_provision.h` 保留原功能，只移动目录，不改业务逻辑。

- Mesh 关闭：原 Wi-Fi + BLE 配网功能继续使用。
- Mesh 打开：进入 Mesh 音频控制流程，不启动原 BLE 配网模块。
