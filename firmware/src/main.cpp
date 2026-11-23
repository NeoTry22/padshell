/**
 * ============================================================================
 * ESP32-S3-DevKitC-1 (N16R8) + iPad 无人值守远控固件 —— 纯鼠标架构
 * ============================================================================
 * 架构（2026-11-22 用户决策：彻底剥离键盘，单一鼠标角色，避免键盘/鼠标互扰）：
 *   网页点击截图坐标 --> ESP32 --BLE HID 鼠标--> iPad 光标移动+点击
 *   截图触发 = 鼠标按钮4 --> iPadOS 辅助触控「自定义按钮」绑定
 *              （快捷指令「截屏上传」或内置「截屏」）--> JPEG POST /upload
 *
 * 为什么剥离键盘（取代旧「复合描述符二选一」结论）：
 *   1) 复合设备下 iPadOS 会把修饰键组合路由走，FKA 触发失效；纯键盘又没鼠标。
 *      与其在描述符层纠缠，不如把"触发快捷指令"也挪到鼠标按钮层——iPadOS
 *      原生支持把鼠标额外按钮绑定到动作（内置「截屏」确定存在，
 *      「快捷指令」iOS16+ 待实机确认，绑定入口需辅助触控开启）。
 *   2) 旧复合版（键盘+鼠标）固件保留在 git 历史首个提交中，需要键盘路线时随时回退。
 *   3) 服务声明逐项对齐 phone-agent（iOS 实测可用参考）：HID 特征值挂强制加密
 *      （READ_ENC/WRITE_ENC，逼 iOS 走规范配对流程）+ Battery Service + GAP 外观
 *      特征值 + 制造商/型号。Apple 指针栈疑似严格执行 HOGP 加密要求（待实证）。
 *
 * 硬性要求：
 *   - 截图缓冲 4MB 上限，ps_malloc 在外部 PSRAM，绝不占内部 SRAM
 *   - 仅用 NimBLE（S3 无经典蓝牙，严禁 Bluedroid），开启 Bonding 静默回连
 *   - 全路由 URL token 校验，失败 5 次/IP 拉黑 10 分钟
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <WebServer.h>
#include <Preferences.h>
#include "NimBLEDevice.h"
// GAP 服务 C API（ble_svc_gap_device_appearance_set）：NimBLE-Arduino 2.x 未封装，直接调宿主 API
#include "nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h"

// ============================================================================
// 用户配置区（烧录前必须修改 WIFI 与 TOKEN！）
// ============================================================================
#define WIFI_SSID            "YOUR_WIFI_SSID"                  // ← 改成你的（或烧录后用 /config?ssid=&pass= 运行时改）
#define WIFI_PASS            "YOUR_WIFI_PASSWORD"              // ← 改成你的
#define AUTH_TOKEN           "CHANGE_ME_TO_LONG_RANDOM_TOKEN"  // ← 改成强随机串，所有路由都要带它

#define DEVICE_NAME          "ESP32-PadMouse"      // 换名强制 iPad 当新设备发现，避开旧复合设备缓存

// 屏幕逻辑分辨率标定（单位：BLE 计数 counts，即从屏幕左上角走到右下角需要的计数）
// 指针加速导致非线形，这是粗标定；精度靠网页微调 /config 与视觉闭环
#define SCREEN_MAX_X_DEFAULT 1300
#define SCREEN_MAX_Y_DEFAULT 950

// 归零：连续推负向位移把光标压到屏幕左上角 (0,0)，推过头无害（被屏幕边界截断）
#define ZERO_PUSH_STEP       (-127)
#define ZERO_PUSH_REPEAT     20        // 20 * 127 = 2540 counts，够穿越 4K 屏

// 心跳保活：每 15s 发一次零位移报告，防止 iPad 判定外设休眠断连
#define KEEPALIVE_MS         15000

// 点击后自动按一下「按钮4」（联动 iPad 辅助触控绑定：按钮4=快捷指令「截屏上传」；
// 可在 /config?auto=0 关掉）
#define AUTO_SHOT_DEFAULT    1
#define AUTO_SHOT_DELAY_MS   1200      // 等点击动画/悬停生效后再触发
#define AUTO_BTN4_BIT        0x08      // HID 按钮4 位掩码（1/2/4=左/右/中，8=按钮4，16=按钮5）

// 安全
#define JPEG_CAP             (4 * 1024 * 1024)  // iPad Retina 截图可达 1-3MB，PSRAM 8MB 放宽到 4MB
#define BAN_FAILS            5              // 同 IP 连续 token 失败次数 → 拉黑
#define BAN_MS               (10 * 60 * 1000UL)

// ============================================================================
// 全局状态
// ============================================================================
static WebServer      web(80);
static Preferences    nvs;

// PSRAM 截图缓冲（仅此一块大内存，位于外部 PSRAM）
static uint8_t*       gJpeg      = nullptr;
static size_t         gJpegLen   = 0;
static SemaphoreHandle_t gJpegMx = nullptr;
static uint32_t       gLastUpload = 0;      // millis()

// BLE
static NimBLECharacteristic* gMouseRep = nullptr;   // Report ID 1（input，鼠标主通道）
static NimBLECharacteristic* gBootMouseRep = nullptr; // Boot Mouse (0x2A33) 兜底通道
static volatile bool  bleConnected  = false;
static volatile bool  bleSubscribed = false;      // ⚠️ 实际仅跟踪 Boot Mouse(0x2A33) 订阅位（历史命名，勿改语义）。
                                                  // Report 模式下 iPad 不订阅 Boot 通道，此位为 0 是正常现象。
                                                  // 判断"鼠标可用"必须用 (bleSubscribed || mouseRepSub)。
// 鼠标单通道发送所需的通道状态（esp32kbd/phone-agent 验证方案）
static volatile uint8_t gProtoMode = 1;           // 主机协议模式：1=Report 0=Boot（0x2A4E）
static volatile bool  mouseRepSub  = false;       // 鼠标 Report 通道(0x2A4D ID2)被订阅
// Report ID 前缀诊断开关：无前缀方案已被 esp32kbd(L0-L3全过) 与 phone-agent(iOS可用) 验证；
// gMousePre 保留仅为应急诊断（/mouse?pre=1 切换），默认无前缀。
static volatile bool  gMousePre  = false;         // 鼠标通知加 0x01 前缀（诊断用，默认关）

// 指针模式：0=rel 相对鼠标（iOS 15 用），1=abs 数字化仪绝对坐标（iOS 17+ 用）
// 两种 Report Map 集合共存，切换用 /config?mode=abs|rel，运行时生效无需重配对
static uint8_t gMode = 0;

// 可调参数（NVS 持久化，网页 /config 可改）
static uint16_t gCountsX    = SCREEN_MAX_X_DEFAULT;
static uint16_t gCountsY    = SCREEN_MAX_Y_DEFAULT;
static uint8_t  gZeroPush   = ZERO_PUSH_REPEAT;
static bool     gAutoShot   = AUTO_SHOT_DEFAULT;
static bool     gInvert     = false;    // 滚轮方向翻转（iPad 上方向反了置 1）
static uint8_t  gFps        = 1;        // 期望截图速率：iPad 广播扩展按此上传帧/秒（1-5，/config?fps= 调）
static uint16_t gJpegEdge   = 720;      // 广播帧长边 px：扩展按 /status qse 降采样（/config?edge= 调，免重装）
static uint8_t  gJpegQ      = 40;       // 广播帧 JPEG 质量 %：扩展按 /status qql 编码（/config?q= 调）
static String   gIPv6Global = "";

// 相对位移光标跟踪：避免每次点击都归零到边缘触发 iPadOS 手势。
// gPtrX/Y 是当前光标逻辑坐标(counts)，持久化到 NVS；gPtrKnown=false(首次配对)
// 时假设光标在屏幕中心建立基准，绝不归零到顶边。
static int  gPtrX = 0, gPtrY = 0;
static bool gPtrKnown = false;
// 把当前光标逻辑位置持久化: 重启后 iPad 指针仍在原处, 恢复即可继续相对位移,
// 无需归零(归零到顶边再下移会触发 iPadOS 下拉手势)。
static void savePtrPos() {
    Preferences p;
    if (p.begin("padctl", false)) {
        p.putInt("ptrx", gPtrX);
        p.putInt("ptry", gPtrY);
        p.putBool("hasptr", true);
        p.end();
    }
}

// 上传流状态（multipart）
static bool   gUpUsed  = false;
static bool   gUpBig   = false;
static size_t gUpLen   = 0;
static bool   gUpMagicOk = false;   // chunk 回调 END 时魔数校验结果
static uint32_t gUpT0 = 0;           // 诊断：本次上传开始时刻

// IP 拉黑表（RAM，重启即清）
struct BanEntry { IPAddress ip; uint32_t fails; uint32_t banUntil; };
static BanEntry gBans[8];
static uint8_t  gBanCount = 0;

// 上传/鉴权遥测（诊断广播扩展“上传持续失败”用）：/status 汇报
static uint32_t gUpOk = 0, gUpFail = 0, gAuthFailN = 0, gBanHits = 0;
static uint32_t gFrameId = 0;              // 每收到一帧有效截图 +1，控制台按帧号增量拉取
static char     gLastBanInfo[80] = "";     // 最近一次拉黑/拒绁记录

// ============================================================================
// BLE HID：复合鼠标 + 键盘（NimBLE 2.x）
// ============================================================================

// Report Map：纯鼠标（Report ID 1，phone-agent 同款单 ID 布局）。
// 按钮声明 1..5：1/2/3=左/右/中（点击闭环用），4/5=侧键（绑定动作用；
// Logitech MX 同款，iPadOS 原生支持其侧键自定义，5 键在正常鼠标范围内）。
// 旧复合键盘版固件在 git 历史首个提交里。
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Buttons)
    0x19, 0x01, 0x29, 0x05,               //     按钮 1..5（1/2/3=左/右/中，4/5=绑动作）
    0x15, 0x00, 0x25, 0x01,
    0x95, 0x05, 0x75, 0x01,
    0x81, 0x02,       //     Input(Data,Var,Abs)
    0x95, 0x01, 0x75, 0x03,
    0x81, 0x03,       //     Input(Const) 填充 3 位（phone-agent 同款写法）
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30, 0x09, 0x31,               //     X, Y（相对轴）
    0x15, 0x81, 0x25, 0x7F,               //     logical -127..127
    0x75, 0x08, 0x95, 0x02,
    0x81, 0x06,       //     Input(Data,Var,Rel)
    0x09, 0x38,       //     Wheel
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x01,
    0x81, 0x06,       //     Input(Data,Var,Rel)
    0xC0, 0xC0        // End Collection (Physical / Mouse)
};

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        bleConnected = true;
        Serial.printf("[BLE] connected (bonded=%d), 主动发起配对/加密\n", (int)info.isBonded());
        // 关键：HID 主机（iPadOS）要求加密链路才激活键盘/指针。
        // 必须由设备端主动发起 pairing，否则 iPad 以明文 GATT 连接挂起，不投递输入。
        NimBLEDevice::startSecurity(info.getConnHandle());
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        bleConnected = false;
        bleSubscribed = false;
        mouseRepSub = false;
        Serial.printf("[BLE] disconnected (reason=%d), 重新广播等待回连\n", reason);
        NimBLEDevice::getAdvertising()->start();
    }
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        Serial.printf("[BLE] 认证/绑定完成 encrypted=%d bonded=%d\n",
                      (int)info.isEncrypted(), (int)info.isBonded());
    }
};

// ⚠️ 此回调只挂在 gBootMouseRep 上，所以 bleSubscribed = Boot Mouse 通道订阅位。
class CharCb : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
        bleSubscribed = (subValue & 0x01) != 0;   // CCCD notify 位
        Serial.printf("[BLE] host subscribed=%d\n", subValue);
    }
};

// 鼠标 Report 通道（0x2A4D, Report ID 2）订阅跟踪
class MouseSubCb : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
        mouseRepSub = (subValue & 0x01) != 0;
        Serial.printf("[BLE] 鼠标 Report 通道 subscribed=%d\n", (int)mouseRepSub);
    }
};
// 协议模式跟踪：主机写 0x2A4E 切换 Report/Boot，发键通道随之切换
class ProtoCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (!v.empty()) {
            gProtoMode = (uint8_t)v[0];
            Serial.printf("[BLE] 主机设置 Protocol Mode = %d (%s)\n",
                          gProtoMode, gProtoMode ? "Report" : "Boot");
        }
    }
};

// 给特征值挂 Report Reference 描述符 (0x2908)，与 phone-agent 同款声明强制加密读取
static void addReportRef(NimBLECharacteristic* c, uint8_t id, uint8_t type) {
    NimBLEDescriptor* d = c->createDescriptor(NimBLEUUID((uint16_t)0x2908),
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC, 2);
    uint8_t v[2] = { id, type };   // report id, input/output type
    d->setValue(v, 2);
}

static void setupBLE() {
    NimBLEDevice::init(DEVICE_NAME);
    // GAP Appearance 特征值(0x2A01)：广播里的外观码只进广播包，配对后 iPad 还会读
    // GAP 服务里的 Appearance。两处都设 0x03C0（phone-agent/esp32kbd 两个可用参考的验证值）。
    ble_svc_gap_device_appearance_set(0x03C0);
    // Bonding 开启 → 配对一次后存 NVS，断电重启 iPad 会自动回连；无键盘/屏幕则不做 MITM
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    // 密钥双向分发 ENC+ID（对齐 phone-agent 在 iOS 上的验证配置）
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    NimBLEServer* svr = NimBLEDevice::createServer();
    svr->setCallbacks(new ServerCb());

    NimBLEService* hid = svr->createService(NimBLEUUID((uint16_t)0x1812));

    NimBLECharacteristic* hidInfo = hid->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A4A), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
    uint8_t info[4] = { 0x11, 0x01, 0x00, 0x02 };   // bcdHID 1.1, 国家码 0, normally-connectable
    hidInfo->setValue(info, 4);

    // Report Map 强制加密读取：未加密读会返回 Insufficient Authentication，
    // 逼 iOS 先完成配对/加密再读 map（phone-agent 在 iOS 上的实际工作流程）。
    NimBLECharacteristic* rmap = hid->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A4B), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
    rmap->setValue(REPORT_MAP, sizeof(REPORT_MAP));

    hid->createCharacteristic(NimBLEUUID((uint16_t)0x2A4C),
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);  // Control Point（phone-agent 同款）

    NimBLECharacteristic* pm = hid->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A4E),
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    uint8_t mode = 0x01;   // Report Mode
    pm->setValue(&mode, 1);
    pm->setCallbacks(new ProtoCb());

    // 鼠标输入报告（Report ID 1 input，相对位移）——指针主通道，强制加密
    gMouseRep = hid->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A4D),
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
    addReportRef(gMouseRep, 1, 1);
    gMouseRep->setCallbacks(new MouseSubCb());

    // Boot Mouse 通道（无 Report Reference 描述符，按 HID 规范）作为兜底。
    gBootMouseRep = hid->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A33),
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
    gBootMouseRep->setCallbacks(new CharCb());

    // Device Information Service：制造商/型号/PnP（对齐 phone-agent，iPad HID 发现时友好）
    NimBLEService* dis = svr->createService(NimBLEUUID((uint16_t)0x180A));
    NimBLECharacteristic* man = dis->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A29), NIMBLE_PROPERTY::READ);
    man->setValue("AutoClaw");
    NimBLECharacteristic* model = dis->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A24), NIMBLE_PROPERTY::READ);
    model->setValue("ESP32-PadMouse-S3");
    NimBLECharacteristic* pnp = dis->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A50), NIMBLE_PROPERTY::READ);
    uint8_t pnpv[7] = { 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x01, 0x00 };
    pnp->setValue(pnpv, 7);
    dis->start();

    // Battery Service（0x180F/0x2A19）：phone-agent 与 esp32kbd 两个可用参考都有。
    NimBLEService* bas = svr->createService(NimBLEUUID((uint16_t)0x180F));
    NimBLECharacteristic* bat = bas->createCharacteristic(
        NimBLEUUID((uint16_t)0x2A19),
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
    uint8_t lvl = 100;
    bat->setValue(&lvl, 1);
    bas->start();

    hid->start();

    // GAP Appearance 广播：0x03C0 为两个 iOS 可用参考的验证值（勿改，血泪教训）。
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(0x03C0);
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));
    adv->start();
    Serial.println("[BLE] 广播已启动，等待 iPad 配对/回连");
}

// 发送鼠标相对位移报告（phone-agent 同款 4 字节 {btn,dx,dy,wheel}，无 Report ID 前缀）。
// 单通道发送：主机订阅了 Report 通道(ID1) → 走 gMouseRep；
// 否则退回 Boot Mouse (0x2A33) 3 字节兜底。绝不同帧双发。
static bool sendMouse(int8_t dx, int8_t dy, int8_t wheel = 0, uint8_t btn = 0) {
    if (!bleConnected) return false;
    if (mouseRepSub) {
        uint8_t b[4] = { btn, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
        if (gMousePre) {
            uint8_t bp[5] = { 0x01, btn, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
            gMouseRep->setValue(bp, 5);
        } else {
            gMouseRep->setValue(b, 4);
        }
        return gMouseRep->notify();
    }
    if (bleSubscribed) {
        uint8_t b[3] = { btn, (uint8_t)dx, (uint8_t)dy };
        gBootMouseRep->setValue(b, 3);
        return gBootMouseRep->notify();
    }
    return false;
}

// abs 数字化仪已停用：Report Map 去掉鼠标/digitizer 集合后无对应报告通道，
// 且 2026-11-18 实测 iPadOS 17 不接受 abs 报告。保留桩函数防止调用方编译错误。
static bool sendDigitizer(uint16_t x, uint16_t y, bool down) {
    (void)x; (void)y; (void)down;
    return false;
}

// abs 模式点击：移动到位 → 按下 → 抬起（绝对坐标鼠标，直发屏幕位置）
static bool absClick(float fx, float fy) {
    if (!bleConnected || !bleSubscribed) return false;
    fx = constrain(fx, 0.0f, 1.0f);
    fy = constrain(fy, 0.0f, 1.0f);
    uint16_t x = (uint16_t)(fx * 32767.0f);
    uint16_t y = (uint16_t)(fy * 32767.0f);
    sendDigitizer(x, y, false);   // 先空移动让指针定位
    delay(50);
    sendDigitizer(x, y, true);    // 左键按下
    delay(80);
    sendDigitizer(x, y, false);   // 抬起
    Serial.printf("[CLICK-abs] (%.3f, %.3f) -> abs(%u, %u)\n", fx, fy, x, y);
    return true;
}

// abs 模式滑动：按下后按插值路径移动，最后抬起（n = 步数）
static bool absSwipe(float fx0, float fy0, float fx1, float fy1, int steps = 24, int holdMs = 120) {
    if (!bleConnected || !bleSubscribed) return false;
    uint16_t x0 = (uint16_t)(constrain(fx0, 0.0f, 1.0f) * 32767.0f);
    uint16_t y0 = (uint16_t)(constrain(fy0, 0.0f, 1.0f) * 32767.0f);
    uint16_t x1 = (uint16_t)(constrain(fx1, 0.0f, 1.0f) * 32767.0f);
    uint16_t y1 = (uint16_t)(constrain(fy1, 0.0f, 1.0f) * 32767.0f);
    sendDigitizer(x0, y0, true);   // 按下
    delay(holdMs);
    for (int i = 1; i <= steps; i++) {
        uint16_t x = x0 + (uint16_t)(((int32_t)x1 - x0) * i / steps);
        uint16_t y = y0 + (uint16_t)(((int32_t)y1 - y0) * i / steps);
        sendDigitizer(x, y, true);
        delay(10);
    }
    delay(60);
    sendDigitizer(x1, y1, false);  // 抬起
    Serial.printf("[SWIPE-abs] (%u,%u)->(%u,%u) %d steps\n", x0, y0, x1, y1, steps);
    return true;
}

// 键盘路径已于 2026-11-22 整体剥离（sendKey/sendShotCombo 删除）。
// 截图触发改走鼠标按钮4：iPadOS 辅助触控「自定义按钮」把按钮4绑定到
// 快捷指令「截屏上传」（或内置「截屏」），固件侧用 mouseHoldBtn(AUTO_BTN4_BIT) 触发。
// 触发入口见 doClick() 自动截图与 handleAction(a=screenshot)，手动可用 /mouse?op=btn&bit=8。

// ============================================================================
// 鼠标动作：归零校准 + 目标移动 + 点击
// ============================================================================

// 防卡键保险：强制发一次"全键松开"报告。若上次点击的松开报告丢失，
// iPadOS 会一直认为左键按着，后续移动全变成拖拽手势（下拉/误触）。
static void releaseMouseButtons() {
    sendMouse(0, 0, 0, 0);
    delay(20);
}

// 归零：把光标压到屏幕【左上角】(0,0)。
// iPadOS 手势区：顶部=下拉通知中心、底部=上滑回主屏幕，两头都不能作为归零起点。
// 左上角归零后向右下移动不触发任何系统手势；之前的"下拉"是真·左键卡住变成拖拽，
// 已由 releaseMouseButtons() 修复，与归零方向无关。推过头会被边界截断，只多不少。
static void mouseZero() {
    releaseMouseButtons();
    for (int i = 0; i < gZeroPush; i++) {
        sendMouse(ZERO_PUSH_STEP, ZERO_PUSH_STEP);   // X→左边, Y→顶边
        delay(3);
    }
    delay(150);   // 等 iPadOS 指针状态稳定
}

// 从当前位置相对移动 counts
// 关键(iPadOS 实测): 连续快速发 8-count 报告(3ms间隔)超过 iPadOS 处理能力,
// 大跨度右移会丢 13~20% 报告(队列溢出)。必须把位移拆成"段", 每段≤28 counts
// (约0.1屏), 段间停顿 45ms 让 iPadOS 队列消化 —— 实测分步后全程误差<0.1%。
// 段内仍用 8 counts/3ms 小步保持线性(避免指针加速曲线)。
static const int CALM_STEP = 8;
static const int CHUNK_CAP = 28;    // 每段最大 counts (≈0.1 屏)
static const int CHUNK_PAUSE = 45;  // 段间停顿 ms

static void moveChunk(int cx_, int cy_) {
    while (cx_ != 0 || cy_ != 0) {
        int8_t sx = cx_ > 0 ? (int8_t)(cx_ > CALM_STEP ? CALM_STEP : cx_)
                            : (cx_ < 0 ? (int8_t)(cx_ < -CALM_STEP ? -CALM_STEP : cx_) : 0);
        int8_t sy = cy_ > 0 ? (int8_t)(cy_ > CALM_STEP ? CALM_STEP : cy_)
                            : (cy_ < 0 ? (int8_t)(cy_ < -CALM_STEP ? -CALM_STEP : cy_) : 0);
        sendMouse(sx, sy);
        delay(3);
        cx_ -= sx;
        cy_ -= sy;
    }
}

static void mouseMoveToRelative(int dx, int dy) {
    releaseMouseButtons();   // 独立移动指令也先清按键，杜绝拖拽干扰
    while (dx != 0 || dy != 0) {
        int stepx = dx > CHUNK_CAP ? CHUNK_CAP : (dx < -CHUNK_CAP ? -CHUNK_CAP : dx);
        int stepy = dy > CHUNK_CAP ? CHUNK_CAP : (dy < -CHUNK_CAP ? -CHUNK_CAP : dy);
        moveChunk(stepx, stepy);
        dx -= stepx;
        dy -= stepy;
        if (dx != 0 || dy != 0) delay(CHUNK_PAUSE);   // 段间停顿, 防丢步
    }
    delay(80);
}

// 从左上角 (0,0) 分步移动到目标坐标（小步线性）。
// 归零点在顶部，tx/ty 是"距左/距顶"的 counts，向右下移动远离边缘，不触发手势。
static void mouseMoveTo(int tx, int ty) {
    int dx = tx, dy = ty;
    while (dx != 0 || dy != 0) {
        int8_t sx = dx > 0 ? (int8_t)(dx > CALM_STEP ? CALM_STEP : dx)
                           : (dx < 0 ? (int8_t)(dx < -CALM_STEP ? -CALM_STEP : dx) : 0);
        int8_t sy = dy > 0 ? (int8_t)(dy > CALM_STEP ? CALM_STEP : dy)
                           : (dy < 0 ? (int8_t)(dy < -CALM_STEP ? -CALM_STEP : dy) : 0);
        sendMouse(sx, sy);
        delay(3);
        dx -= sx;
        dy -= sy;
    }
    delay(80);   // 指针到位需要一点时间
}

static void mouseClick(uint8_t btn = 1) {
    releaseMouseButtons();   // 清除可能残留的按下状态
    sendMouse(0, 0, 0, btn);
    delay(60);
    sendMouse(0, 0, 0, 0);
    delay(30);
    sendMouse(0, 0, 0, 0);   // 双重松开：防单条松开报告丢失导致卡键
    delay(60);
}

// 诊断用：发任意按钮位掩码（1=左 2=右 4=中），可指定按住时长。
// 用途：iPadOS 辅助触控「自定义更多按钮」绑定了动作但不触发时，
// 排查是「按钮编号映射」还是「需要长按」——标准 HID Button usage 位操作。
static void mouseHoldBtn(uint8_t btnBit, int holdMs) {
    if (holdMs < 20) holdMs = 20;
    if (holdMs > 5000) holdMs = 5000;
    releaseMouseButtons();
    sendMouse(0, 0, 0, btnBit);
    delay(holdMs);
    sendMouse(0, 0, 0, 0);
    delay(50);
    sendMouse(0, 0, 0, 0);
    delay(80);
}

// 完整点击动作：按当前模式分流（rel=归零+移动+单击；abs=数字化仪直发绝对坐标）
static bool doClick(float fx, float fy) {
    // 门禁：Boot Mouse 或 Report 鼠标任一可用即可（旧门禁只查 bleSubscribed，
    // Report 模式下恒为 0 → /click 从未发出，2026-11-22 修复）。
    if (!bleConnected || (!bleSubscribed && !mouseRepSub)) return false;
    fx = constrain(fx, 0.0f, 1.0f);
    fy = constrain(fy, 0.0f, 1.0f);
    if (gMode == 1) {
        absClick(fx, fy);
    } else {
        // 相对位移法：靠 gPtrX/Y 跟踪，每次只做目标与当前的差值移动，
        // 永远不把光标顶回边缘 → 杜绝 iPadOS 手势劫持。
        int tx = (int)(fx * gCountsX), ty = (int)(fy * gCountsY);
        if (!gPtrKnown) {
            // 无基准(全新配对): 假设光标在屏幕中心(iPad 蓝牙鼠标默认位置),
            // 从这里缓慢移向目标, 绝不归零到顶边(会触发下拉手势)。
            gPtrX = gCountsX / 2; gPtrY = gCountsY / 2;
            gPtrKnown = true;
        }
        int rx = tx - gPtrX, ry = ty - gPtrY;
        mouseMoveToRelative(rx, ry);
        gPtrX = tx; gPtrY = ty;
        savePtrPos();                  // 持久化: 重启后恢复, 不再需要建基准
        mouseClick();
        Serial.printf("[CLICK-rel] (%.3f,%.3f) -> rel(%d,%d) abs(%d,%d)\n",
                      fx, fy, rx, ry, tx, ty);
    }
    if (gAutoShot) {
        delay(AUTO_SHOT_DELAY_MS);
        mouseHoldBtn(AUTO_BTN4_BIT, 120);   // 触发 iPad 绑定在按钮4上的动作（快捷指令/内置截屏）
    }
    return true;
}

// ============================================================================
// 自动标定：iPad Safari 测试页闭环（相对位移法）
// 关键教训：iPadOS 顶部=下拉通知中心、底部=上滑回主屏幕，都是手势劫持区。
//   反复把光标暴力归零到边缘会累积触发手势 → Safari 被切后台 → 后续全 timeout。
//   方案：全程【只归零一次】，其余点用相对位移串联，且标定点都在 30%~70% 中部安全区。
// X 增益: 点2→点3 相对右移 0.4*cx counts, 观察屏幕X分数差 / 0.4 = kx → cx/=kx
// Y 增益: 点0→点1 相对下移 0.4*cy counts, 视口Y分数差*vh/sh / 0.4 = ky (顶栏差分抵消)
// ============================================================================
static float gCalVw = 0, gCalVh = 0, gCalSw = 0, gCalSh = 0;  // 页面上报的视口/屏幕尺寸(点)
static float gRepAx = 0, gRepAy = 0;                          // 最近命中(视口分数)
static uint32_t gRepMs = 0;                                   // 最近命中时间戳

// 标定点全部在屏幕中部 30%~70%，远离上下边缘手势区
static const float CAL_PTS[][2] = {
    {0.30f, 0.30f},   // 0: Y 测起点
    {0.30f, 0.70f},   // 1: Y 测终点 (0→1 算 ky)
    {0.30f, 0.50f},   // 2: X 测起点
    {0.70f, 0.50f},   // 3: X 测终点 (2→3 算 kx, 应用新系数)
    {0.50f, 0.50f},   // 4: 新系数验证(中心)
    {0.50f, 0.35f},   // 5: 补充验证
};
static const int CAL_N = 6;
static float gCalObs[CAL_N][2];
static bool  gCalGot[CAL_N];
static bool  gAutoCalOn = false;
static int   gCalStep = -1;
static uint8_t calPhase = 0;          // 0=点击前延迟 1=已点击等命中
static uint32_t gCalStepMs = 0;
static bool  gCalApplied = false;
static float gCalKx = 0, gCalKy = 0, gCalChSh = 0;
static int   gCurCx = 0, gCurCy = 0;  // 光标当前逻辑坐标(counts)，靠积分跟踪
static bool  gCalZeroed = false;      // 本次标定是否已归零(全程只归一次)
static String gCalLog;

// 相对位移把光标移到目标屏幕分数（不归零，始终在中部安全区移动）
static void calMoveTo(float fx, float fy) {
    int tx = (int)(fx * gCountsX), ty = (int)(fy * gCountsY);
    mouseMoveToRelative(tx - gCurCx, ty - gCurCy);
    gCurCx = tx; gCurCy = ty;
}

static void calibTick() {
    if (!gAutoCalOn || gCalStep < 0) return;
    if (!bleConnected || !bleSubscribed) {
        gAutoCalOn = false; gCalStep = -1; gCalLog += " | BLE断开中止";
        Serial.println("[CAL] " + gCalLog);
        return;
    }
    if (calPhase == 0) {
        if (millis() - gCalStepMs < 500) return;   // 给 Safari 反应时间
        if (!gCalZeroed) {                         // 全程只归零这一次
            mouseZero();
            gCurCx = 0; gCurCy = 0;
            gCalZeroed = true;
            delay(120);
        }
        calMoveTo(CAL_PTS[gCalStep][0], CAL_PTS[gCalStep][1]);
        mouseClick();
        gCalStepMs = millis();
        calPhase = 1;
        return;
    }
    // calPhase == 1: 等命中
    bool got = gRepMs > gCalStepMs;
    if (!got && millis() - gCalStepMs < 6000) return;
    if (got) {
        gCalObs[gCalStep][0] = gRepAx;
        gCalObs[gCalStep][1] = gRepAy;
        gCalGot[gCalStep] = true;
        gCalLog += " | P" + String(gCalStep) + "=" + String(gRepAx, 3) + "," + String(gRepAy, 3);
    } else {
        gCalGot[gCalStep] = false;
        gCalLog += " | P" + String(gCalStep) + "=timeout";
    }
    // 4 个测量点收齐 → 计算增益并应用
    if (gCalStep == 3 && gCalGot[0] && gCalGot[1] && gCalGot[2] && gCalGot[3] && gCalSw > 1 && gCalSh > 1) {
        float dfx = CAL_PTS[3][0] - CAL_PTS[2][0];
        float kx = ((gCalObs[3][0] - gCalObs[2][0]) * gCalVw / gCalSw) / dfx;
        float dfy = CAL_PTS[1][1] - CAL_PTS[0][1];
        float ky = ((gCalObs[1][1] - gCalObs[0][1]) * gCalVh / gCalSh) / dfy;
        gCalKx = kx; gCalKy = ky;
        gCalLog += " | kx=" + String(kx, 3) + " ky=" + String(ky, 3);
        if (kx > 0.3f && kx < 3.0f) {
            gCountsX = constrain((int)(gCountsX / kx), 80, 16000);
            nvs.putUShort("cx", gCountsX);
        }
        if (ky > 0.3f && ky < 3.0f) {
            gCountsY = constrain((int)(gCountsY / ky), 80, 16000);
            nvs.putUShort("cy", gCountsY);
        }
        // Safari 顶栏占比: ch/sh = ky*f0 - vy0*vh/sh（f0=点0的y=0.30）
        gCalChSh = ky * CAL_PTS[0][1] - gCalObs[0][1] * gCalVh / gCalSh;
        gCalApplied = true;
        // 系数已变，光标逻辑坐标要按新系数换算到点3位置，否则后续相对位移会错
        gCurCx = (int)(CAL_PTS[3][0] * gCountsX);
        gCurCy = (int)(CAL_PTS[3][1] * gCountsY);
        gCalLog += " | 应用 cx=" + String(gCountsX) + " cy=" + String(gCountsY);
    }
    gCalStep++;
    calPhase = 0;
    gCalStepMs = millis();
    if (gCalStep >= CAL_N) {
        gCalStep = -1;
        gAutoCalOn = false;
        if (gCalApplied && gCalGot[4] && gCalSw > 1) {
            float px = gCalObs[4][0] * gCalVw / gCalSw;
            float py = gCalObs[4][1] * gCalVh / gCalSh + gCalChSh;
            gCalLog += " | 验证(0.5,0.5)→" + String(px, 3) + "," + String(py, 3);
        }
        gCalLog += " | 结束";
        Serial.println("[CAL] " + gCalLog);
        // 标定最后光标停在点5，同步给点击跟踪器，避免下次点击基准错位
        gPtrX = (int)(CAL_PTS[CAL_N-1][0] * gCountsX);
        gPtrY = (int)(CAL_PTS[CAL_N-1][1] * gCountsY);
        gPtrKnown = true;
    }
}

// ============================================================================
// 安全：token 鉴权 + IP 拉黑
// ============================================================================

static int findBan(const IPAddress& ip) {
    for (int i = 0; i < gBanCount; i++)
        if (gBans[i].ip == ip) return i;
    return -1;
}

static bool isBanned(const IPAddress& ip) {
    int i = findBan(ip);
    return i >= 0 && millis() < gBans[i].banUntil;
}

static void noteAuthFail(const IPAddress& ip) {
    int i = findBan(ip);
    if (i < 0) {
        if (gBanCount < 8) { gBans[gBanCount].ip = ip; i = gBanCount++; }
        else return;
    }
    gBans[i].fails++;
    gAuthFailN++;
    if (gBans[i].fails >= BAN_FAILS) {
        gBans[i].banUntil = millis() + BAN_MS;
        snprintf(gLastBanInfo, sizeof(gLastBanInfo), "ban %s fails=%u", ip.toString().c_str(), (unsigned)gBans[i].fails);
        Serial.printf("[SEC] IP %s 已拉黑 10 分钟\n", ip.toString().c_str());
    }
}

// 所有路由开头调用：失败返回 false 并已发送 403
static bool requireAuth() {
    IPAddress ip = web.client().remoteIP();
    if (isBanned(ip)) {
        gBanHits++;
        snprintf(gLastBanInfo, sizeof(gLastBanInfo), "hit %s", ip.toString().c_str());
        web.send(403, "text/plain", "Forbidden");
        return false;
    }
    if (!web.hasArg("token") || web.arg("token") != AUTH_TOKEN) {
        noteAuthFail(ip);
        web.send(403, "text/plain", "Forbidden");
        return false;
    }
    int i = findBan(ip);
    if (i >= 0) gBans[i].fails = 0;
    return true;
}

// ============================================================================
// Web 控制台页面（PROGMEM，{{TOKEN}} 由服务端替换）
// ============================================================================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>ESP32 iPad 控制台</title>
<style>
:root{--fg:#1a1a1a;--bg:#f5f6f7;--card:#fff;--line:#ddd;--ac:#0a66c2}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:8px 12px;font-size:13px;background:var(--card);border-bottom:1px solid var(--line);position:sticky;top:0;z-index:2}
#wrap{position:relative;margin:10px auto;max-width:100%;text-align:center}
#screen{max-width:100%;max-height:68vh;border:1px solid var(--line);background:#eee;border-radius:6px;touch-action:manipulation}
.rip{position:absolute;width:36px;height:36px;margin:-18px 0 0 -18px;border:2px solid var(--ac);border-radius:50%;animation:r .7s ease-out forwards;pointer-events:none}
@keyframes r{from{transform:scale(.4);opacity:.9}to{transform:scale(1.7);opacity:0}}
#grid{position:absolute;display:none;pointer-events:none;z-index:1}
#grid svg{width:100%;height:100%;display:block}
h3{margin:14px 14px 4px;font-size:13px;color:#666}
.bar{display:flex;flex-wrap:wrap;gap:8px;padding:2px 12px;max-width:960px;margin:0 auto}
.bar button{flex:1;min-width:72px;padding:12px 0;font-size:14px;border:1px solid var(--line);background:var(--card);border-radius:8px;color:var(--fg)}
.bar button:active{background:var(--ac);color:#fff}
.hint{display:block;padding:6px 14px;color:#888;font-size:11px;line-height:1.7}
</style></head><body>
<header>状态：<span id="st">连接中…</span></header>
<div id="wrap"><img id="screen" src="/screenshot.jpg?token={{TOKEN}}&t=0" alt="暂无截图"><div id="grid"></div></div>
<h3>快捷功能</h3>
<div class="bar">
<button onclick="act('screenshot')">触发按钮4</button>
<button onclick="act('home')">主屏(右键)</button>
<button onclick="act('double')">双击</button>
<button onclick="act('scroll_up')">上滑</button>
<button onclick="act('scroll_down')">下滑</button>
<button onclick="toggleGrid()">网格标定</button>
<button onclick="toggleMode()" id="mbtn">模式</button>
</div>
<h3>鼠标验证</h3>
<div class="bar">
<button onclick="mo('zero')">归零到左上角</button>
<button onclick="mo('center')">移到中心</button>
<button onclick="mo('click')">原地单击</button>
<button onclick="mo('rclick')">原地右键</button>
</div>
<h3>快捷指令触发（鼠标按钮 → iPad 辅助触控绑定）</h3>
<div class="bar">
<button onclick="mbtn(8)">按钮4</button>
<button onclick="mbtn(16)">按钮5</button>
</div>
<small class="hint">
· 点击截图任意位置 = 相对移动到该处→左键单击；auto=1 时点击后自动按一次按钮4触发截图。<br>
· 截图触发路径：iPad 设置→辅助功能→触控→辅助触控（开）→指针设备→设备→自定义按钮
  → 按「按钮4」→ 选「快捷指令·截屏上传」（或内置「截屏」）。同页「鼠标键」必须保持关闭。<br>
· 「主屏」发送鼠标右键，需在 辅助功能→辅助触控 中把鼠标右键绑定自定义手势（底部上滑）。<br>
· 滚轮：若 iPad 上滚动方向相反，用 /config?invert=1 翻转。
</small>
<script>
const T='{{TOKEN}}';
const img=document.getElementById('screen'), st=document.getElementById('st');
let invert=0;
function refresh(){ img.src='/screenshot.jpg?token='+T+'&t='+Date.now(); }
let gFps=0, shotTimer=null, lastFid=0, shotBusy=false;
function shotWait(gotNew){
  // 新帧后 60ms 立即追帧（消除固定轮询相位差）；无新帧按上游 fps 放缓：
  // 900/fps 略快于帧到达间隔，让轮询总赶在下一帧落地前就位（下限 250ms 护服务器）
  if(gotNew) return 60;
  return Math.max(250, Math.min(1000, Math.round(900/(gFps||2))));
}
async function shotCycle(){
  if(shotBusy) return;
  shotBusy=true;
  let gotNew=false;
  try{
    const r=await fetch('/shotf?token='+T+'&since='+lastFid+'&r='+Math.random());
    const fid=r.headers.get('X-Fid');
    if(fid && +fid!==lastFid){
      lastFid=+fid;
      const b=await r.blob();
      if(img.dataset.u) URL.revokeObjectURL(img.dataset.u);
      const u=URL.createObjectURL(b); img.dataset.u=u; img.src=u;
      gotNew=true;
    }
  }catch(e){}
  shotBusy=false;
  shotTimer=setTimeout(shotCycle, shotWait(gotNew));
}
shotCycle();
img.addEventListener('pointerdown',e=>{
  const r=img.getBoundingClientRect(), x=(e.clientX-r.left)/r.width, y=(e.clientY-r.top)/r.height;
  if(x<0||x>1||y<0||y>1)return;
  const d=document.createElement('div'); d.className='rip';
  d.style.left=(e.clientX-r.left)+'px'; d.style.top=(e.clientY-r.top)+'px';
  document.getElementById('wrap').appendChild(d); setTimeout(()=>d.remove(),700);
  st.textContent='已发送点击 ('+x.toFixed(2)+', '+y.toFixed(2)+')';
  fetch('/click?token='+T+'&x='+x.toFixed(4)+'&y='+y.toFixed(4));
});
// ---- 标定网格：10% 一格 + 坐标标签，覆盖在截图上 ----
const grid=document.getElementById('grid');
let gridOn=false;
function buildGrid(){
  let s='<svg viewBox="0 0 100 100" preserveAspectRatio="none">';
  s+='<rect x="0" y="0" width="100" height="100" fill="none" stroke="rgba(255,0,0,.55)" stroke-width="0.5"/>';
  for(let i=1;i<10;i++){
    s+='<line x1="'+i*10+'" y1="0" x2="'+i*10+'" y2="100" stroke="rgba(255,0,0,.35)" stroke-width="0.25"/>';
    s+='<line x1="0" y1="'+i*10+'" x2="100" y2="'+i*10+'" stroke="rgba(255,0,0,.35)" stroke-width="0.25"/>';
  }
  for(let i=1;i<10;i++)for(let j=1;j<10;j++)
    s+='<text x="'+(i*10+1.2)+'" y="'+(j*10+3)+'" font-size="3.2" fill="rgba(200,0,0,.85)" font-family="monospace">.'+i+j+'</text>';
  s+='</svg>';
  grid.innerHTML=s;
}
function fitGrid(){
  const wr=document.getElementById('wrap').getBoundingClientRect(), r=img.getBoundingClientRect();
  grid.style.left=(r.left-wr.left)+'px'; grid.style.top=(r.top-wr.top)+'px';
  grid.style.width=r.width+'px'; grid.style.height=r.height+'px';
}
function toggleGrid(){
  gridOn=!gridOn;
  if(gridOn&&!grid.innerHTML)buildGrid();
  fitGrid();
  grid.style.display=gridOn?'block':'none';
  st.textContent=gridOn?'网格已开启：点击截图发送坐标，坐标显示在状态栏':'网格已关闭';
}
window.addEventListener('resize',()=>{if(gridOn)fitGrid();});
img.addEventListener('load',()=>{if(gridOn)fitGrid();});
function act(a){ fetch('/action?token='+T+'&a='+a).then(r=>r.text()).then(t=>st.textContent=t).catch(()=>st.textContent='请求失败'); }
function mo(o){ fetch('/mouse?token='+T+'&op='+o).then(r=>r.text()).then(t=>st.textContent=t); }
let curMode='rel';
function toggleMode(){ const to=curMode==='abs'?'rel':'abs';
  fetch('/config?token='+T+'&mode='+to).then(r=>r.text()).then(t=>{curMode=to;
  document.getElementById('mbtn').textContent=to==='abs'?'切到rel':'切到abs';
  st.textContent='已切换 '+t+'（换平板/重新配对后才测 abs）';}); }
function mbtn(b){ fetch('/mouse?token='+T+'&op=btn&bit='+b+'&hold=120').then(r=>r.text()).then(t=>st.textContent=t); }
async function poll(){ try{
  const j=await(await fetch('/status?token='+T)).json(); invert=j.invert||0; curMode=j.mode||'rel';
  document.getElementById('mbtn').textContent=curMode==='abs'?'切到rel':'切到abs';
  st.textContent='BLE:'+(j.ble?(j.sub?'已连接':'未订阅'):'未连接')
    +'['+(j.mode||'rel')+']'
    +' 截图:'+(j.shot?Math.round(j.shot/1024)+'KB':'无')+' '+j.ago+'s前'
    +' · IP6:'+(j.ip6||'获取中')+' · 标定:'+j.cx+'x'+j.cy;
  // 广播速率自适应：记录 fps，截图循环按 900/fps 自调节（追帧式，见 shotWait）
  if(j.fps){ gFps=+j.fps; }
 }catch(e){ st.textContent='离线'; } }
setInterval(poll,3000); poll();
</script></body></html>
)rawliteral";

// iPad 端自动标定测试页：全屏网格(含 0/100 边缘刻度)，点击即回传实际命中坐标
static const char INDEX_CALIB[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1,user-scalable=no">
<style>html,body{margin:0;width:100%;height:100%;background:#151515;overflow:hidden;touch-action:manipulation}
#m{position:fixed;top:6px;left:8px;right:8px;color:#4f4;font:700 22px monospace;z-index:9;text-shadow:0 0 4px #000}
svg{width:100%;height:100%;display:block}</style></head><body>
<div id=m>标定页：保持打开，等自动点击<br><span id=o>待命</span></div>
<svg id=g viewBox="0 0 100 100" preserveAspectRatio=none></svg>
<script>const T='{{TOKEN}}';
(function(){let s='<rect x=0 y=0 width=100 height=100 fill=none stroke=rgba(0,255,120,.6) stroke-width=.4/>';
for(let i=0;i<=10;i++){const p=i*10;
 s+='<line x1='+p+' y1=0 x2='+p+' y2=100 stroke=rgba(0,255,120,.28) stroke-width=.22/>';
 s+='<line x1=0 y1='+p+' x2=100 y2='+p+' stroke=rgba(0,255,120,.28) stroke-width=.22/>';
 s+='<text x='+(p+.8)+' y=5 font-size=3 fill=rgba(0,255,120,.85) font-family=monospace>'+p+'</text>';
 s+='<text x=1 y='+(p-1)+' font-size=3 fill=rgba(255,220,0,.85) font-family=monospace>'+p+'</text>';}
document.getElementById('g').innerHTML=s;})();
const o=document.getElementById('o');
document.addEventListener('pointerdown',e=>{
 const ax=e.clientX/innerWidth, ay=e.clientY/innerHeight;
 o.textContent='命中 '+(ax*100).toFixed(1)+'% , '+(ay*100).toFixed(1)+'%';
 fetch('/calrep?token='+T+'&ax='+ax.toFixed(5)+'&ay='+ay.toFixed(5)
  +'&vw='+innerWidth+'&vh='+innerHeight+'&sw='+screen.width+'&sh='+screen.height);
});
</script></body></html>
)rawliteral";

// ============================================================================
// Web 路由处理
// ============================================================================

static void handleRoot() {
    if (!requireAuth()) return;
    String html(FPSTR(INDEX_HTML));
    html.replace("{{TOKEN}}", AUTH_TOKEN);
    web.send(200, "text/html", html);
}

static void handleShot() {
    if (!requireAuth()) return;
    xSemaphoreTake(gJpegMx, portMAX_DELAY);
    size_t len = gJpegLen;
    xSemaphoreGive(gJpegMx);
    if (len == 0) {
        web.send(404, "text/plain", "no screenshot yet - /upload first");
        return;
    }
    web.setContentLength(len);
    web.send(200, "image/jpeg", "");
    WiFiClient c = web.client();
    size_t off = 0;
    uint32_t t0 = millis();
    xSemaphoreTake(gJpegMx, portMAX_DELAY);
    while (off < len) {
        int w = c.write(gJpeg + off, len - off);
        if (w <= 0) break;
        off += w;
    }
    xSemaphoreGive(gJpegMx);
    Serial.printf("[SHOT] len=%u sent=%u dur=%ums\n", (unsigned)len, (unsigned)off, millis() - t0);
}

// 轻量轮询端点：帧号没变就回 "same"（几十字节），控制台不再反复拉整张 JPEG。
// 背景：iPad 上多客户端（控制台页/标定页/App/广播扩展）同时轮询时，
// 单线程同步 WebServer 被整图拉取挤爆，广播扩展上传超时/被拒 → 300 连败自杀（2026-11-22 实测）。
static void handleShotF() {
    if (!requireAuth()) return;
    if (web.hasArg("since")) {
        uint32_t since = (uint32_t) strtoul(web.arg("since").c_str(), nullptr, 10);
        if (gFrameId > 0 && since == gFrameId) {
            web.sendHeader("X-Fid", String(gFrameId));
            web.send(200, "text/plain", "same");
            return;
        }
    }
    xSemaphoreTake(gJpegMx, portMAX_DELAY);
    size_t len = gJpegLen;
    uint32_t fid = gFrameId;
    xSemaphoreGive(gJpegMx);
    if (len == 0) { web.send(404, "text/plain", "no frame yet"); return; }
    web.sendHeader("X-Fid", String(fid));
    web.setContentLength(len);
    web.send(200, "image/jpeg", "");
    WiFiClient c = web.client();
    size_t off = 0;
    uint32_t t0 = millis();
    xSemaphoreTake(gJpegMx, portMAX_DELAY);
    while (off < len) {
        int w = c.write(gJpeg + off, len - off);
        if (w <= 0) break;
        off += w;
    }
    xSemaphoreGive(gJpegMx);
    Serial.printf("[SHOTF] len=%u sent=%u dur=%ums\n", (unsigned)len, (unsigned)off, millis() - t0);
}

// multipart 分片回调：iPad 快捷指令「请求体=文件」时走这里（二进制安全）
static void handleUploadChunk() {
    HTTPUpload& up = web.upload();
    if (up.status == UPLOAD_FILE_START) {
        gUpUsed = true; gUpBig = false; gUpLen = 0; gUpMagicOk = false;
        gUpT0 = millis();
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (gUpLen + up.currentSize > JPEG_CAP) {   // 防内存爆破：超限即断
            gUpBig = true;
            return;
        }
        memcpy(gJpeg + gUpLen, up.buf, up.currentSize);
        gUpLen += up.currentSize;
    } else if (up.status == UPLOAD_FILE_END && !gUpBig) {
        // JPEG 魔数校验：FF D8 FF
        if (gUpLen >= 3 && gJpeg[0] == 0xFF && gJpeg[1] == 0xD8 && gJpeg[2] == 0xFF) {
            xSemaphoreTake(gJpegMx, portMAX_DELAY);
            gJpegLen = gUpLen;
            gLastUpload = millis();
            gFrameId++;
            gUpMagicOk = true;
            xSemaphoreGive(gJpegMx);
            Serial.printf("[UP] len=%u dur=%ums\n", (unsigned)gUpLen, millis() - gUpT0);
            Serial.printf("[UP] 收到截图 %u bytes (multipart)\n", (unsigned)gUpLen);
        } else {
            gUpFail++;
            Serial.printf("[UP] 非 JPEG 数据 %u bytes，丢弃\n", (unsigned)gUpLen);
            gUpLen = 0;
        }
    }
}

static void handleUpload() {
    if (!requireAuth()) return;
    // 判定顺序敏感：先查 raw body（非 multipart 时 WebServer 才把 body 存进 arg("plain")），
    // 再查 multipart 状态。曾用 web.header("Content-Type") 判定 multipart，但部分客户端
    // （广播扩展的 URLSession）下该头取不到/不匹配 → 全部落入 no data（fid 照涨、应答全 400），
    // 扩展侧 300 连败自杀（2026-11-22 实测）。现在：chunk 回调跑过 = multipart 成立，直接信任其状态。
    if (web.hasArg("plain")) {
        String b = web.arg("plain");
        if (b.length() > JPEG_CAP) { gUpFail++; web.send(413, "text/plain", "payload too large"); return; }
        if (b.length() >= 3 && (uint8_t)b[0] == 0xFF && (uint8_t)b[1] == 0xD8 && (uint8_t)b[2] == 0xFF) {
            memcpy(gJpeg, b.begin(), b.length());
            gJpegLen = b.length();
            gLastUpload = millis();
            gFrameId++;
            gUpOk++;
            web.send(200, "text/plain", "ok");
            return;
        }
        gUpFail++;
        web.send(400, "text/plain", "body not a jpeg (multipart truncated?) - Shortcuts body type must be File");
        return;
    }
    if (gUpUsed && gUpLen > 0 && gUpMagicOk) { gUpOk++; web.send(200, "text/plain", "ok"); return; }
    if (gUpBig) { gUpFail++; web.send(413, "text/plain", "payload too large (limit 4MB)"); return; }
    gUpFail++;
    web.send(400, "text/plain", "no data");
}

static void handleClick() {
    if (!requireAuth()) return;
    // 门禁与 /mouse 一致：任一鼠标通道可用即可。旧门禁只查 bleSubscribed（Boot Mouse 位），
    // Report 模式下恒 0 → 主控制台 /click 全部 503，点击从未发出（2026-11-22 修复）。
    if (!bleConnected || (!bleSubscribed && !mouseRepSub)) {
        web.send(503, "text/plain", "BLE not connected/subscribe - iPad 未配对或未订阅");
        return;
    }
    float fx = web.hasArg("x") ? web.arg("x").toFloat() : 0.5f;
    float fy = web.hasArg("y") ? web.arg("y").toFloat() : 0.5f;
    if (doClick(fx, fy)) web.send(200, "text/plain", "ok");
    else web.send(500, "text/plain", "send failed");
}

// 验证面板：单独的鼠标原语
static void handleMouse() {
    if (!requireAuth()) return;
    String op = web.hasArg("op") ? web.arg("op") : "";
    // pre=1/0 切换 Report ID 前缀（复合描述符诊断，2026-11-22）
    if (web.hasArg("pre")) gMousePre = (web.arg("pre") == "1");
    // 门禁只查链路 + 任一鼠标通道（Report ID1 或 Boot Mouse）可用，具体通道由 sendMouse 选择
    if (!bleConnected || (!bleSubscribed && !mouseRepSub)) {
        web.send(503, "text/plain", "BLE 未连接");
        return;
    }
    if (gMode == 1) {   // ---- abs 模式 ----
        if (op == "center") { absClick(0.5f, 0.5f); web.send(200, "text/plain", "abs 已移到中心"); }
        else if (op == "click") { absClick(web.hasArg("x") ? web.arg("x").toFloat() : 0.5f,
                                           web.hasArg("y") ? web.arg("y").toFloat() : 0.5f);
                                  web.send(200, "text/plain", "abs 已单击"); }
        else if (op == "move") {   // 悬停移动（fraction 坐标）
            float x = web.hasArg("x") ? web.arg("x").toFloat() : 0.5f;
            float y = web.hasArg("y") ? web.arg("y").toFloat() : 0.5f;
            sendDigitizer((uint16_t)(x * 32767), (uint16_t)(y * 32767), false);
            web.send(200, "text/plain", "abs 已悬停移动");
        }
        else if (op == "swipe") {  // x0,y0,x1,y1 均 fraction
            float x0 = web.hasArg("x0") ? web.arg("x0").toFloat() : 0.5f;
            float y0 = web.hasArg("y0") ? web.arg("y0").toFloat() : 0.3f;
            float x1 = web.hasArg("x1") ? web.arg("x1").toFloat() : 0.5f;
            float y1 = web.hasArg("y1") ? web.arg("y1").toFloat() : 0.7f;
            absSwipe(x0, y0, x1, y1);
            web.send(200, "text/plain", "abs 已滑动");
        }
        else if (op == "zero" || op == "rclick" || op == "move_counts")
            web.send(400, "text/plain", "abs 模式无归零/右键/counts 概念，/config?mode=rel 切回");
        else web.send(400, "text/plain", "unknown op (abs)");
        return;
    }
    // ---- rel 模式（原有逻辑）----
    if (op == "zero") { mouseZero(); gPtrX = 0; gPtrY = 0; gPtrKnown = true; savePtrPos(); web.send(200, "text/plain", "已归零(左上角)"); }
    else if (op == "center") { mouseZero(); mouseMoveTo(gCountsX / 2, gCountsY / 2); gPtrX = gCountsX/2; gPtrY = gCountsY/2; gPtrKnown = true; savePtrPos(); web.send(200, "text/plain", "已移到中心"); }
    else if (op == "move") {   // 校准用：按原始 counts 相对移动
        int dx = web.hasArg("dx") ? web.arg("dx").toInt() : 0;
        int dy = web.hasArg("dy") ? web.arg("dy").toInt() : 0;
        mouseMoveToRelative(dx, dy);
        if (gPtrKnown) { gPtrX += dx; gPtrY += dy; savePtrPos(); }
        web.send(200, "text/plain", "已移动 counts(" + String(dx) + "," + String(dy) + ")");
    }
    else if (op == "syncbase") {
        // 重建指针基准：iPad 蓝牙鼠标在 BLE 重连/重启后会被系统把指针挪到别处
        // (本次落到屏幕底部 ry≈1.0)，而固件 NVS 基准还停在旧位置 → 点击整体偏移。
        // 用页面回传的真实落点(视口分数)反推当前指针 counts，重写基准，不移动指针。
        // 可显式传 x/y(视口分数) 覆盖；否则用最近一次点击回传 gRepAx/gRepAy。
        float bx = web.hasArg("x") ? web.arg("x").toFloat() : gRepAx;
        float by = web.hasArg("y") ? web.arg("y").toFloat() : gRepAy;
        if (!web.hasArg("x") && gRepMs == 0) {
            web.send(400, "text/plain", "无有效回传: 先点一次或传 x/y");
        } else {
            gPtrX = (int)(constrain(bx, 0.0f, 1.0f) * gCountsX);
            gPtrY = (int)(constrain(by, 0.0f, 1.0f) * gCountsY);
            gPtrKnown = true;
            savePtrPos();
            web.send(200, "text/plain", "基准同步 ptr=" + String(gPtrX) + "," + String(gPtrY));
        }
    }
    else if (op == "click") { mouseClick(1); web.send(200, "text/plain", "已单击"); }
    else if (op == "rclick") { mouseClick(2); web.send(200, "text/plain", "已右键"); }
    else if (op == "mclick") { mouseClick(4); web.send(200, "text/plain", "已中键"); }
    else if (op == "btn") {   // 诊断：/mouse?op=btn&bit=1|2|4&hold=ms
        int bit = web.hasArg("bit") ? web.arg("bit").toInt() : 1;
        int hold = web.hasArg("hold") ? web.arg("hold").toInt() : 60;
        mouseHoldBtn((uint8_t)bit, hold);
        web.send(200, "text/plain", "已发按钮 bit=" + String(bit) + " hold=" + String(hold) + "ms");
    }
    else web.send(400, "text/plain", "unknown op");
}

// ---- 自动标定接口 ----
static void handleCalibPage() {
    if (!requireAuth()) return;
    String html(FPSTR(INDEX_CALIB));
    html.replace("{{TOKEN}}", AUTH_TOKEN);
    web.send(200, "text/html", html);
}

static void handleCalRep() {   // iPad 测试页回传实际命中
    if (!requireAuth()) return;
    gRepAx = web.arg("ax").toFloat();
    gRepAy = web.arg("ay").toFloat();
    if (web.hasArg("vw")) gCalVw = web.arg("vw").toFloat();
    if (web.hasArg("vh")) gCalVh = web.arg("vh").toFloat();
    if (web.hasArg("sw")) gCalSw = web.arg("sw").toFloat();
    if (web.hasArg("sh")) gCalSh = web.arg("sh").toFloat();
    gRepMs = millis();
    web.send(200, "text/plain", "ok");
}

static void handleAutoCal() {
    if (!requireAuth()) return;
    if (gAutoCalOn) { web.send(200, "text/plain", "进行中"); return; }
    for (int i = 0; i < CAL_N; i++) gCalGot[i] = false;
    gCalLog = "";
    gCalApplied = false; gCalKx = gCalKy = 0;
    gCalZeroed = false; gCurCx = gCurCy = 0;
    gCalStep = 0; calPhase = 0; gCalStepMs = millis();
    gAutoCalOn = true;
    web.send(200, "text/plain", "自动标定已启动(约40s)，请勿触碰iPad");
}

static void handleCalResult() {
    if (!requireAuth()) return;
    String s = "{\"on\":";
    s += gAutoCalOn ? "true" : "false";
    s += ",\"step\":" + String(gCalStep);
    s += ",\"kx\":" + String(gCalKx, 3) + ",\"ky\":" + String(gCalKy, 3);
    s += ",\"cx\":" + String(gCountsX) + ",\"cy\":" + String(gCountsY);
    s += ",\"log\":\"" + gCalLog + "\"}";
    web.send(200, "application/json", s);
}

// （键盘组合键验证面板 handleKbd 已随键盘路径整体删除，2026-11-22）

static void handleAction() {
    if (!requireAuth()) return;
    if (!bleConnected) { web.send(503, "text/plain", "BLE 未连接"); return; }
    String a = web.hasArg("a") ? web.arg("a") : "";
    if (a == "screenshot") { mouseHoldBtn(AUTO_BTN4_BIT, 120); web.send(200, "text/plain", "已按按钮4（iPad 辅助触控绑定的动作）"); }
    else if (a == "home") {   // 主屏 = 鼠标右键，iPad 侧在辅助触控里把右键绑成底部上滑手势
        mouseClick(2);
        web.send(200, "text/plain", "已发送右键(主屏)");
    }
    else if (a == "double") { mouseClick(1); delay(120); mouseClick(1); web.send(200, "text/plain", "已双击"); }
    else if (a == "scroll_up" || a == "scroll_down") {
        int n = web.hasArg("n") ? constrain(web.arg("n").toInt(), 1, 100) : 10;
        int8_t w = (a == "scroll_up") ? 1 : -1;   // 正=前滚(页面上滑)
        if (gInvert) w = -w;
        for (int i = 0; i < n; i++) { sendMouse(0, 0, w); delay(12); }
        web.send(200, "text/plain", a + " x" + String(n));
    }
    else web.send(400, "text/plain", "unknown action");
}

static void handleStatus() {
    if (!requireAuth()) return;
    uint32_t len; uint32_t last;
    xSemaphoreTake(gJpegMx, portMAX_DELAY);
    len = gJpegLen; last = gLastUpload;
    xSemaphoreGive(gJpegMx);
    String s = "{";
    s += "\"ble\":" + String(bleConnected ? 1 : 0);
    s += ",\"sub\":" + String(bleSubscribed ? 1 : 0);
    // 通道诊断：当前协议模式(1=Report, 0=Boot) + 鼠标各通道订阅位
    s += ",\"proto\":" + String(gProtoMode);
    s += ",\"msub\":" + String(mouseRepSub ? 1 : 0);
    s += ",\"mpre\":" + String(gMousePre ? 1 : 0);
    s += ",\"shot\":" + String(len);
    s += ",\"ago\":" + String(last ? (int)((millis() - last) / 1000) : -1);
    s += ",\"cx\":" + String(gCountsX);
    s += ",\"cy\":" + String(gCountsY);
    s += ",\"auto\":" + String(gAutoShot ? 1 : 0);
    s += ",\"invert\":" + String(gInvert ? 1 : 0);
    s += ",\"fps\":" + String(gFps);
    s += ",\"qse\":" + String(gJpegEdge);
    s += ",\"qql\":" + String(gJpegQ);
    // 上传/鉴权遥测：upok=成功上传数 upfail=被拒/无效上传数 a403=鉴权失败数 banhits=拉黑命中数
    // fid=当前帧号 ban=最近拉黑/拒绁记录（空=无）。广播扩展“上传持续失败”时先看这四项。
    s += ",\"upok\":" + String(gUpOk);
    s += ",\"upfail\":" + String(gUpFail);
    s += ",\"a403\":" + String(gAuthFailN);
    s += ",\"banhits\":" + String(gBanHits);
    s += ",\"fid\":" + String(gFrameId);
    s += ",\"ban\":\"" + String(gLastBanInfo) + "\"";
    s += ",\"mode\":\"" + String(gMode == 1 ? "abs" : "rel") + "\"";
    s += ",\"heap\":" + String((uint32_t)ESP.getFreeHeap());
    // 最近一次页面回传的真实落点(视口分数) + 时间戳, 供自动读落点调系数
    s += ",\"rx\":" + String(gRepAx, 4);
    s += ",\"ry\":" + String(gRepAy, 4);
    s += ",\"rms\":" + String(gRepMs);
    // 光标逻辑位置(counts)与目标, 供诊断跟踪漂移
    s += ",\"px\":" + String(gPtrX);
    s += ",\"py\":" + String(gPtrY);
    // 页面视口/屏幕尺寸, 供视口坐标↔屏幕物理坐标换算(顶栏补偿)
    s += ",\"vw\":" + String(gCalVw, 0) + ",\"vh\":" + String(gCalVh, 0);
    s += ",\"sw\":" + String(gCalSw, 0) + ",\"sh\":" + String(gCalSh, 0);
    s += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    s += ",\"ip6\":\"" + gIPv6Global + "\"";
    s += "}";
    web.send(200, "application/json", s);
}

// /config 运行时调参（全部持久化 NVS，重启保留）
// 例: /config?token=..&cx=1300&cy=950&auto=0&shotmod=5&shotkey=16&zero=20&invert=1
//     /config?token=..&ssid=xxx&pass=yyy  → 保存 WiFi 并提示重启
static void handleConfig() {
    if (!requireAuth()) return;
    nvs.begin("padctl", false);
    String out = "已保存: ";
    if (web.hasArg("cx"))   { gCountsX  = constrain(web.arg("cx").toInt(), 80, 16000); nvs.putUShort("cx", gCountsX); out += "cx=" + String(gCountsX) + " "; }
    if (web.hasArg("cy"))   { gCountsY  = constrain(web.arg("cy").toInt(), 80, 16000); nvs.putUShort("cy", gCountsY); out += "cy=" + String(gCountsY) + " "; }
    if (web.hasArg("zero")) { gZeroPush = constrain(web.arg("zero").toInt(), 5, 130);     nvs.putUChar("zero", gZeroPush); out += "zero=" + String(gZeroPush) + " "; }
    if (web.hasArg("auto")) { gAutoShot = web.arg("auto").toInt() != 0;                 nvs.putUChar("auto", gAutoShot); out += "auto=" + String(gAutoShot) + " "; }
    if (web.hasArg("invert"))  { gInvert = web.arg("invert").toInt() != 0; nvs.putUChar("invert", gInvert); out += "invert=" + String(gInvert) + " "; }
    if (web.hasArg("fps"))   { gFps = constrain(web.arg("fps").toInt(), 1, 5); nvs.putUChar("fps", gFps); out += "fps=" + String(gFps) + " "; }
    if (web.hasArg("edge"))  { gJpegEdge = constrain(web.arg("edge").toInt(), 480, 1280); nvs.putUShort("qse", gJpegEdge); out += "edge=" + String(gJpegEdge) + " "; }
    if (web.hasArg("q"))     { gJpegQ = constrain(web.arg("q").toInt(), 20, 85); nvs.putUChar("qql", gJpegQ); out += "q=" + String(gJpegQ) + " "; }
    if (web.hasArg("mode")) {
        String m = web.arg("mode");
        gMode = (m == "abs") ? 1 : 0;
        nvs.putUChar("mode", gMode);
        out += "mode=" + String(gMode == 1 ? "abs(绝对坐标,iOS17+)" : "rel(相对鼠标)") + " ";
    }
    if (web.hasArg("unbond")) {
        // 清空 ESP32 绑定表并重启：反复忽略/重配对后密钥残留会让配对静默失败
        // （症状：connected bonded=0 → encrypted=0，iPad 收到报告但不响应）。
        // 用法：iPad 先「忽略此设备」→ 调本接口 → 设备重启广播 → 重新配对
        out += "clearing bonds, restarting... ";
        web.send(200, "text/plain", "绑定表已清空，设备重启中");
        delay(300);
        // 重新配对后 iPad 指针回到中心, 清除旧的光标位置基准
        { Preferences p; if (p.begin("padctl", false)) { p.putBool("hasptr", false); p.end(); } }
        NimBLEDevice::deleteAllBonds();
        ESP.restart();
        return;
    }
    if (web.hasArg("ssid")) {
        nvs.putString("ssid", web.arg("ssid"));
        if (web.hasArg("pass")) nvs.putString("pass", web.arg("pass"));
        out += "SSID(重启生效) ";
    }
    nvs.end();
    web.send(200, "text/plain", out);
}

// ============================================================================
// WiFi（IPv4 + 全局 IPv6）
// ============================================================================
static String gSsid, gPass;

// esp_ip6_addr_t → 标准冒分十六进制字符串
static String ip6ToString(const esp_ip6_addr_t& a) {
    const uint8_t* b = (const uint8_t*)a.addr;
    char out[64];
    int n = 0;
    for (int i = 0; i < 8; i++) {
        n += snprintf(out + n, sizeof(out) - n, i ? ":%x" : "%x",
                      (b[2 * i] << 8) | b[2 * i + 1]);
    }
    return String(out);
}

// 判断是否为可用的全局单播 IPv6（排除全零与 fe80::/10 链路本地）
static bool isGlobalV6(const esp_ip6_addr_t& a) {
    const uint8_t* b = (const uint8_t*)a.addr;
    for (int i = 0; i < 16; i++)
        if (b[i]) break;
        else if (i == 15) return false;          // 全零 = 无地址
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return false;  // link-local
    return true;
}

static bool fetchGlobalIPv6() {
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_ip6_addr_t a;
    if (sta && esp_netif_get_ip6_global(sta, &a) == ESP_OK && isGlobalV6(a)) {
        gIPv6Global = ip6ToString(a);
        return true;
    }
    return false;
}

static void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.enableIPv6();          // 请求 SLAAC 全局 IPv6（家庭宽带需已开通 IPv6）
    WiFi.begin(gSsid.c_str(), gPass.c_str());

    Serial.printf("[WiFi] 连接 %s", gSsid.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] 连接失败！20 秒后在 loop 中持续重试");
        return;
    }
    Serial.printf("[WiFi] IPv4: %s  RSSI:%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

    // 等全局 IPv6 通过 SLAAC 到位（最多 15 秒）
    t0 = millis();
    while (!fetchGlobalIPv6() && millis() - t0 < 15000) delay(500);
    if (gIPv6Global.length())
        Serial.printf("[WiFi] 全局 IPv6: http://[%s]/?token=%s\n", gIPv6Global.c_str(), AUTH_TOKEN);
    else
        Serial.println("[WiFi] 暂无全局 IPv6（检查路由器 IPv6/防火墙），局域网用 IPv4 也可联调");
}

// ============================================================================
// setup / loop
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== ESP32-S3 iPad 远控 (阶段1&2: 联调验证版) ===");

    // ---- NVS 读取持久化参数 ----
    nvs.begin("padctl", true);
    gCountsX = nvs.getUShort("cx", SCREEN_MAX_X_DEFAULT);
    gCountsY = nvs.getUShort("cy", SCREEN_MAX_Y_DEFAULT);
    gZeroPush = nvs.getUChar("zero", ZERO_PUSH_REPEAT);
    gAutoShot = nvs.getUChar("auto", AUTO_SHOT_DEFAULT);
    gInvert  = nvs.getUChar("invert", 0);
    gFps = nvs.getUChar("fps", 1);
    gJpegEdge = nvs.getUShort("qse", 720);
    gJpegQ = nvs.getUChar("qql", 40);
    // 恢复光标位置: 重启后 iPad 指针仍在原处, 恢复逻辑坐标即可直接相对位移,
    // 第一次点击不用归零(避免下拉手势)。hasPtr 标记区分"从未保存过"。
    bool hasPtr = nvs.getBool("hasptr", false);
    if (hasPtr) {
        gPtrX = nvs.getInt("ptrx", gCountsX / 2);
        gPtrY = nvs.getInt("ptry", gCountsY / 2);
        gPtrKnown = true;
    } else {
        gPtrKnown = false;   // 首次配对: doClick 会假设中心建立基准
    }
    gMode    = 0;   // 本固件 rel-only：实测 iPadOS 17 蓝牙 HID 拒绝绝对坐标轴，
                    // abs 仅 iOS 18+（AScript 官方文档确认）。mode=abs 已禁用
    gSsid = nvs.getString("ssid", WIFI_SSID);
    gPass = nvs.getString("pass", WIFI_PASS);
    nvs.end();
    Serial.printf("[CFG] 标定 counts=%dx%d zero=%d autoShot(按钮4)=%d mode=%s\n",
                  gCountsX, gCountsY, gZeroPush, gAutoShot,
                  gMode == 1 ? "abs" : "rel");

    // ---- PSRAM 截图缓冲（交接文档硬指标：必须 ps_malloc）----
    gJpeg = (uint8_t*)heap_caps_malloc(JPEG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gJpeg) {
        Serial.println("[FATAL] PSRAM 分配失败！检查 memory_type=qio_opi 配置");
        while (true) delay(1000);
    }
    gJpegMx = xSemaphoreCreateMutex();
    Serial.printf("[PSRAM] 4MB 截图缓冲分配成功 freeHeap=%u\n", (unsigned)ESP.getFreeHeap());

    setupBLE();
    setupWiFi();

    // ---- 路由注册（全部带 token 鉴权）----
    web.on("/", handleRoot);
    web.on("/screenshot.jpg", handleShot);
    web.on("/shotf", handleShotF);
    web.on("/upload", HTTP_POST, handleUpload, handleUploadChunk);
    web.on("/click", handleClick);
    web.on("/mouse", handleMouse);
    web.on("/action", handleAction);
    web.on("/status", handleStatus);
    web.on("/config", handleConfig);
    web.on("/calib", handleCalibPage);
    web.on("/calrep", handleCalRep);
    web.on("/autocal", handleAutoCal);
    web.on("/calresult", handleCalResult);
    web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
    web.begin();

    Serial.printf("\n[READY] 控制台: http://%s/?token=%s\n",
                  WiFi.localIP().toString().c_str(), AUTH_TOKEN);
    Serial.println("[READY] iPad 设置→蓝牙→点击 " DEVICE_NAME " 完成配对，之后断电重启会自动回连");
}

void loop() {
    web.handleClient();
    calibTick();

    // WiFi 断线重连
    static uint32_t lastWifiTry = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 15000) {
        lastWifiTry = millis();
        Serial.println("[WiFi] 断线，尝试重连");
        WiFi.disconnect();
        WiFi.begin(gSsid.c_str(), gPass.c_str());
    }

    // IPv6 到位后补打印一次
    static bool ip6Logged = false;
    if (!ip6Logged && WiFi.status() == WL_CONNECTED && fetchGlobalIPv6()) {
        ip6Logged = true;
        Serial.printf("[WiFi] 全局 IPv6: http://[%s]/?token=%s\n", gIPv6Global.c_str(), AUTH_TOKEN);
    }

    // BLE 心跳保活：每 15s 零位移报告，防止 iPad 判定外设休眠。
    // 旧门禁只查 bleSubscribed（Boot Mouse 位）→ Report 模式下心跳从未发出，一并修复。
    static uint32_t lastBeat = 0;
    if (bleConnected && (bleSubscribed || mouseRepSub) && millis() - lastBeat > KEEPALIVE_MS) {
        lastBeat = millis();
        sendMouse(0, 0, 0, 0);
    }
}
