import ReplayKit
import UIKit

// ============================================================================
// PadShell 广播上传扩展（RPBroadcastSampleHandler）
// iPadOS 屏幕广播 → CI 降采样 JPEG → multipart POST ESP32 /upload
// 帧率(fps)/帧长边(qse)/JPEG质量(qql)均由 ESP32 /status 实时下发（每 20s 刷新）：
// 网页 /config?fps=&edge=&q= 即调，无需重装 App。2026-11-22：游戏等亮色画面帧体积
// 是暗页面的 3.5×（73KB vs 21KB），单线程服务器在 fps=4×73KB 下超载 2.8×，
// 默认收窄为 720px/q0.40（~37KB）换回及时性。扩展与主 App 无 App Group，上传地址/令牌只能编译期固定。
// ============================================================================
final class SampleHandler: RPBroadcastSampleHandler {

    // ===== 编译期配置：换 ESP32 地址/令牌改这里，重新跑一遍 CI 编译即可 =====
    private let host  = "http://192.168.1.100"            // ← 改成你的 ESP32 地址，重编译生效
    private let token = "CHANGE_ME_TO_LONG_RANDOM_TOKEN"  // ← 与固件 AUTH_TOKEN 一致
    // =====================================================================

    private var fps = 1                       // 期望帧/秒（ESP32 下发，1-5）
    private var edge: CGFloat = 720           // 帧长边 px（ESP32 下发 qse，480-1280）
    private var quality: CGFloat = 0.40       // JPEG 质量（ESP32 下发 qql%，0.20-0.85）
    private var lastSent = -10.0              // 上一帧时间戳（秒）
    private var inFlight = false              // 上传在途标志：在途就丢帧
    private var fails = 0                     // 连续失败计数
    private let ctx = CIContext(options: [.cacheIntermediates: false])
    private let session = URLSession(configuration: .ephemeral)
    private var cfgTask: Task<Void, Never>?

    override func broadcastStarted(withSetupInfo setupInfo: [String: NSObject]?) {
        lastSent = -10; inFlight = false; fails = 0
        cfgTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.refreshFps()
                try? await Task.sleep(nanoseconds: 20_000_000_000)   // 20s
            }
        }
    }

    override func broadcastFinished() {
        cfgTask?.cancel()
    }

    // 视频帧按帧时间节流到 fps；音频帧忽略。
    // 图床要的是「最新画面」，不是全量——在途丢帧天然抗背压。
    // 注意：Xcode 26 SDK 里本方法的 Swift 名是 processSampleBuffer(_:with:)（老教程的 process(_:with:) 已不匹配）。
    // 官方文档保证：本回调返回前 ReplayKit 不会再次调用——同步编码+异步上传正好自节奏。
    // 节流用墙钟而非 CMSampleBufferGetPresentationTime：后者在 Xcode 26 显式模块模式下不导入 Swift，绕开。
    override func processSampleBuffer(_ sampleBuffer: CMSampleBuffer, with sampleBufferType: RPSampleBufferType) {
        guard sampleBufferType == .video,
              CMSampleBufferIsValid(sampleBuffer),
              !inFlight else { return }
        let t = Date().timeIntervalSinceReferenceDate
        guard t - lastSent >= 0.95 / Double(fps) else { return }
        lastSent = t
        encodeAndUpload(sampleBuffer)
    }

    private func refreshFps() async {
        guard let url = URL(string: "\(host)/status?token=\(token)") else { return }
        do {
            let (data, _) = try await session.data(from: url)
            if let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                if let f = j["fps"] as? Int { fps = max(1, min(5, f)) }
                if let e = j["qse"] as? Int { edge = CGFloat(max(480, min(1280, e))) }
                if let q = j["qql"] as? Int { quality = CGFloat(max(20, min(85, q))) / 100.0 }
            }
        } catch { /* 网络抖动：沿用上次值 */ }
    }

    private func encodeAndUpload(_ sb: CMSampleBuffer) {
        guard let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        inFlight = true
        let ci = CIImage(cvPixelBuffer: pb)
        // 实测（2026-11-22）：iPad 横屏时广播帧是竖屏（内容被系统转了 90°CW，dock 在左、状态栏在右）。
        // 逆时针转回 90°，上传图与真实屏幕方向一致，控制台点击映射才能对上。
        let rot = CGAffineTransform(a: 0, b: 1, c: -1, d: 0, tx: ci.extent.height, ty: 0)
        let landscape = ci.transformed(by: rot)
        let ext = landscape.extent
        let scale = min(1.0, edge / max(ext.width, ext.height))    // 长边压到 ESP32 下发的 qse（默认 720px）
        let scaled = scale < 1.0 ? landscape.transformed(by: CGAffineTransform(scaleX: scale, y: scale)) : landscape
        guard let cg = ctx.createCGImage(scaled, from: scaled.extent),
              let jpg = UIImage(cgImage: cg).jpegData(compressionQuality: quality) else {
            inFlight = false
            return
        }
        upload(jpg)
    }

    private func upload(_ jpg: Data) {
        let b = "padshell.boundary"
        var body = Data()
        body.append(Data("--\(b)\r\nContent-Disposition: form-data; name=\"file\"; filename=\"f.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n".utf8))
        body.append(jpg)
        body.append(Data("\r\n--\(b)--\r\n".utf8))
        var req = URLRequest(url: URL(string: "\(host)/upload?token=\(token)")!)
        req.httpMethod = "POST"
        req.setValue("multipart/form-data; boundary=\(b)", forHTTPHeaderField: "Content-Type")
        req.timeoutInterval = 25   // ESP32 同步服务器高峰会排队，10s 会误判失败
        session.uploadTask(with: req, from: body) { [weak self] _, resp, _ in
            guard let self else { return }
            defer { self.inFlight = false }
            if let http = resp as? HTTPURLResponse, http.statusCode == 200 {
                self.fails = 0
            } else {
                self.noteFail()
            }
        }.resume()
    }

    private func noteFail() {
        fails += 1
        // 不轻易自杀：偶发失败不该杀掉广播；只有持续失败才报错停止。
        if fails > 300 {
            finishBroadcastWithError(NSError(
                domain: "PadShell", code: -1,
                userInfo: [NSLocalizedFailureReasonErrorKey:
                    "上传持续失败 300+ 次：检查 ESP32 地址/令牌、是否同一 WiFi、或 IP 被拉黑（等 10 分钟再试）"]))
        }
    }
}
