# ESP32-S3 BLE + Wi-Fi + Cloud Status 项目梳理

## 1. 项目定位

这是一个基于 `ESP32-S3FH4R2` 的嵌入式联网终端项目，核心目标是：

- 通过 `BLE` 完成设备近场配网和状态回读
- 通过 `Wi-Fi APSTA` 实现设备联网和兜底热点
- 通过公网接口获取 `公网 IP / 地理位置 / 时区 / 天气`
- 通过 `SNTP` 维护本地时间
- 通过 `ST7789 + LVGL` 在屏幕上实时显示设备状态

从工程结构看，它不是单纯的点屏或配网页，而是一套完整的“设备接入 + 联网 + 云端信息拉取 + 本地显示 + BLE 回读”的端侧系统。

---

## 2. 简历上可以怎么写

### 2.1 一句话版本

基于 `ESP32-S3` 独立完成 BLE 配网、Wi-Fi APSTA 联网、云端天气/时区信息拉取、ST7789 状态显示和 OTA 分区规划，形成可量产扩展的嵌入式联网终端基础框架。

### 2.2 项目描述版本

负责开发一套基于 `ESP32-S3FH4R2` 的嵌入式联网终端系统，采用 `NimBLE + Wi-Fi APSTA + ST7789 + LVGL` 架构，实现了手机 BLE 配网、Wi-Fi 自动连接、BLE 状态回读、云端公网 IP/定位/天气信息获取、本地时间同步显示，以及双 OTA 槽位分区与统一日志封装，为后续量产和功能扩展提供基础平台。

### 2.3 简历亮点版本

- 设计并实现 `BLE GATT + 二进制协议`，支持文本命令与二进制帧两套控制路径
- 实现 `Wi-Fi APSTA` 双角色模式，支持设备热点兜底和 STA 自动联网
- 接入 `HTTPS + SNTP` 云端链路，获取公网 IP、地理位置、时区、天气并形成统一状态快照
- 基于 `ST7789 + LVGL` 实现全屏英文状态面板，实时展示 Wi-Fi、BLE、天气、时间、时区等信息
- 规划 `4MB Flash A/B OTA` 分区并封装统一日志宏，提升后续 OTA 和现场调试能力

---

## 3. 技术栈

- 主控：`ESP32-S3FH4R2`
- SDK：`ESP-IDF`
- 蓝牙：`NimBLE`
- Wi-Fi：`esp_wifi` + `APSTA`
- 显示：`esp_lcd` + `ST7789`
- UI：`LVGL`
- 网络：`esp_http_client`
- 时间同步：`esp_netif_sntp`
- 配置存储：`NVS`
- JSON：`cJSON`

---

## 4. 项目整体流程

## 4.1 上电主流程

主入口在 [main/main.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/main.c:49)。

上电后的流程是：

1. 初始化 `NVS`
2. 从 `NVS` 读取本地持久化配置
3. 初始化云端信息服务
4. 初始化 `ST7789` 屏幕和 `LVGL` 状态页
5. 启动 `BLE` 配网服务
6. 启动 `Wi-Fi APSTA`
7. 如果本地已保存 Wi-Fi，则自动尝试联网
8. 联网成功后自动触发公网 IP、地理位置、天气和时间同步
9. 屏幕和 BLE 都可以回读当前状态

---

## 4.2 模块关系

可以把项目分成五层：

### 1. 配置层

- `main/common/app_config.c/.h`

职责：
- 统一管理 NVS 持久化配置
- 保存 STA Wi-Fi 列表、BLE 显示名、AP 名称和密码

### 2. 通信层

- `main/middle/ble_provision.c/.h`
- `main/drivers/wifi_manager.c/.h`

职责：
- BLE 负责近场配置和状态回读
- Wi-Fi 负责 APSTA 双模式联网

### 3. 云端服务层

- `main/common/cloud_service.c/.h`

职责：
- 联网后拉取公网 IP、定位、天气
- 维护本地时间和统一状态快照

### 4. 显示层

- `main/drivers/display_st7789.c/.h`
- `main/common/ui_status.c/.h`

职责：
- 底层驱动屏幕
- 上层绘制 LVGL 状态页

### 5. 公共能力层

- `main/common/app_log.h`

职责：
- 统一日志输出格式
- 自动带文件名、行号、函数名

---

## 5. 我建议你在面试里怎么讲这个项目

