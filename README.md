# ESP32-S3FH4R2 BLE + Wi-Fi 配网工程说明

## 项目概览

本项目面向 `ESP32-S3FH4R2`，当前主流程集成了以下能力：

- 基于 NimBLE 的 BLE 配网
- `Wi-Fi APSTA` 模式
- `ST7789` SPI 屏幕驱动
- `LVGL` 全屏状态页
- 云端信息获取：
  - 公网 IP
  - IP 地理位置
  - `SNTP` 本地时间
  - 天气信息

当前上电后的主流程如下：

1. 设备上电
2. 启动 BLE 广播
3. 启动 Wi-Fi `APSTA`
4. 手机或上位机通过 BLE 下发 Wi-Fi 信息
5. 设备用 STA 连接路由器或热点
6. `STA got IP` 后自动拉取云端信息
7. 在屏幕上显示 Wi-Fi / BLE / 云端状态

## 主要功能

- BLE 广播名固定为 `BMSxxxxxx`
- BLE GATT 服务：
  - `FFF0` 服务
  - `FFF1` 写命令特征
  - `FFF2` 通知特征
- 同时支持两套 BLE 协议：
  - 文本命令协议，方便蓝牙调试助手测试
  - 二进制帧协议，方便正式 App 或设备接入
- Wi-Fi 配置最多保存 5 组 STA 信息到 NVS
- 启动后优先扫描并连接已保存列表中信号最强的 Wi-Fi
- 常驻 SoftAP，便于兜底配网
- 屏幕上显示：
  - 公网 IP
  - 地区
  - 天气文案 + 温度
  - 本地时间
- 分区已切换为 `A/B OTA` 预留布局，并保留 `coredump`
- 关键模块已接入统一日志封装，日志自带文件名、行号、函数名

## 目录结构

```text
.
|-- main/
|   |-- main.c
|   |-- common/
|   |   |-- app_config.c/.h
|   |   |-- app_log.h
|   |   |-- cloud_service.c/.h
|   |   `-- ui_status.c/.h
|   |-- drivers/
|   |   |-- wifi_manager.c/.h
|   |   |-- display_st7789.c/.h
|   |   `-- font_gb2312.c/.h
|   |-- middle/
|   |   `-- ble_provision.c/.h
|   `-- ble_protocol.md
|-- partitions.csv
|-- sdkconfig.defaults
`-- README.md
```

## 主要模块说明

### `main.c`

职责：

- 初始化 NVS
- 加载持久化配置
- 初始化云端服务
- 初始化屏幕和 LVGL 状态页
- 启动 BLE 配网和 Wi-Fi 管理

### `common/app_config.*`

职责：

- 定义应用配置结构体
- 从 NVS 加载和保存配置
- 保存以下信息：
  - BLE 显示名
  - AP 名称和密码
  - 多组 STA Wi-Fi 配置

### `common/app_log.h`

职责：

- 对 `ESP_LOGx` 做统一封装
- 自动在日志前缀中补充：
  - 文件名
  - 行号
  - 函数名
- 为后续 BLE/UI 回读日志或日志持久化预留统一入口

### `drivers/wifi_manager.*`

职责：

- 管理 `WIFI_MODE_APSTA`
- 启动和维护兜底 SoftAP
- 扫描并连接已保存 Wi-Fi 中信号最强的一项
- 对外提供运行时状态，供 BLE / UI / 云端模块读取

### `middle/ble_provision.*`

职责：

- 启动 NimBLE Host
- 注册 `FFF0 / FFF1 / FFF2`
- 处理 BLE 文本命令和二进制命令
- 通过 `FFF2` 返回 ACK / ERR / 状态数据

### `common/cloud_service.*`

职责：

- 启动 `SNTP`
- 通过 HTTPS 获取公网 IP
- 根据公网 IP 获取地理位置
- 根据经纬度获取天气
- 维护一份统一状态快照，供 BLE 和 UI 读取

### `common/ui_status.*`

职责：

- 在 `ST7789` 上绘制 LVGL 全屏状态页
- 显示：
  - Wi-Fi 状态
  - BLE 状态
  - 云端状态摘要

### `drivers/display_st7789.*`

职责：

- 初始化 `ST7789` SPI 屏幕
- 提供底层显示能力
- 作为 LVGL 的显示硬件层

## BLE 协议说明

### 文本命令

常用示例：

- `CFG=ssid,password`
- `NAME=xxx`
- `APNAME=xxx`
- `APPASS=12345678`
- `CLEAR_WIFI`
- `STATUS`
- `REFRESH_CLOUD`

说明：

- `NAME=xxx` 保存的是“用户显示名”
- 当前 BLE 扫描广播名仍固定为 `BMSxxxxxx`

### 二进制帧协议

帧格式：

```text
AA 55 | length(2 LE) | mac(6) | cmd(1) | seq(1) | data(N) | crc16(2 LE) | 55 AA
```

命令字：

- `0x01` 设置 STA Wi-Fi
- `0x02` 设置 BLE 显示名
- `0x03` 设置 AP 名称
- `0x04` 设置 AP 密码
- `0x05` 清除 STA Wi-Fi
- `0x06` 查询状态
- `0x80` ACK
- `0x81` ERR

更详细的协议说明见 [main/ble_protocol.md](main/ble_protocol.md)。

## 云端数据链路

在 `STA got IP` 之后，当前流程如下：

1. 启动或保持 `SNTP`
2. 请求公网 IP
3. 根据公网 IP 请求地理位置
4. 根据经纬度请求天气
5. 更新屏幕和 BLE 状态快照

当前数据来源：

- 公网 IP：
  - `https://api.ipify.org?format=json`
  - 失败回退 `https://api64.ipify.org?format=json`
  - 再失败回退 `https://ifconfig.co/json`
