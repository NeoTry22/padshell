import serial, time, sys, re

port = "COM3"
baud = 115200
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
s = serial.Serial(port, baud, timeout=0.2)
# 触发硬复位：拉低 RTS 100ms 再释放（ESP32 EN 引脚），抓完整启动日志
s.setDTR(False); s.setRTS(True); time.sleep(0.1)
s.setRTS(False); time.sleep(0.1)
s.reset_input_buffer()
t0 = time.time()
buf = []
print(f"=== 读取 {port} {dur}s ===")
while time.time() - t0 < dur:
    try:
        line = s.readline().decode("utf-8", "replace").rstrip()
        if line:
            buf.append(line)
            print(line)
    except Exception:
        pass
s.close()

text = "\n".join(buf)
print("\n=== 关键信息提取 ===")
for pat, label in [
    (r"192\.168\.\d+\.\d+", "IPv4"),
    (r"\[[0-9a-fA-F:]+\]", "IPv6"),
    (r"认证/绑定完成[^\n]*", "BLE绑定"),
    (r"bonded=\d+[^\n]*", "bonded"),
    (r"[Bb]lue[^\n]*(connected|连接|ready|ready)[^\n]*", "BLE状态"),
    (r"STA[^\n]*", "WiFi"),
    (r"[Ee]rror[^\n]*", "错误"),
]:
    m = re.findall(pat, text)
    if m:
        print(f"{label}: {m[-5:]}")