可以按这个顺序讲：

### 5.1 先讲业务目标

“这个项目本质上是一个 ESP32-S3 的联网终端。设备本身没有键盘和复杂交互，所以我用 BLE 做近场配置，用 Wi-Fi 去联网，再把联网后的状态、天气、时间这些信息显示到 ST7789 屏上，同时也支持 BLE 回读这些状态，方便手机或者后续上位机读取。”

### 5.2 再讲系统架构

“我把它拆成配置层、BLE/Wi-Fi 通信层、云端服务层、显示层和日志层。这样 BLE 协议、Wi-Fi 策略、云端拉取逻辑、UI 展示都能独立维护，不会全塞进 main 里。”

### 5.3 再讲关键链路

“设备上电后先起 BLE 和 Wi-Fi APSTA，如果有历史 Wi-Fi 配置就自动连接。STA 拿到 IP 后，我会触发 cloud_service 去拉公网 IP、地理位置、时区和天气，同时启动 SNTP 维护时间，再把统一快照输出给屏幕和 BLE 查询接口。”

### 5.4 最后讲你做的技术点

“我重点做了 BLE 文本和二进制双协议、Wi-Fi 多配置扫描连接、云端 HTTPS 拉取和状态快照、ST7789+LVGL 状态页、OTA 分区和日志体系。”

---

## 6. 关键实现过程

## 6.1 配置持久化

配置模块在 [main/common/app_config.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/app_config.c:1)。

实现点：

- 通过 `NVS` 保存最多 5 组 `STA Wi-Fi`
- 支持保存：
  - `ble_name`
  - `ap_name`
  - `ap_pass`
  - 多组 `wifi_profiles`
- 提供默认值初始化、读取、保存、清空 Wi-Fi、清空全部配置

这个模块的价值在于：

- 业务层不直接操作 NVS key
- 持久化逻辑集中，后续加字段不容易失控

---

## 6.2 BLE 配网和状态回读

BLE 模块在 [main/middle/ble_provision.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:797)。

### 架构

- GATT Service：`FFF0`
- 写入特征：`FFF1`
- 通知特征：`FFF2`

### 两套协议

#### 1. 文本命令

适合调试：

- `CFG=ssid,password`
- `NAME=xxx`
- `APNAME=xxx`
- `APPASS=12345678`
- `CLEAR_WIFI`
- `STATUS`
- `REFRESH_CLOUD`

#### 2. 二进制协议

协议头尾和结构已经梳理在 [main/ble_protocol.md](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/ble_protocol.md:1)。

帧格式：

```text
AA 55 | length(2) | mac(6) | cmd(1) | seq(1) | data(N) | crc16(2) | 55 AA
```

关键点：

- 支持分包写入，不假设一次写入就是一整帧
- 用接收缓冲区重组完整帧
- 校验：
  - 帧头
  - 长度
  - 帧尾
  - CRC16
  - 目标 MAC
- 命令执行后通过 `FFF2 Notify` 返回 `ACK/ERR`

### 关键实现能力

- `send_frame()` 负责二进制回包 [main/middle/ble_provision.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/middle/ble_provision.c:181)
- `QUERY_STATUS(0x06)` 返回 JSON 状态
- `STATUS` 文本命令也能回读设备状态

### 这个点在面试里容易加分

因为它体现了你不是只会调 GATT，而是考虑了：

- 分包
- 粘包
- 校验
- ACK/ERR
- 设备 MAC 过滤
- 调试协议和正式协议共存

---

## 6.3 Wi-Fi APSTA 联网

Wi-Fi 模块在 [main/drivers/wifi_manager.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/drivers/wifi_manager.c:197)。

### 模式

项目工作在 `WIFI_MODE_APSTA`：

- `AP`：设备自己开热点，便于未联网时配置
- `STA`：连接外部路由器或手机热点

### 当前连接策略

当前实现是：

- 扫描已保存 SSID
- 对扫描结果打分
- 优先连接信号最强的已保存热点
- 如果扫描没命中，再回退第一条有效保存配置直连

对应函数：

- 扫描打分：[main/drivers/wifi_manager.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/drivers/wifi_manager.c:311)
- 发起连接：[main/drivers/wifi_manager.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/drivers/wifi_manager.c:393)

### 事件处理

事件入口在 [main/drivers/wifi_manager.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/drivers/wifi_manager.c:128)。

