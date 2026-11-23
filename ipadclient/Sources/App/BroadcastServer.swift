import Foundation

// 主 App 侧：3s 轮询 ESP32 /status 显示图床健康度；调帧率走 /config?fps=（NVS 持久化）。
// 注意：这里用的是主 App 自己的 UserDefaults——广播扩展没有 App Group 读不到它，
// 所以扩展的上传地址/令牌是编译期常量（见 SampleHandler.swift）。
final class BroadcastServer: ObservableObject {
    @Published var host: String {
        didSet { UserDefaults.standard.set(host, forKey: "host") }
    }
    @Published var token: String {
        didSet { UserDefaults.standard.set(token, forKey: "token") }
    }
    @Published var fps: Int = 1
    @Published private(set) var online = false
    @Published private(set) var shotKB = 0
    @Published private(set) var ago = -1
    @Published private(set) var fpsText = "—"

    private let session = URLSession(configuration: .ephemeral)
    private var timer: Timer?

    init() {
        let d = UserDefaults.standard
        host = d.string(forKey: "host") ?? "http://192.168.1.100"
        token = d.string(forKey: "token") ?? "CHANGE_ME_TO_LONG_RANDOM_TOKEN"
        fps = d.object(forKey: "fps") as? Int ?? 1
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.poll()
        }
    }

    func setFps(_ v: Int) {
        fps = v
        UserDefaults.standard.set(v, forKey: "fps")
        var req = URLRequest(url: URL(string: "\(host)/config?token=\(token)&fps=\(v)")!)
        req.timeoutInterval = 6
        session.dataTask(with: req) { [weak self] _, _, _ in self?.poll() }.resume()
    }

    private func poll() {
        var req = URLRequest(url: URL(string: "\(host)/status?token=\(token)")!)
        req.timeoutInterval = 6
        session.dataTask(with: req) { [weak self] data, resp, _ in
            guard let self else { return }
            DispatchQueue.main.async {
                if let http = resp as? HTTPURLResponse, http.statusCode == 200,
                   let data,
                   let j = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
                    self.online = true
                    self.shotKB = (j["shot"] as? Int ?? 0) / 1024
                    self.ago = j["ago"] as? Int ?? -1
                    if let f = j["fps"] as? Int { self.fpsText = "\(f) fps" } else { self.fpsText = "旧固件(无fps字段)" }
                } else {
                    self.online = false
                    self.ago = -1
                }
            }
        }.resume()
    }
}
