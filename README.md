# ESP32-S3FH4R2 Power-Go 固件说明

## 项目概览

这是一个基于 `ESP32-S3FH4R2` 的设备固件，当前主流程包含：

- 标准 `BluFi` BLE 配网
- `Wi-Fi APSTA`
- `ST7789` SPI 屏幕
- `LVGL` 全屏状态页
- 云端信息获取
  - 公网 IP
  - 地理位置
  - SNTP 本地时间
  - 天气信息
- `A/B OTA + rollback`

当前 BLE 配网已经切换为**标准 BluFi**，不再使用旧的自定义：

- `FFF0 / FFF1 / FFF2`
- `CFG=ssid,password`
- `AA55 ... 55AA` 私有二进制帧

## 当前启动流程

1. 初始化 NVS
2. 加载本地配置
3. 初始化云端状态服务
4. 初始化 OTA 服务
5. 初始化 ST7789 和 LVGL 状态页
6. 初始化 `Wi-Fi APSTA`
7. 初始化 `BluFi`
8. 如有已保存 Wi-Fi，则按现有 Wi-Fi 策略自动连接

## BLE / BluFi

### 广播信息

- 广播名固定为 `ESPARK-PowerGo`
- BLE 宿主使用 `NimBLE`
- BluFi Profile 使用 ESP-IDF 官方实现

### App 交互方式

手机 App 应按标准 BluFi 流程工作：

1. 扫描 BluFi 设备
2. 连接设备
3. 完成 BluFi 安全协商
4. 下发 Wi-Fi SSID / Password
5. 请求设备连接目标 Wi-Fi
6. 接收设备回报的 Wi-Fi 状态

### BluFi 安全设计

当前采用 ESP-IDF 官方 BluFi 安全流程：

1. DH 参数协商
2. 计算共享密钥
3. 用 `MD5` 派生会话 `PSK`
4. 用 `AES-CFB128` 加解密 BluFi 数据
5. 用 `CRC16` 做包校验

注意：

- 没有一个写死在固件里的固定蓝牙 PIN
- 没有一个固定的数据传输密钥
- 密钥是每次会话协商生成的

## Wi-Fi

### 当前策略

项目当前实际采用的是：

- 启动时扫描已保存 SSID
- 优先连接匹配列表里信号最强的目标
- 扫描未命中时，回退到第一条有效保存配置直连

### BluFi 配网时的例外

BluFi 下发新的 SSID/Password 后，固件会：

1. 先写入 `app_config / NVS`
2. 再通过 `wifi_manager_connect_specific_sta()` 直连本次下发的目标热点

这样不会因为“保存列表里还有别的热点更强”而把本次配网目标切走。

## 屏幕状态页

当前状态页为 `320x240` 横屏全屏调试页，显示：

### Wi-Fi 区

- `STA`
- `SSID`
- `RSSI`
- `CH`
- `PHY`
- `STA IP`
- `AP`
- `AP IP`

### BLE 区

- `BLE`
- `NAME`
- `CONN`
- `NOTIFY`
- `ADV`
- `NET`

### 云端区

- `PUB IP`
- `AREA`
- `WEATHER`
- `TIME`
- `TZ`

### OTA 区

- `OTA`
- `OTA PROG`

## 目录结构

```text
main/
├─ main.c
├─ common/
│  ├─ app_config.c/.h
│  ├─ app_log.h
│  ├─ cloud_service.c/.h
│  ├─ ota_service.c/.h
│  └─ ui_status.c/.h
├─ drivers/
│  ├─ wifi_manager.c/.h
│  ├─ display_st7789.c/.h
│  └─ font_gb2312.c/.h
├─ middle/
│  ├─ ble_provision.c/.h
│  ├─ blufi_security.c/.h
│  └─ mesh_audio_node.c
└─ app/
   ├─ mesh_audio_gateway.c/.h
   ├─ mesh_audio_controller.c/.h
   └─ mesh_audio_speaker.c/.h
```

## 关键模块

### `main/main.c`

- 系统启动入口
- 初始化 NVS / Cloud / OTA / Display / Wi-Fi / BluFi

### `main/middle/ble_provision.c`

- 标准 BluFi 接入层
- 管理 BLE 广播、BluFi 回调、Wi-Fi 配网联动

### `main/middle/blufi_security.c`

- BluFi 安全协商
- DH / AES / CRC16

### `main/drivers/wifi_manager.c`

- 管理 `WIFI_MODE_APSTA`
- 自动连接已保存热点
- 提供 `wifi_manager_connect_specific_sta()` 给 BluFi 定向配网使用

### `main/common/cloud_service.c`

- 获取公网 IP
- 地理定位
- SNTP 时间
- 天气刷新

### `main/common/ota_service.c`

- `manifest + HTTPS + Range + SHA256 + rollback`

## 分区

当前按 `4MB Flash` 使用，分区表见 [partitions.csv](C:/Users/Zc/Desktop/esp32-s3_test/t1/partitions.csv)。

当前布局：

- `nvs`
- `otadata`
- `phy_init`
- `ota_0`
- `ota_1`
- `coredump`

## 日志

统一日志宏在：

- [main/common/app_log.h](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/app_log.h)

关键日志会带：

- 文件名
- 行号
- 函数名

当前重点日志覆盖：

- Wi-Fi 连接/断开/重连
- BluFi 连接和 Wi-Fi 状态上报
- 云端请求过程
- OTA 状态流转

## 说明

- 当前手机 App 如果是标准 BluFi App 或基于 BluFi 改的 App，应直接对接当前固件
- 如果还要保留自定义 BLE 指令、状态查询、OTA 命令，建议后续在 BluFi 之外单独增加一个自定义服务，不要再和配网流程混用
