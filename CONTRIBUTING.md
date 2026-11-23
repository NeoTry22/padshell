# 贡献指南

## 工程纪律

- **先读现状再改**：改动前必须实际读过目标文件的当前内容。
- **失败不重放**：工具/命令报错后，先确认现状（上次调用可能已部分生效），再决定下一步；
  不原样重发同一个改动。
- **改完跑同层校验**：固件改动跑 PlatformIO 编译；Swift 改动至少过 CI 编译；
  文档改动自查链接与表述。
- **如实报告**：没跑过的校验不写成"已验证"；有失败贴原始输出。
- 不为了让检查变绿而删除或弱化测试与校验逻辑。

## 固件（firmware/）

- 平台：PlatformIO + Arduino-ESP32 3.3.x（pioarduino 55.x）+ NimBLE-Arduino 2.2.0。
- 板子：ESP32-S3-DevKitC-1 N16R8（16MB Flash / 8MB OPI PSRAM）。
- 常改参数：分段移动引擎（段长/段间延时）、帧参数下发、鉴权。
- **不要调 socket 超时来"优化"上传**：被 iOS 节流的上传会被更短超时切断形成重试循环（实测有害）。

## iPad 端（ipadclient/）

- Swift 5 + XcodeGen（`project.yml` 定义主 App + 广播上传扩展）。
- CI：GitHub Actions macOS runner 编译未签名 IPA（见 .github/workflows/）。
- 兼容基线：**iPadOS 17.x**（deploymentTarget 17.0）——新 API 若要求 iOS 26，
  请做成可选分支并在 README 标注系统版本要求。

## 提交约定

- 一个主题一个提交；提交信息写清楚动机与实测结果（"实测 1.6fps 稳定"优于"优化性能"）。
- 涉及行为变化，同步更新 docs/ 对应篇目。
- **任何提交前自查**：不要提交真实 WiFi/密码/token/IP；占位符见各源文件顶部。
