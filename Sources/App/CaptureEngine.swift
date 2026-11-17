// CaptureEngine — 空壳验证版屏幕捕获
// 改编自 phone-agent (MIT, https://github.com/boboabw/phone-agent) 的 CaptureService，
// 仅保留：系统共享选择器 → SCStream → 帧渲染 → 计数/预览。
// 其帧新鲜度门禁（capturedAt/confirmedAt）留待正式客户端接入 espvision 时再移植。

import Foundation
import UIKit
import Combine
import CoreImage
import CoreMedia
import ScreenCaptureKit

@MainActor
final class CaptureEngine: NSObject, ObservableObject,
                           SCContentSharingPickerObserver, SCStreamOutput, SCStreamDelegate {
    @Published private(set) var state = "未开始"
    @Published private(set) var running = false
    @Published private(set) var fps: Double = 0
    @Published private(set) var frameCount = 0
    @Published private(set) var lastImage: UIImage?
    var frameSize: String {
        guard let cg = lastImage?.cgImage else { return "-" }
        return "\(cg.width)×\(cg.height)"
    }

    private var stream: SCStream?
    private let context = CIContext(options: [.cacheIntermediates: false])
    private let framesQueue = DispatchQueue(label: "padshell.frames")
    private var times: [Date] = []
    private var lastPublishAt = Date.distantPast
    private var fullDisplay = false
    private var stopped = false
    private let maxSide: CGFloat = 1280

    // MARK: 生命周期

    func start() {
        guard !running else { return }
        stopped = false
        state = "请在系统选择器中共享整个屏幕"
        let picker = SCContentSharingPicker.shared
        guard picker.isAvailable else {
            running = false; state = "系统不允许屏幕捕获（检查屏幕录制限制 / 系统版本）"
            return
        }
        picker.add(self); picker.isActive = true; picker.present(using: .display)
    }

    func stop() {
        stopped = true
        let previous = stream; stream = nil
        let picker = SCContentSharingPicker.shared
        picker.remove(self); picker.isActive = false
        previous?.stopCapture { _ in }
        running = false; fps = 0; lastImage = nil
        state = "已停止"
    }

    private func endStream(_ s: SCStream, status: String, error: String?) {
        guard s === stream, !stopped else { return }
        stream = nil
        running = false; fps = 0; lastImage = nil
        state = error == nil ? status : "\(status): \(error!)"
    }

    // MARK: SCContentSharingPickerObserver

    func contentSharingPicker(_ picker: SCContentSharingPicker,
                              didUpdateWith filter: SCContentFilter, for s: SCStream?) {
        guard !stopped else { return }
        fullDisplay = filter.style == .display
        let previous = stream; stream = nil
        previous?.stopCapture { _ in }

        let config = SCStreamConfiguration()
        config.capturesAudio = false
        let scale = CGFloat(filter.pointPixelScale)
        config.width = max(1, Int(filter.contentRect.width * scale))
        config.height = max(1, Int(filter.contentRect.height * scale))

        let next = SCStream(filter: filter, configuration: config, delegate: self)
        do {
            try next.addStreamOutput(self, type: .screen, sampleHandlerQueue: framesQueue)
            stream = next
            state = "等待第一帧…"
            next.startCapture { [weak self] error in
                Task { @MainActor in
                    if let error { self?.endStream(next, status: "捕获启动失败", error: error.localizedDescription) }
                    else { self?.running = true; self?.state = "捕获中" }
                }
            }
        } catch {
            state = "捕获启动失败: \(error.localizedDescription)"
        }
    }

    func contentSharingPicker(_ picker: SCContentSharingPicker, didCancelFor s: SCStream?) {
        if stream == nil, !stopped { state = "已取消屏幕共享" }
    }

    func contentSharingPickerStartDidFailWithError(_ error: Error) {
        state = "共享选择器失败: \(error.localizedDescription)"
    }

    // MARK: SCStreamDelegate

    func stream(_ s: SCStream, didStopWithError error: Error) {
        Task { @MainActor in self.endStream(s, status: "系统已停止屏幕共享", error: error.localizedDescription) }
    }

    // MARK: SCStreamOutput（framesQueue 线程）

    func stream(_ s: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard s === stream, !stopped, type == .screen else { return }
        guard CMSampleBufferIsValid(sampleBuffer),
              let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false)
                  as? [[SCStreamFrameInfo: Any]],
              let raw = attachments.first?[.status] as? Int,
              let status = SCFrameStatus(rawValue: raw),
              status == .complete,
              let buffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }

        let now = Date()
        times.append(now)
        times.removeAll { now.timeIntervalSince($0) > 2 }

        // 节流到 4 FPS，省 CPU；空壳阶段足够
        guard now.timeIntervalSince(lastPublishAt) >= 0.25 else { return }
        lastPublishAt = now

        autoreleasepool {
            let source = CIImage(cvPixelBuffer: buffer)
            let scale = min(1, maxSide / max(source.extent.width, source.extent.height))
            let scaled = source.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
            guard let cg = context.createCGImage(scaled, from: scaled.extent) else { return }
            let image = UIImage(cgImage: cg)
            let measured = confirmedAt2s(times)
            Task { @MainActor in
                guard !stopped else { return }
                self.lastImage = image
                self.frameCount += 1
                self.fps = measured
                if self.running {
                    self.state = self.fullDisplay ? "整屏捕获中" : "仅共享了部分窗口（请选整个屏幕）"
                }
            }
        }
    }

    private func confirmedAt2s(_ times: [Date]) -> Double {
        guard let first = times.first, let last = times.last,
              last.timeIntervalSince(first) > 0, times.count > 1 else { return 0 }
        return Double(times.count - 1) / last.timeIntervalSince(first)
    }
}
