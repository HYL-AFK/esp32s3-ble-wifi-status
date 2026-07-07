# BLE / Wi-Fi 通信协议说明

## 1. 当前方案总览

项目当前已经切换为标准 `BluFi` 配网方案，不再使用旧的自定义：

- `FFF0 / FFF1 / FFF2` 私有服务协议
- `CFG=ssid,password` 文本配网命令
- `AA55 ... 55AA` 二进制私有帧

现在手机和 ESP32-S3 的配网通信链路是：

1. 手机通过 `BLE GATT` 连接设备
2. 在 `BluFi` Profile 上完成安全协商
3. 手机下发 `SSID / Password / 可选 BSSID`
4. ESP32 用收到的参数去连接目标 Wi-Fi
5. 设备通过 BluFi 回报 Wi-Fi 连接状态

对应实现见：

- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c)
- [blufi_security.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/blufi_security.c)
- [wifi_manager.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/drivers/wifi_manager.c)

## 2. BLE 侧协议

### 2.1 传输层

- BLE Host：`NimBLE`
- 配网 Profile：`ESP-IDF BluFi`
- 广播名：`ESPARK-PowerGo`

初始化代码：

- `ble_provision_init()` 调用 BluFi 栈初始化  
  参考 [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:593)
- `blufi_on_sync()` 中设置设备名并启动 BluFi Profile  
  参考 [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:508)

### 2.2 GATT 结构

从你当前运行日志看，BluFi GATT 服务已经注册为：

- 标准 GAP 服务：`0x1800`
- 标准 GATT 服务：`0x1801`
- BluFi 服务：`0xFFFF`
  - 写特征：`0xFF01`
  - 通知特征：`0xFF02`

这部分是 ESP-IDF BluFi GATT Server 提供的，不是项目自己手写私有 GATT。

### 2.3 BluFi 业务事件

当前固件实际处理的 BluFi 事件主要有：

- `ESP_BLUFI_EVENT_RECV_STA_SSID`
- `ESP_BLUFI_EVENT_RECV_STA_PASSWD`
- `ESP_BLUFI_EVENT_RECV_STA_BSSID`
- `ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP`
- `ESP_BLUFI_EVENT_GET_WIFI_STATUS`
- `ESP_BLUFI_EVENT_GET_WIFI_LIST`

对应代码见：

- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:396)
- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:402)
- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:413)
- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:378)
- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:391)
- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:456)

## 3. BluFi 安全机制

### 3.1 不是固定 PIN，也不是固定密钥

当前这套方案里：

- **没有固定蓝牙配对 PIN 码**
- **没有写死在固件里的固定 Wi-Fi 传输密钥**
- **每次会话都会重新协商会话密钥**

原因是当前安全不是靠 BLE SMP 配对密码，而是靠 BluFi 自己的应用层安全。

NimBLE 安全配置见：

- `sm_sc = 0`
- `sm_bonding = 0`
- `sm_mitm = 0`

参考 [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:563)

这说明当前没有启用“手机蓝牙设置里常见的固定配对码/绑定密钥”那套流程。

### 3.2 当前使用的安全算法

`blufi_security.c` 里实现的是：

1. `DH` 密钥协商
2. 双方计算 `shared_key`
3. 对 `shared_key` 做 `MD5`
4. 取 16 字节结果作为本次会话 `session_psk`
5. 后续 BluFi 数据用 `AES-CFB128` 加解密
6. 包校验使用 `CRC16`

对应代码：

- DH 协商入口  
  [blufi_security.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/blufi_security.c:55)
- `MD5(shared_key) -> psk`  
  [blufi_security.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/blufi_security.c:140)
- `AES-CFB128` 加密  
  [blufi_security.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/blufi_security.c:168)
- `AES-CFB128` 解密  
  [blufi_security.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/blufi_security.c:189)
- `CRC16` 校验  
  [blufi_security.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/blufi_security.c:210)

### 3.3 你日志里的“密钥数据”是什么

你贴出来的这段日志：

```text
blufi shared_key len=128
blufi session_psk len=16
04 dd 42 1f 1b 61 59 5d 42 cb d0 97 94 dc 36 3f
```

说明：

- `shared_key`：DH 协商出的共享密钥，当前会话专用
- `session_psk`：用 `MD5(shared_key)` 算出来的 16 字节会话密钥
- `04 dd 42 1f 1b 61 59 5d 42 cb d0 97 94 dc 36 3f`：**只是这一次会话的临时密钥**

它不是固定值，下次重新连接会变。