- 地理位置：
  - `https://ipapi.co`
- 天气：
  - `https://api.open-meteo.com`

## 地区精度说明

当前项目使用的是 **公网 IP 定位**，不是 GPS。

因此显示精度受公网 IP 库限制，通常以市级最可靠，区级结果不一定稳定。

当前 `AREA` 字段显示优先级如下：

1. `district/city/region`
2. `city/region`
3. `city`
4. `region`
5. `country`

实际交付时，应默认按“城市级定位”来理解，不应把它当作高精度地理定位。

## 屏幕状态页说明

当前全屏状态页主要分三块：

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

### 云端区

底部云端区按调试优先方式拆成 4 条独立单行：

- `PUB IP:`
- `AREA:`
- `WEATHER:`
- `TIME:`

说明：

- `WEATHER` 显示英文天气短词 + 温度
- `TIME` 在 SNTP 未同步前固定显示 `--`
- 当前界面优先保证“值能完整看见”，不以美观为主

## 分区说明

当前 Flash 按 `4MB` 使用，自定义分区表见 [partitions.csv](partitions.csv)。

当前布局为：

- `nvs`
- `otadata`
- `phy_init`
- `ota_0`
- `ota_1`
- `coredump`

对应说明：

- `ota_0 / ota_1`：后续 OTA 升级双槽位
- `otadata`：OTA 启动信息记录
- `coredump`：异常崩溃转储

注意：

- 这次只是把分区调整为可承载 OTA 的结构
- 本轮**没有**实现 OTA 下载、校验、写入、切换逻辑

## 日志说明

当前关键模块已经开始接入统一日志封装，入口文件是 [main/common/app_log.h](main/common/app_log.h)。

日志前缀会自动带上：

- 文件名
- 行号
- 函数名

常用宏：

- `APP_LOGI(TAG, "...")`
- `APP_LOGW(TAG, "...")`
- `APP_LOGE(TAG, "...")`
- `APP_LOGD(TAG, "...")`
- `APP_LOG_ERR(TAG, err, "...")`
- `APP_LOG_STATE(TAG, "...")`
- `APP_LOG_HEX(TAG, title, buf, len)`

