import SwiftUI

@main
struct PadShellApp: App {
    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

struct ContentView: View {
    @StateObject private var engine = CaptureEngine()

    var body: some View {
        VStack(spacing: 16) {
            Text("PadShell 捕获验证")
                .font(.title2.bold())

            Button(engine.running ? "停止捕获" : "开始捕获") {
                engine.running ? engine.stop() : engine.start()
            }
            .buttonStyle(.borderedProminent)
            .padding(.horizontal, 32)

            Text(engine.state)
                .font(.footnote)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)

            Grid(horizontalSpacing: 24, verticalSpacing: 8) {
                GridRow {
                    Text("帧数").foregroundStyle(.secondary)
                    Text("\(engine.frameCount)").monospacedDigit()
                }
                GridRow {
                    Text("FPS").foregroundStyle(.secondary)
                    Text(String(format: "%.1f", engine.fps)).monospacedDigit()
                }
                GridRow {
                    Text("帧尺寸").foregroundStyle(.secondary)
                    Text(engine.frameSize).monospacedDigit()
                }
            }
            .font(.body)

            if let image = engine.lastImage {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
                    .frame(maxHeight: 320)
                    .border(Color.secondary.opacity(0.3))
                    .accessibilityLabel("最新捕获帧预览")
            } else {
                Rectangle()
                    .fill(Color.secondary.opacity(0.15))
                    .frame(maxHeight: 320)
                    .overlay(Text("暂无画面").foregroundStyle(.secondary))
            }

            Spacer()
        }
        .padding(24)
    }
}