主要处理：

- `WIFI_EVENT_STA_START`
- `WIFI_EVENT_STA_DISCONNECTED`
- `WIFI_EVENT_AP_START`
- `IP_EVENT_STA_GOT_IP`

### 设计上的考虑

- AP 始终作为兜底入口存在
- STA 拿到 IP 后刷新运行时信息：SSID、RSSI、信道、PHY、IP
- 断开后按当前保存配置重新评估连接目标

---

## 6.4 云端信息服务

云端服务在 [main/common/cloud_service.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/cloud_service.c:675)。

### 它做的事情

1. 获取公网 IP
2. 根据公网 IP 获取地理位置和时区
3. 根据经纬度获取天气
4. 启动 SNTP，同步本地时间
5. 维护统一状态快照
6. 给 BLE 和 UI 读取

### 外部接口

- `ipify`
- `ipapi.co`
- `open-meteo`
- `SNTP`

### 刷新策略

- `STA got IP` 后自动触发刷新
- 定位信息按联网/IP变化/手动触发刷新
- 天气默认 `15 分钟` 刷新一次
- 时间由 `SNTP + 本地维护` 负责

关键实现：

- 启动 SNTP：[main/common/cloud_service.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/cloud_service.c:379)
- 公网 IP：[main/common/cloud_service.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/cloud_service.c:404)
- 地理位置：[main/common/cloud_service.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/cloud_service.c:446)
- 天气：[main/common/cloud_service.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/cloud_service.c:499)
- 刷新主流程：[main/common/cloud_service.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/cloud_service.c:557)

### 快照结构

状态统一收口在 `cloud_service_snapshot_t`：

- 联网状态
- 内网 IP / 公网 IP
- 国家、省、市、区
- 时区
- 本地时间字符串
- 温度、湿度、风速、天气码
- 错误信息

这类快照式设计很适合面试里讲，因为它体现了：

- 业务数据源和显示层解耦
- BLE / UI 共用一份数据源
- 后续可扩展性较好

---

## 6.5 屏幕和 UI

### 屏幕底层

驱动文件在 [main/drivers/display_st7789.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/drivers/display_st7789.c:1)。

实现点：

- 使用 `esp_lcd` 驱动 `ST7789`
- 提供清屏、矩形填充、ASCII 文本绘制、GB2312 文本绘制接口
- 屏幕异常不阻塞 Wi-Fi/BLE 主流程

### 状态页

UI 文件在 [main/common/ui_status.c](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/ui_status.c:287)。

页面内容：

- 左侧：Wi-Fi 状态
- 右侧：BLE 状态
- 底部：云端摘要
  - `PUB IP`
  - `AREA`
  - `WEATHER`
  - `TIME`
  - `TZ`

### 为什么这里用了 LVGL

不是为了做炫 UI，而是为了：

- 后续扩展更方便
- 刷新逻辑和底层屏幕绘制分开
- 增加字段比手写像素更容易维护

---

## 6.6 日志体系

日志封装在 [main/common/app_log.h](c:/Users/Zc/Desktop/esp32-s3_test/t1/main/common/app_log.h:1)。

做法：

- 对 `ESP_LOGI/W/E/D` 做统一封装
- 自动打印：
  - 文件名
  - 行号
  - 函数名

意义：

- 现场调试效率高
- BLE/Wi-Fi/Cloud 三条链路出问题时更容易定位

---

## 6.7 OTA 分区规划

分区表在 [partitions.csv](c:/Users/Zc/Desktop/esp32-s3_test/t1/partitions.csv:1)。

当前采用 4MB Flash 双 OTA 槽位：

- `nvs`
- `otadata`
- `phy_init`
- `ota_0`
- `ota_1`
- `coredump`

目的：

- 先把后续 OTA 的基础形态搭好
- 支持 A/B 升级
- 出问题时保留 `coredump` 现场

这个点你在简历里可以写成：

“完成 4MB Flash 下双 OTA 槽位与 coredump 分区规划，为后续远程升级和异常排查预留基础设施。”

---

## 7. 项目难点和你可以强调的点

## 7.1 BLE 不是简单收发字符串

你做的不是一个“手机写字符串，设备直接 sscanf”的 Demo。

你这里有：

- GATT 服务设计
- 文本命令与二进制协议双通道
- 帧同步
- 分包重组
- CRC 校验
- ACK/ERR 机制
- JSON 状态回读