当前第一批已接入的模块：

- `main.c`
- `wifi_manager.c`
- `ble_provision.c`
- `cloud_service.c`

当前日志重点增强的内容：

- Wi-Fi 自动连接决策
- Wi-Fi 断开原因和重连路径
- BLE 二进制帧收发
- BLE CRC / MAC 校验失败原因
- 云端刷新阶段和关键字段

## 当前默认值

### BLE

- 广播名：
  - `BMS` + BLE MAC 后 3 字节

### SoftAP

- 默认名称：
  - `ESP32S3-SETUP`
- 默认密码：
  - `12345678`

## 备注

- 当前主维护路径仍然是 `Wi-Fi + BLE 配网`
- 仓库里虽然还有 Mesh 相关文件，但当前主流程并不依赖 Mesh
- 天气、时间、地区的刷新速度受外网 HTTPS 接口和网络质量影响
- 如果手机热点、公司 Wi-Fi、运营商网络环境不同，公网 IP 和 HTTPS 成功率可能会有差异

## Wi-Fi 策略说明

当前项目实际采用的策略是：

- 扫描已保存 SSID
- 优先连接信号最强的目标
- 扫描未命中时，回退第一条有效保存配置直连

常见 Wi-Fi 策略的优缺点：

### 1. 扫描最强优先

优点：
- 更容易连到信号更好的 AP
- 对多热点环境更直接

缺点：
- 首次扫描会多花一点时间
- 多个热点来回变化时，连接选择可能波动

### 2. 粘性连接

优点：
- 重启后更容易回到上一次成功的热点
- 适合热点固定的场景

缺点：
- 热点信号变差时容易卡住
- 多热点环境里切换不够灵活

### 3. 按保存顺序直连

优点：
- 逻辑最简单
- 行为最稳定可预测

缺点：
- 不会自动挑更好的信号
- 前面的配置失败时恢复较慢

### 4. 重试后再切换

优点：
- 对“第一次失败、后面能成功”的热点更友好
- 能减少来回切站

缺点：
- 逻辑更复杂
- 需要维护重试计数和节流状态

### 5. 质量评分策略

优点：
- 最适合复杂现场
- 可以综合 RSSI、失败次数、上次成功时间

缺点：
- 维护成本最高
- 调试时不如简单策略直观

## OTA 升级方案

当前工程已接入成熟版 OTA：`manifest.json + HTTPS + HTTP Range + SHA256 + A/B 回滚`。

### OTA manifest

服务器需要提供 JSON：

```json
{
  "project": "t1",
  "chip": "esp32s3",
  "version": "1.0.1",
  "url": "https://example.com/firmware/t1-1.0.1.bin",
  "size": 1536000,
  "sha256": "64位小写hex",
  "force": false
}
```

校验规则：

- `project` 必须匹配当前固件 `project_name`。
- `chip` 必须是 `esp32s3`。
- `url` 必须是 HTTPS。
- `size` 必须小于 OTA 目标分区大小。
- `sha256` 必须和下载后 OTA 分区内容一致。
- 新固件内的 `project_name/version` 必须和 manifest 匹配。

### BLE OTA 命令

- `OTA_START <manifest_url>`：开始 OTA。
- `OTA_STATUS`：查询 OTA 状态。
- `OTA_CANCEL`：取消 OTA 并清除断点。
- `OTA_RESUME`：手动恢复断点。
- `OTA_REBOOT`：OTA 成功后重启进入新固件。

### 断点续传

OTA 状态保存在 NVS namespace `ota_state`。下载中断后，设备联网时会尝试自动恢复。服务器必须支持 HTTP Range，并在续传请求时返回 `206 Partial Content`。如果服务器不支持 Range，设备会清除旧断点并从头下载。

### 回滚

`sdkconfig.defaults` 已启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`。新固件启动后，主流程初始化成功会调用 `esp_ota_mark_app_valid_cancel_rollback()` 标记固件有效；如果新固件启动失败或未确认，bootloader 会回滚到旧固件。
