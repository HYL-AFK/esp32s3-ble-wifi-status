# BLE 协议整理

## 1. 连接入口

- 广播名固定为 `BMSxxxxxx`
- GATT Service: `FFF0`
- 写入特征: `FFF1`
- 通知特征: `FFF2`

## 2. 两种写入方式

### 2.1 文本命令

手机蓝牙调试助手可以直接写入 ASCII 文本，例如：

- `CFG=ssid,password`
- `NAME=xxx`
- `APNAME=xxx`
- `APPASS=12345678`
- `CLEAR_WIFI`
- `CLEAR`
- `STATUS`
- `REFRESH_CLOUD`

说明：

- `CFG=ssid,password` 用于保存并连接 STA Wi-Fi
- `NAME=xxx` 保存 BLE 用户显示名，不改广播发现名
- `STATUS` 用于查询当前 Wi-Fi / BLE / 云端状态
- `REFRESH_CLOUD` 用于手动触发一次公网 IP、定位、天气刷新

### 2.2 二进制帧

正式协议使用二进制帧：

```text
AA 55 | length(2) | mac(6) | cmd(1) | seq(1) | data(N) | crc16(2) | 55 AA
```

字段说明：

- `length`：只包含 `mac + cmd + seq + data`
- `mac`：目标设备 BLE MAC，用于设备过滤
- `cmd`：命令字
- `seq`：请求序号，用于应答对应
- `data`：命令数据区
- `crc16`：覆盖 `length + payload`

## 3. 分包 / 组包流程

当前 BLE 的拆包 / 组包逻辑在 `main/middle/ble_provision.c`。

核心原则：

- 每次 `FFF1 Write` 都只当成一个“字节流片段”
- 不假设一次写入就是一整帧
- 允许手机端分多次把一帧写完

设备侧处理顺序：

1. 收到写入数据
2. 追加到 `s_rx_buffer`
3. 搜索帧头 `AA 55`
4. 读取 `length`
5. 判断当前缓冲是否已收齐整帧
6. 校验帧尾 `55 AA`
7. 校验 CRC16
8. 校验 `mac` 是否等于本机 BLE MAC
9. 按 `cmd` 分发命令
10. 返回 ACK 或 ERR

异常处理规则：

- 帧头不对：丢弃到下一个可能帧头
- 长度非法：丢弃 1 字节后重同步
- CRC 错误：丢弃 1 字节后重同步
- MAC 不匹配：整帧丢弃
- 缓冲溢出：清空缓冲，等待下一帧重新同步

## 4. 当前命令字

- `0x01` 设置 STA Wi-Fi
- `0x02` 设置 BLE 用户显示名
- `0x03` 设置 AP 名称
- `0x04` 设置 AP 密码
- `0x05` 清空 STA Wi-Fi
- `0x06` 查询状态
- `0x80` ACK
- `0x81` ERR

## 5. 状态查询返回

### 5.1 文本 `STATUS`

- 如果手机端已经订阅 `FFF2 Notify`，设备通过 Notify 回包
- 如果没有订阅 Notify，设备只打印日志，不主动返回

### 5.2 二进制 `0x06 QUERY_STATUS`

- 返回一帧 `ACK`
- `ACK` 的 `data` 区直接放 JSON 状态字符串
- 当前实现只回一次，不会再追加一个空 ACK

## 6. JSON 状态字段

当前状态 JSON 至少包含以下字段：

- `sta_connected`
- `sta_ip`
- `public_ip`
- `country`
- `region`
- `city`
- `district`
- `timezone`
- `local_time`
- `temp_c`
- `humidity`
- `weather_code`
- `wind_kmh`
- `geo_valid`
- `weather_valid`
- `time_valid`
- `updating`
- `error`

说明：

- `district` 目前只在定位接口真实返回时才有值
- `local_time` 来自 SNTP 同步后的本地维护时间
- `error` 用于记录最近一次云端刷新错误

## 7. 维护要点

- 改 BLE 协议：优先改 `ble_provision.c`
- 改云端状态字段：优先改 `cloud_service.h/.c`
- 改 Wi-Fi 状态来源：优先改 `wifi_manager.c`
- 改屏幕显示：优先改 `ui_status.c`

## 8. 当前限制

- 文本命令更适合调试，不适合正式 App 协议
- 状态 JSON 目前控制在单帧可承载范围内
- 如果后续状态字段继续增多，建议把 `QUERY_STATUS` 改成：
  - 分页回读
  - 多帧分片回读
  - 或者拆成多个独立查询命令

## 9. OTA 调试命令

OTA 使用 `manifest.json + HTTPS + Range` 方案，不通过 BLE 直接传固件。

文本命令：

- `OTA_START <manifest_url>`：从 HTTPS manifest 开始 OTA。
- `OTA_STATUS`：回读 OTA 状态片段。
- `OTA_CANCEL`：取消当前 OTA，并清除断点状态。
- `OTA_RESUME`：手动恢复未完成 OTA。
- `OTA_REBOOT`：OTA 校验成功后重启进入新固件。

`STATUS` JSON 会追加 OTA 字段：

- `ota_state`
- `ota_progress`
- `ota_version`
- `ota_written`
- `ota_total`
- `ota_error`
- `ota_resume_valid`

manifest 示例：

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