这是可以当成协议设计能力讲的。

## 7.2 Wi-Fi 不是单一联网

你不是只做 `esp_wifi_connect()`。

你做的是：

- APSTA 双角色
- 多热点保存
- 扫描选网
- STA 状态缓存
- 与 BLE 广播状态联动

## 7.3 云端链路不是单点接口调用

你不是单独打一条 HTTP。

你做的是：

- `STA got IP -> public IP -> geo -> weather -> time`
- 状态快照管理
- 周期刷新
- 超时和错误日志
- TLS 证书 bundle

## 7.4 显示层不是直接刷彩条

你不是只做了点屏。

你做的是：

- 屏幕底层驱动
- UI 中间层
- 数据与显示解耦
- 联网状态可视化

---

## 8. 面试官大概率会问的问题

## 8.1 项目整体类

### Q1：这个项目最核心的目标是什么？

答题思路：

核心目标是做一套可扩展的 ESP32-S3 联网终端基础框架，解决设备配网、联网、云端状态获取、本地显示和后续 OTA 扩展的问题。

### Q2：为什么用 BLE 配网，不直接让设备开 AP 配网页？

答题思路：

- BLE 配网对手机更轻量
- 不需要先切 Wi-Fi 网络
- 近场调试效率更高
- AP 仍然保留作为兜底

### Q3：为什么同时保留 APSTA？

答题思路：

- STA 负责业务联网
- AP 负责兜底和调试
- 即使 STA 连不上，设备也还能被近场接入

---

## 8.2 BLE 协议类

### Q4：为什么同时保留文本命令和二进制协议？

答题思路：

- 文本命令适合蓝牙调试助手快速联调
- 二进制协议适合正式 App 或设备接入
- 前期开发效率和后期协议规范都能兼顾

### Q5：BLE 二进制协议为什么要做 CRC？

答题思路：

- BLE GATT 写入虽然可靠，但应用层仍然需要防止帧拼接错位或数据污染
- CRC 可以帮助判断是否拿到完整有效帧

### Q6：为什么要校验 MAC？

答题思路：

- 防止广播环境里把其他设备的控制帧误处理
- 在多设备同时调试时更安全

### Q7：状态回读为什么选 JSON？

答题思路：

- 调试可读性高
- 手机蓝牙助手和上位机容易解析
- 当前字段变化频繁，JSON 比固定二进制结构更灵活

限制也要能说：

- 带宽效率不如二进制
- 字段继续增多时需要分页或拆帧

---

## 8.3 Wi-Fi 类

### Q8：你们的 Wi-Fi 连接策略是什么？

答题思路：

当前是“扫描最强 + 扫描未命中时回退第一条有效配置”。这样实现简单，调试直观，也适合当前保存多个热点但不希望过度设计状态机的阶段。

### Q9：为什么不做粘性连接或复杂评分策略？

答题思路：

- 当前项目阶段更需要稳定和可调试
- 复杂状态机会引入更多边界问题
- 现场主要问题已经判断更偏信号质量，而不是选网逻辑

### Q10：Wi-Fi 弱信号会带来哪些现象？

答题思路：

- 首次关联失败
- DHCP 拿 IP 慢
- HTTPS 请求超时、reset、DNS 失败增多
- 看起来像“云端接口失败”，但根因是链路质量差

---

## 8.4 云端链路类

### Q11：为什么先获取公网 IP 再做定位？

答题思路：

当前设备没有 GPS，也没有蜂窝基站定位能力，所以只能基于公网 IP 做近似定位。这种方法实现成本低，适合室内联网设备。

### Q12：公网 IP 定位能精确到什么程度？

答题思路：

通常城市级最可靠，区级结果不稳定。因为它依赖公网 IP 库，不是 GPS。

### Q13：为什么时间走 SNTP，不走 HTTP 时间接口？

答题思路：

- SNTP 是专门做时间同步的
- 同步后本地可以持续维护时间
- 不需要每次显示时间都重新发 HTTP
- 更省流量，也更稳定

### Q14：为什么天气不直接用城市名查，而是用经纬度？

答题思路：

- 经纬度更标准
- 避免城市同名和模糊匹配问题
- open-meteo 这类接口本身就更适合经纬度查询

---

## 8.5 显示和系统设计类

