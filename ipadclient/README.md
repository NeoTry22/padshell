# PadShell — iPad 屏幕广播上传（iPadOS 17 可用版）

把 iPad 画面以 **1-2fps 持续上传到 ESP32 图床**，让现有控制台（`http://<esp32-ip>/`）变成准实时监控页。
无闪屏、无截图横幅、不需要第三方中控。

> 本版为项目早期 ScreenCaptureKit 原型的整体重写（SC 拾取器是 iOS 26 API，iPadOS 17.x 不可用，故回归 ReplayKit）。
> SC `SCContentSharingPicker` 是 iOS 26 API，iPadOS 17.7 实机不可用；故退回
> **ReplayKit 广播上传扩展（Broadcast Upload Extension）**——iOS 12+ 全系支持。

## 架构

```
iPad 屏幕 → 控制中心广播(RPBroadcastSampleHandler) → CI 降采样 JPEG(长边900/q0.5)
  → multipart POST http://<esp32>/upload?token=  → 图床 screenshot.jpg
  → 控制台 <img> 按 fps 自适应轮询 → 监控页
```

- **主 App（PadShell）**：放一个系统广播按钮（`RPSystemBroadcastPickerView`）+ 监控面板（轮询 `/status` 显示图床健康度）+ 调帧率（`/config?fps=`）。
- **广播扩展（PadBroadcast）**：真正干活的。拿到系统广播帧 → 节流到 fps → JPEG → POST 图床。
- **帧率谁说了算**：ESP32 `/status` 的 `fps` 字段。扩展每 20s 拉一次，网页 `/config?fps=2` 即调，扩展 20s 内生效，**无需重装 App**。
- **限流策略**：上传在途就丢帧（图床要"最新"不要"全量"），连续失败 30 次自动停止广播并报错。

## 已知限制（iOS 平台约束，无解）

1. **广播不能静默自启**：iPad 每次重启后要人到控制中心/本 App 点一次「开始广播」。
2. **免费签名 7 天过期**：过期后 App 打不开，需重新 Sideloadly 签名（数据不丢）。
3. 扩展上传的 ESP32 **地址/令牌编译期固定**（`Sources/Broadcast/SampleHandler.swift` 顶部两个常量），改 IP 要重编译；帧率则可远程调。
4. 广播启动时 iPad 顶部会出现红色胶囊/圆点（系统强制指示），无法隐藏。

## 构建 → 安装 → 使用

### 1. GitHub Actions 编译未签名 ipa

把 `ipadclient/` 整个目录推到一个 GitHub 仓库（含 `.github/workflows/`），
仓库页 → Actions → 「Build unsigned IPA」→ Run workflow →
跑完在该次运行底部下载 artifact `PadShell-unsigned-ipa`，得 `PadShell-unsigned.ipa`。

编译报错请对照 Actions 日志排查。常见坑：macOS runner 镜像变动、Swift 版本、scheme 名不匹配。

### 2. Sideloadly 免费签名安装

1. 下载 [Sideloadly](https://sideloadly.io/)；先装苹果官方 Windows 驱动（顺序别反）：
   - iTunes（官网下载，不要 Microsoft Store 版）：https://www.apple.com/itunes/download/
   - iCloud：https://support.apple.com/kb/DL2007
2. iPad 数据线连电脑，弹「信任此电脑」点信任。
3. Sideloadly：拖入 ipa → 填 Apple ID → Advanced 勾 `Normal Installation` → Start。
   - 免费 Apple ID 限 3 台设备 + 10 个 App，签名 7 天过期。
   - bundle id 冲突就让它自动改。

### 3. iPad 上信任证书

设置 → 通用 → VPN与设备管理 → 找到你的开发者证书 → 信任。

### 4. 开始广播

1. 打开 PadShell → 点圆形广播按钮 → 系统弹窗选 **PadShell Broadcast** → 开始广播。
2. App 里的监控面板应显示「在线」，截图 KB 数开始增长——全通。
3. 电脑浏览器开 `http://<esp32-ip>/`：画面 1fps 自动刷新（这就是监控页）。

### 5. 调帧率

- 网页：`http://<esp32-ip>/config?token=...&fps=2`（1-5，NVS 持久化）。
- PadShell App 里也有 Stepper。
- fps 越大越流畅、ESP32 负担越大：**1fps 稳，2fps 推荐，5fps 压测用**。

## 停止广播

状态栏顶部红色胶囊 → 点它 → 停止广播。或锁屏后再解锁看情况（广播默认锁屏即断）。

## 排障

| 症状 | 处置 |
|---|---|
| 广播弹窗里没有 PadShell Broadcast | App 没装好/扩展没嵌进去：重装；检查 ipa 内 PlugIns/PadBroadcast.appex |
| 开始广播立即断，提示上传失败 | ESP32 地址/令牌不对（SampleHandler.swift 常量）、或 iPad 不在 ESP32 同一 WiFi |
| 图床「在线」但画面不刷新 | 看 ESP32 `/status` 的 `ago`：若一直涨，扩展被系统休眠；重新开始广播一次 |
| App 监控面板「离线」 | 地址/token 填错，或电脑/iPad 与 ESP32 不在同一网 |
| 7 天后 App 打不开 | 重新 Sideloadly（同一步骤 2），数据不丢 |

## 升级路线（可选）

- **App Group**：主 App 与扩展共享 UserDefaults，把上传地址/令牌做成 App 内可改（要重签名，Sideloadly 支持）。
- **加密上传**：HTTPS/摘要鉴权（ESP32 侧上 TLS 资源紧张，现用内网 + token 已可接受）。
- **目标识别联动**：主 App 内嵌目标识别（本地模板匹配或接入 VLM），识别结果直接驱动 ESP32 点击，形成全自主闭环。
