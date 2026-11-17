# PadShell —— iPad 客户端支线 · 空壳验证指南

> 目标：用最小成本验证「GitHub Actions 编译 → Sideloadly 免费签名安装 → ScreenCaptureKit 捕获 iPad 整屏」这条链路。
> 验证通过后，正式客户端（广播捕获 + espvision Swift 版 + BLE 指令）在此骨架上生长。
> 捕获实现改编自 phone-agent（MIT）：`SCContentSharingPicker` + `SCStream`，单 target、无广播扩展。

## 第 0 步：先决条件检查（最重要）

1. **iPadOS 版本 ≥ 26**：iPad 上 `设置 → 通用 → 关于本机 → 系统版本`。
   - ≥ 26.0：本路线可行（`SCContentSharingPicker` 是 iOS 26 新 API）。
   - < 26：**本路线不可行**，需退回 ReplayKit 广播扩展方案（多一个 target + App Group IPC），先告知我再改骨架。
2. **一个 Apple ID**（免费账号即可，用于签名）。
3. **Windows 上能装 Sideloadly**（需要 iTunes + iCloud 官方 Windows 版驱动，见下）。

## 第 1 步：GitHub Actions 编译出未签名 ipa

1. 把 `ipadclient/` 整个目录推到一个 GitHub 仓库（含 `.github/workflows/`）。
2. 仓库页 → Actions 标签 → 选 "Build unsigned IPA" → Run workflow。
3. 跑完后在该次运行页面底部下载 artifact `PadShell-unsigned-ipa`，得到 `PadShell-unsigned.ipa`。

> 编译报错直接把 Actions 日志发我修。常见坑：`macos-26` runner 不可用（改 `macos-latest`）、XcodeGen 版本、scheme 名不匹配。

## 第 2 步：Windows 上 Sideloadly 签名安装

1. 下载安装 [Sideloadly](https://sideloadly.io/)。
2. 先装 Apple 官方 Windows 驱动（顺序别反）：
   - iTunes（**从苹果官网下，不要从 Microsoft Store 装**）：https://www.apple.com/itunes/download/
   - iCloud：https://support.apple.com/kb/DL2007
3. iPad 用数据线连电脑（信任对话框点信任）。
4. Sideloadly：拖入 `PadShell-unsigned.ipa` → 填 Apple ID → Advanced 里勾选 `Normal Installation` → Start。
   - 同一免费 Apple ID 签名上限 3 台设备 + 10 个 App，签名 **7 天过期**需重签。
   - 若报 bundle id 冲突，让 Sideloadly 自动改 bundle id 即可。

## 第 3 步：iPad 上信任证书并验证

1. `设置 → 通用 → VPN与设备管理` → 找到你的 Apple ID 开发者证书 → 点"信任"。
2. 打开 **PadShell** → 点"开始捕获" → 系统弹出屏幕共享选择器 → **选择"整个屏幕"**。
3. 验证点（全过 = 链路打通）：
   - [ ] 系统选择器正常弹出、可选"整个屏幕"
   - [ ] 状态显示"整屏捕获中"、FPS > 0、帧数持续增长
   - [ ] 切到任意其他 App，预览图跟着变化（**跨 App 捕获成功**——这是核心验证项）
   - [ ] 停止按钮能干净停止

## 验证通过后的下一步（正式客户端）

| 模块 | 内容 |
|---|---|
| 捕获增强 | 移植完整帧新鲜度门禁（capturedAt/confirmedAt、idle 确认、流 ID 校验） |
| 识别 | espvision 模板匹配 Swift 移植（离线）或云 VLM 调用（先跑通） |
| 执行 | CoreBluetooth 连接 ESP32-PadCtl，走自定义特征或直接触发系统 HID 前先发坐标 |
| 闭环 | 识别 → 坐标 → ESP32 点击 → 帧变化确认 → 下一轮 |
