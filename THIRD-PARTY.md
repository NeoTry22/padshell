# 第三方组件与许可声明 (THIRD-PARTY.md)

本仓库的原创代码以 MIT 许可发布（见 LICENSE）。固件在构建时链接以下第三方组件，
它们的许可条款适用于各自的代码，本仓库未复制其源码：

## 1. NimBLE-Arduino

- 用途：BLE HID over GATT 协议栈（鼠标设备端实现）
- 上游：https://github.com/h2zero/NimBLE-Arduino
- 许可：Apache License 2.0（`lib_deps = h2zero/NimBLE-Arduino@^2.2.0`，见 firmware/platformio.ini）
- 说明：作为 PlatformIO 依赖在构建期获取，本仓库不含其源码副本。

## 2. Arduino-ESP32 核心框架

- 用途：WiFi/HTTP 服务、任务调度等 Arduino API 与底层 ESP-IDF 组件
- 上游：https://github.com/espressif/arduino-esp32 （经 pioarduino 分支发行：
  https://github.com/pioarduino/platform-espressif32）
- 许可：LGPL-2.1（核心）与 ESP-IDF 各组件的 Apache-2.0
- 说明：同上，构建期获取。若你按本仓库说明编译并**分发固件二进制**，
  请遵守 LGPL-2.1 的对应义务（提供链接信息、允许用户替换库文件重新链接）。

## 3. Apple SDK（闭源，非开源组件）

- 用途：ipadclient（Swift）使用 ReplayKit（广播上传扩展）、UIKit/SwiftUI 等
- 说明：按 Apple 开发者计划条款随 Xcode/SDK 提供；本仓库不含 SDK 代码。

## 4. 参考但未复制

- AgentMouse（ESP-IDF 参考实现）：BLE HID 状态机**架构思路**借鉴，未复制任何代码。
- 本仓库所有原创代码（固件、控制台 HTML/JS、Swift 扩展与主 App、Python 工具）均为原创实现。

## 5. 运行期外部工具（不随本仓库分发）

- [Sideloadly](https://sideloadly.io/) —— 个人签名安装工具（其自身许可由其作者规定）
- Apple iTunes / iCloud 驱动 —— 按 Apple 官方渠道获取
- [PlatformIO](https://platformio.org/) —— 构建系统

## 6. Python 工具依赖（tools/）

- `tools/serial_probe.py` 依赖 `pyserial`（BSD-3-Clause），运行期 `pip install pyserial` 获取。