## 4. Wi-Fi 侧协议

### 4.1 工作模式

项目当前固定工作在 `WIFI_MODE_APSTA`：

- 一边保留设备自己的 SoftAP
- 一边让 STA 去连接外部路由器

参考：

- [wifi_manager.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/drivers/wifi_manager.c:17)
- [wifi_manager.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/drivers/wifi_manager.c:453)

### 4.2 手机下发的 Wi-Fi 数据

BluFi 收到的核心 Wi-Fi 参数有：

- `STA SSID`
- `STA Password`
- `可选 BSSID`

参考：

- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:396)
- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:402)
- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:413)

你后来抓到的密码原始字节：

```text
73 70 61 72 6b 39 36 37 37
```

翻译成 ASCII 就是：

```text
spark9677
```

### 4.3 ESP32 连接 Wi-Fi 的方式

当 BluFi 收到 `REQ_CONNECT_TO_AP` 后，固件会把刚收到的 SSID/密码直接交给 Wi-Fi 驱动连接：

- `ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP`  
  [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:378)
- 直接连接指定热点  
  [wifi_manager.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/drivers/wifi_manager.c:657)

核心调用是：

- `esp_wifi_set_config(WIFI_IF_STA, &sta_cfg)`
- `esp_wifi_connect()`

### 4.4 你当前连接成功时的 Wi-Fi 形态

从你贴的成功日志看，当前连接形态是：

- 频段：`2.4GHz`
- 安全：`WPA2-PSK`
- 信道：`channel 2`
- PHY：`bgn / 11n`
- LAN IP：`192.168.1.162`
- 网关：`192.168.1.10`
- DNS1：`192.168.1.10`

这说明 ESP32 连的是普通 2.4G 家用/办公路由器模式，不是 5G。

## 5. 联网后的外网通信协议

ESP32 STA 拿到 IP 后，云端信息刷新走的是 `HTTPS + JSON`：

1. 查公网 IP
2. 按公网 IP 查地区 / 时区
3. 按经纬度查天气

实现见：

- 公网 IP  
  [cloud_service.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/cloud_service.c:500)
- 地区 / 时区  
  [cloud_service.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/cloud_service.c:542)
- 天气  
  [cloud_service.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/cloud_service.c:598)

当前用到的外网接口：

- `https://api.ipify.org?format=json`
- `https://api64.ipify.org?format=json`
- `https://ifconfig.co/json`
- `https://ipapi.co/<ip>/json/`
- `https://api.open-meteo.com/...`

所以这部分协议不是 MQTT，也不是 WebSocket，而是标准 `HTTPS REST` 请求。

## 6. 整体链路流程

### 6.1 BLE 配网流程

1. 设备启动，打开 `APSTA`
2. 启动 BLE 广播，名字为 `ESPARK-PowerGo`
3. 手机连接 BluFi GATT
4. 双方做 DH 密钥协商
5. 派生 `session_psk`
6. 手机发送 SSID / Password / BSSID
7. 手机发起“连接 AP”请求
8. ESP32 直接连接该热点
9. ESP32 通过 BluFi 回报连接状态

### 6.2 上网流程

1. Wi-Fi 关联成功
2. DHCP 获取内网地址
3. `cloud_service` 拉取公网 IP
4. 如果公网 IP 成功，再获取地区 / 时区 / 天气
5. UI 和日志刷新显示结果

## 7. 你现在最该注意的两个点

### 7.1 当前已经能看到密钥，但这是调试日志

你现在日志里已经能看到：

- `shared_key`
- `session_psk`
- Wi-Fi 明文密码字节

这些日志只适合联调，**不能保留到发布版**。

建议后续至少关掉这两处：

- [ble_provision.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:422)
- [blufi_security.c](C:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/blufi_security.c:21)

### 7.2 当前“蓝牙密钥”和“Wi-Fi 密钥”要分开理解

- 蓝牙链路层当前没有固定 PIN / Passkey
- BluFi 应用层有每次动态协商出的 `session_psk`
- Wi-Fi 本身仍然使用你下发给路由器的真实密码，例如 `spark9677`

这三者不是一个东西，不能混为一谈。

## 8. 一句话结论

当前项目的通信协议可以概括为：

- **手机到 ESP32 配网：BLE GATT + 标准 BluFi**
- **BluFi 安全：DH + MD5 派生 PSK + AES-CFB128 + CRC16**
- **ESP32 到路由器：2.4G Wi-Fi STA，当前实测为 WPA2-PSK**
- **ESP32 到公网服务：HTTPS + JSON**
