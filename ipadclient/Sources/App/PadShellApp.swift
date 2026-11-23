import SwiftUI
import ReplayKit

@main
struct PadShellApp: App {
    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

// RPSystemBroadcastPickerView 封装：点它弹系统广播选择器（iOS 12+，iPadOS 17 可用）
struct BroadcastPicker: UIViewRepresentable {
    func makeUIView(context: Context) -> RPSystemBroadcastPickerView {
        let v = RPSystemBroadcastPickerView(frame: .zero)
        v.showsMicrophoneButton = false
        v.preferredExtension = "com.padshell.padshell.broadcast"   // 预选我们的扩展
        return v
    }
    func updateUIView(_ uiView: RPSystemBroadcastPickerView, context: Context) {}
}

struct ContentView: View {
    @StateObject private var srv = BroadcastServer()

    var body: some View {
        VStack(spacing: 14) {
            Text("PadShell 屏幕广播")
                .font(.title2.bold())

            Text("点下方圆形按钮 → 系统弹窗选「PadShell Broadcast」→ 开始广播。\n开始后可退到后台，画面持续上传 ESP32。\n停止：状态栏红色胶囊 → 点它 → 停止广播。")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal)

            BroadcastPicker()
                .frame(width: 60, height: 60)
                .padding(10)
                .background(Color.white.cornerRadius(16))
                .overlay(RoundedRectangle(cornerRadius: 16).stroke(Color.secondary.opacity(0.35)))
            Text("↑ 广播按钮（iPadOS 17 上系统图标可能渲染成空白，点白框中心即可）")
                .font(.caption2)
                .foregroundStyle(.secondary)

            Divider().padding(.horizontal)

            Form {
                Section("ESP32 监控") {
                    TextField("地址", text: $srv.host)
                        .keyboardType(.URL)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                    TextField("token", text: $srv.token)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                    Stepper("期望帧率：\(srv.fps) fps", value: Binding(
                        get: { srv.fps }, set: { srv.setFps($0) }), in: 1...5)
                }
                Section("状态") {
                    LabeledContent("图床", value: srv.online ? "在线" : "离线")
                    LabeledContent("最新截图", value: srv.shotKB > 0 ? "\(srv.shotKB) KB" : "无")
                    LabeledContent("上传于", value: srv.ago >= 0 ? "\(srv.ago) 秒前" : "—")
                    LabeledContent("当前帧率", value: srv.fpsText)
                }
                Section {
                    Text("注：广播扩展上传用的 ESP32 地址/令牌在扩展里编译期固定（Sources/Broadcast/SampleHandler.swift 顶部常量）；本页设置仅用于此监控面板与调帧率。改帧率也可直接访问 ESP32 网页 /config?fps=2。")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
        }
    }
}