### Q15：为什么用 LVGL，而不是自己直接刷字符？

答题思路：

- 后续字段会继续增加
- 页面结构会改
- LVGL 更适合做布局和定时刷新
- 底层驱动和显示逻辑分层更清楚

### Q16：为什么显示层要读统一快照，而不是分别去调 Wi-Fi/BLE/HTTP 接口？

答题思路：

- UI 不应该关心数据来源
- 统一快照可以减少跨模块耦合
- BLE 和 UI 可以共用一套状态源

---

## 8.6 工程化类

### Q17：为什么要做统一日志宏？

答题思路：

- 现场调试时最重要的是快速定位
- 文件名、行号、函数名能显著减少排查时间
- 后续如果要接 BLE 回读日志或持久化日志，只改一层即可

### Q18：为什么提前做 OTA 分区，但还没实现 OTA 下载？

答题思路：

- 分区是 OTA 的基础设施
- 当前镜像已经比较大，先把空间和升级形态规划好
- 这样后面开发 OTA 逻辑时不用再回头大改 Flash 布局

---

## 9. 面试里可能追问的“深一层”问题

## 9.1 如果让你继续做下一步，你会做什么？

可以回答：

1. 做完整 OTA 下载和切换流程
2. 给 BLE 状态回读做分页或分帧
3. 给 Wi-Fi 增加更细的失败分类和统计
4. 给 cloud_service 增加缓存和退避机制
5. 把 Mesh 角色真正接入主流程

## 9.2 这个项目还有哪些可优化点？

可以回答：

- 文档编码和注释编码需要统一，避免乱码
- BLE 状态 JSON 未来可能超过单帧容量，需要扩协议
- 显示布局目前以调试优先，可继续优化适配性
- 云端接口依赖外部免费服务，量产阶段需要准备自有服务或备份源
- Wi-Fi 和云端任务的异常恢复策略还可以再细化

---

## 10. 你在简历上可以用的“项目职责”描述

如果你想写得更像“我做的事情”，可以直接用下面这版：

### 版本 A：偏开发内容

负责 `ESP32-S3FH4R2` 联网终端项目的软件开发，基于 `ESP-IDF` 实现 `BLE 配网 + Wi-Fi APSTA + 云端信息服务 + ST7789/LVGL 状态页`，完成 GATT 协议设计、Wi-Fi 多配置连接、HTTPS 天气/定位拉取、SNTP 时间同步、双 OTA 分区规划与统一日志体系建设。

### 版本 B：偏结果导向

独立搭建一套可扩展的嵌入式联网终端基础框架，使设备具备 BLE 配网、Wi-Fi 自动联网、状态回读、天气/时间显示和后续 OTA 扩展能力，显著提升了设备调试效率和后续功能迭代基础。

---

## 11. 你在面试时最好主动强调的三点

### 1. 这是完整链路，不是单点 Demo

不是只点亮了屏，也不是只连上了 Wi-Fi，而是把：

- 配置
- 联网
- 协议
- 云端数据
- 屏幕显示
- 调试日志

串成了一条完整链路。

### 2. 你做了工程化收口

比如：

- 配置集中到 `app_config`
- 日志集中到 `app_log`
- 云端数据集中到 `cloud_service_snapshot`
- 分区提前按 OTA 规划

### 3. 你能清楚解释取舍

比如：

- 为什么 BLE 同时保留文本和二进制
- 为什么时间走 SNTP
- 为什么 Wi-Fi 当前先用简单策略
- 为什么定位只能做到城市级为主

面试官通常不是只看你“做了什么”，而是看你能不能把“为什么这么做”讲清楚。

---

## 12. 结论

如果你把这个项目写进简历，建议你把它定位成：

**“基于 ESP32-S3 的嵌入式联网终端基础平台项目”**

不要只写成：

- BLE 配网 Demo
- ST7789 点屏 Demo
- 天气显示小项目

因为从现在这套代码的结构和功能看，它已经明显超过 Demo 级别了。更准确的讲法是：

- 设备接入能力
- 联网能力
- 云端信息获取能力
- 本地显示能力
- OTA 预研能力
- 调试与维护能力

如果你后面要，我可以继续给你补两份内容：

1. 一版“简历项目经历”精简成 3 到 5 条 bullet 的版本  
2. 一版“面试自我介绍/项目介绍口述稿”，按 3 分钟和 8 分钟两个版本整理
