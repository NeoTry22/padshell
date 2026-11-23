# PadShell — iPadOS 17 全系可用的 iPad 网页远程控制基座

**English**: PadShell is an open-source foundation for controlling an iPad from a web console:
an ESP32-S3 acts as a BLE HID mouse (pointer + click driven by HTTP calls), while a ReplayKit
broadcast upload extension streams the iPad screen back to the ESP32 for live feedback.
Works on **iPadOS 17.x and later — no iOS 26 requirement, no jailbreak**.

把一台 ~¥50 的 ESP32-S3 变成 iPad 的蓝牙鼠标：网页发屏幕分数坐标 → ESP32 BLE HID 精准移动+点击 →
iPad 侧 ReplayKit 广播扩展把实时画面回传 ESP32 图床 → 网页准实时查看（实测 1.6fps / 延迟 ~0.7-1s）。
**全程只依赖公开 API，无需越狱，无需 iOS 26**（对比 ScreenCaptureKit 拾取器路线的系统版本门槛）。

## 特性

| 能力 | 说明 |
|---|---|
| 网页控制台 | ESP32 自托管单页：点图即点击、实时画面、状态/遥测面板 |
| BLE HID 鼠标 | Boot + Report 双通道；相对位移跟踪，绝不归零到屏幕边缘（规避 iPadOS 手势劫持） |
| 实时画面 | ReplayKit 广播上传扩展 → JPEG → ESP32 图床；帧率/长边/质量全部运行时可调 |
| 仪器级校准 | 标定页点击回传 `/calrep` 闭环校准，双点验证误差 <±0.4% |
| 可观测性 | `upok/upfail/a403/banhits/fid` 遥测 + 帧号增量协议（`/shotf?since=N`） |
| 外网访问 | 配合任意常开主机跑 Tailscale 子网路由，外地如同局域网（见 docs/04） |

## 架构

```
浏览器 ──HTTP(点击/拉帧)──▶ ESP32-S3 ──BLE HID──▶ iPadOS 指针
   ▲                          ▲                    │
   └──────JPEG 帧(图床轮询)────┘──ReplayKit 广播上传──┘
```

视频流始终留在局域网内（iPad → ESP32），外网只承载控制台 HTTP——配合 Tailscale 隧道，
暴露面为零端口、全加密。

## 硬件

| 件 | 说明 |
|---|---|
| ESP32-S3-DevKitC-1 **N16R8**（16MB Flash / 8MB OPI PSRAM） | 主控，¥40-60 |
| 支持 BLE 的 iPad | 实测 iPadOS 17.7.x；理论上 iPadOS 13+ 可跑（广播扩展 iOS 12+，未全测） |
| USB 数据线（烧录用）、2.4G WiFi | — |

## 快速开始

1. **固件**：装 [PlatformIO](https://platformio.org/) → 改 `firmware/src/main.cpp` 顶部三个常量
   （`WIFI_SSID` / `WIFI_PASS` / `AUTH_TOKEN`——后两者务必改成你自己的强随机值）→
   `pio run -e esp32s3 -t upload`。串口(115200)会打印控制台地址。
2. **iPad 端**：见 [ipadclient/README.md](ipadclient/README.md)——GitHub Actions 免费编译
   未签名 IPA → Sideloadly 个人签名（免费证书 7 天一签）→ 设置里信任证书 → 开始广播。
3. **配对**：iPad 设置 → 蓝牙 → 点 `ESP32-PadMouse` 完成配对；之后断电重启自动回连。
4. **控制**：浏览器开 `http://<esp32-ip>/?token=<你的token>` → 点画面即点击。
5. **校准**：见 [docs/02-校准与坐标系统.md](docs/02-校准与坐标系统.md)（一次 10 分钟，仪器级精度）。

更多文档：[docs/](docs/)（快速开始 / 校准 / 帧率与性能 / 外网访问 / 故障排查）。

## 兼容性说明

- **实测环境**：iPadOS 17.7.x + Arduino-ESP32 3.3.x + NimBLE-Arduino 2.2.0。
- 广播上传扩展是 **iOS 12+ 公开 API**；ScreenCaptureKit 拾取器（iOS 26 才有的官方路线）不使用。
- iPadOS 的指针加速非线性 → 首次使用必须校准；**「指针速度」滑块改动后需重校**（见 docs/02）。

## 安全与隐私

- **数据边界**：广播画面只上传到你**局域网内自己的 ESP32**，无云端、无遥测上报、无第三方服务。
- **鉴权**：所有 HTTP 路由要求 token；同一 IP 连错 5 次拉黑 10 分钟。请务必把默认 token 换成强随机串。
- **加密通道**：外网访问推荐 Tailscale（WireGuard 端到端加密、零端口暴露），见 docs/04。
- **隐私提醒**：广播扩展捕获的是**整块屏幕**。请勿在广播开启时展示敏感信息；
  不要把 ESP32 控制台暴露到公网明文访问。

## 法律与合规

- 本项目仅使用 Apple 公开 API（ReplayKit 广播、BLE HID），**无越狱、无私有 API**。
- 本项目仅供**控制你拥有或获得明确授权的设备**；未经授权控制他人设备属违法行为。
- iPad 是 Apple Inc. 的商标，本项目与 Apple 无关联。仓库/产品名刻意避开 Apple 商标。
- 本仓库只发布源码与 CI 工作流；**请勿再分发编译产物（IPA）**——签名安装请走个人开发者流程
  （Sideloadly/AltStore 等，遵守 Apple 开发者条款）。

## 致谢

- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)（Apache-2.0）——BLE HID 协议栈。
- [Arduino-ESP32](https://github.com/espressif/arduino-esp32)（LGPL-2.1，经 pioarduino 发行）——核心框架。
- AgentMouse（ESP-IDF 参考实现）——架构思路借鉴，未复制代码。
- 详见 [THIRD-PARTY.md](THIRD-PARTY.md)。

## License

MIT — 见 [LICENSE](LICENSE)。
