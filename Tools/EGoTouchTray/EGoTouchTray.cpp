// EGoTouchTray — a small companion that shows pen status when it changes.
//
// The service runs in session 0 and cannot draw, so anything the user sees has to come
// from a process in their own session. This one deliberately stays minimal: it reads the
// PenStatusChannel broadcast, and pops a layered window for a few seconds when the pen
// starts charging.
//
// It must not require elevation — a UAC prompt at every logon would be worse than no
// panel at all — which is why it reads the dedicated read-only channel rather than the
// admin-only IPC control pipe.
//
// The same session boundary is why the pen's side-key double click is injected here and
// not in the service: SendInput needs an interactive input desktop, and a session-0
// service has none — it fails outright with ERROR_ACCESS_DENIED. So the service reports
// the gesture over the channel and this process either turns it into Win+F19 for Windows
// Ink or synchronizes OneNote's private drawing-tool state.
//
// 托盘菜单里改双击行为走的是第三条通道 PenControlChannel：写权限只到「提交一个枚举值」，
// 与只读的状态广播分开，两者都不需要提权。菜单上的对勾取自状态广播回报的当前模式，不是
// 本进程提交过什么——提交可能被服务按枚举校验拒绝，也可能被别处的配置改写。

#include "ManagedResource.h"
#include "PenButtonConfig.h"
#include "PenControlChannel.h"
#include "PenInkShortcut.h"
#include "PenStatusChannel.h"
#include "EGoTouchTrayIpc.h"
#include "BrandMark.h"

#include <d2d1.h>
#include <dwrite.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr const wchar_t* kWindowClass = EGoTouchTrayIpc::kTrayWindowClass;
constexpr wchar_t kMutexName[]   = L"Local\\OpenEGoHubTraySingleton";
constexpr wchar_t kSettingsRegistryKey[] = L"Software\\OpenEGoHub";
constexpr wchar_t kRunRegistryKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"OpenEGoHubTray";

// Design metrics, expressed at 96 DPI. Everything drawn is scaled from these by the DPI
// of the monitor the panel currently sits on — with PER_MONITOR_AWARE_V2 the process gets
// no automatic scaling, so a fixed pixel size would shrink physically on a high-DPI
// display, which is exactly what it did.
constexpr int   kBaseDpi          = 96;

// 笔照是 17:1 的横条，通栏压在顶部；下面一行左名称、右电量，两端各有分量，中间不空。
// 状态文字（「正在充电」「已连接」）不单独占行：电量图标本身就带充电闪电，同一件事说两遍。
// 只在没有电量可显示时，右侧才退回状态词。
constexpr int   kPadXDip          = 16;
constexpr int   kPadYDip          = 16;
// 放得下「HUAWEI M-Pencil（第二代）」加右侧电量组：名称约 182、电量组约 79、中间 8，合计
// 269。留出余量，别卡在临界值上。
constexpr int   kContentWidthDip  = 304;
constexpr int   kPenGapDip        = 12;   // 笔照与下面那行之间
constexpr int   kInfoRowDip       = 26;

// 电量图标原图 54x26，只缩不放——放大会糊。
constexpr int   kBatteryIconHeightDip = 16;
// 阴影画在窗口内部，所以窗口要比卡片大一圈，否则会被窗口边缘裁掉。
constexpr int   kShadowPadDip     = 8;
// 面板中心在屏幕上的相对位置。横向居中、纵向靠上——与系统自己的音量/亮度提示同区域，
// 且离底部任务栏和右下角通知区都远。
constexpr float kAnchorXRatio     = 0.50f;
constexpr float kAnchorYRatio     = 0.10f;


// 取自 PCManager 选件中心的 CD54PenApp\views\window_battery.baml：卡片圆角 16，投影
// BlurRadius=5 / ShadowDepth=0 / Opacity=0.3，文字层级靠不透明度（主 0.9、次 0.6）区分而
// 不是换灰度，强调色 #0A59F7。
constexpr float kCardRadiusDip    = 16.0f;
constexpr float kTextPrimaryAlpha   = 0.9f;
constexpr float kTextSecondaryAlpha = 0.6f;

constexpr UINT_PTR kTimerPoll   = 1;   // channel poll / reconnect
constexpr UINT_PTR kTimerAnim   = 2;   // fade animation
constexpr UINT_PTR kTimerDwell  = 3;   // visible hold
constexpr UINT_PTR kTimerLease  = 4;   // provider lease heartbeat / safe exit

constexpr UINT kPollIntervalMs = 400;
constexpr UINT kAnimIntervalMs = 16;
constexpr UINT kDwellMs        = 3000;
constexpr UINT kFadeInMs       = 150;
constexpr UINT kFadeOutMs      = 300;
constexpr UINT kLeaseIntervalMs = 1000;
constexpr ULONGLONG kSafeExitTimeoutMs = 20000;
constexpr ULONGLONG kReaderReconnectMs = 5000;
// 键盘开关等待回读确认的上限。服务下发命令后由宿主读回 MCU 再经快照回来，实测一秒以内；
// 三秒是留给一次重试的余量，过了就当它没生效。
constexpr ULONGLONG kKbdDetachPendingMs = 3000;

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT kTrayMenuExit = 100;
constexpr UINT kTrayMenuSettings = 102;
constexpr UINT kTrayMenuProvider = 103;
constexpr UINT kTrayMenuKbdDetach = 104;
constexpr UINT kTrayMenuEyeComfort = 105;
// 双击行为各项的命令 ID = 基址 + PenButtonMode 的数值。这样菜单项与枚举一一对应，
// 加一个模式只要多一行 AppendMenu，WM_COMMAND 那边不用跟着改。
constexpr UINT kTrayMenuPenButtonBase = 200;
constexpr UINT kTrayMenuPenButtonSpan = 256;   // 枚举底层类型是 uint8_t

enum class Phase { Hidden, FadeIn, Dwell, FadeOut };

struct App {
    HWND hwnd = nullptr;
    HINSTANCE instance = nullptr;

    // Refreshed whenever the panel is placed, and on WM_DPICHANGED.
    UINT dpi = kBaseDpi;

    PenStatus::Reader reader;
    // 菜单弹出时才按需打开：服务可能比托盘起得晚，也可能中途重启，一次失败不该让菜单
    // 永远置灰。
    PenControl::Client control;
    PenStatus::State state{};
    bool hasState = false;
    ULONGLONG lastReaderReconnectTick = 0;
    bool prevCharging = false;
    bool prevChargingValid = false;
    uint32_t prevNotificationSequence = 0;
    bool prevNotificationValid = false;
    bool prevEraserActive = false;
    bool prevEraserValid = false;
    ULONGLONG lastConnectionNotificationTick = 0;
    ULONGLONG lastDeviationNotificationTick = 0;
    ULONGLONG lastKeyboardConnectionNotificationTick = 0;

    // 「分离后保持无线连接」的在途请求。这个开关的真值在键盘固件里，一次点击要经服务、
    // 命令管道、MCU 才回到快照，中间大约一秒。这段时间里菜单显示用户刚选的那个值：不这样
    // 的话再打开菜单会看到对勾还在原处，看起来像是没点上。
    bool kbdDetachPending = false;
    bool kbdDetachPendingValue = false;
    ULONGLONG kbdDetachPendingDeadline = 0;

    bool providerDesired = true;
    bool autoStart = true;
    bool oneNoteCompatibility = true;
    bool deviceNotifications = true;
    bool exitPending = false;
    ULONGLONG exitDeadlineTick = 0;

    Phase phase = Phase::Hidden;
    DWORD phaseStartTick = 0;
    BYTE alpha = 0;
    bool hovered = false;

    bool darkMode = false;

    // 解码后的 BGRA 预乘像素。D2D 位图绑在渲染目标上，而这里每次绘制都新建一个
    // DCRenderTarget，所以缓存像素而不是位图，每帧再从像素建一次——图都很小，代价可忽略。
    struct ImageCache {
        std::vector<BYTE> pixels;
        UINT width = 0;
        UINT height = 0;
        int  key = -1;              // -1 = 还没加载过
        bool unavailable = false;   // 文件不存在，别再重试
    };
    ImageCache battery;
    ImageCache pen;

    ID2D1Factory* d2dFactory = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    IWICImagingFactory* wicFactory = nullptr;
    IDWriteTextFormat* fmtTitle = nullptr;
    IDWriteTextFormat* fmtBody = nullptr;
    IDWriteTextFormat* fmtPercent = nullptr;
    IDWriteTextFormat* fmtPercentSign = nullptr;

    NOTIFYICONDATAW trayIcon{};
    bool trayIconAdded = false;
    // LoadIcon 返回的是共享图标，销毁它是错的；自己画的那个必须销毁。两者只能靠这个标志
    // 区分——HICON 本身看不出来源。
    bool trayIconOwned = false;
    // 当前这个图标是按什么条件画出来的。任务栏的深浅与应用主题是两个开关，图标跟前者走。
    bool trayIconDark = false;
    int  trayIconPx = 0;
};

App g_app;
std::atomic<bool> g_oneNoteCompatibility{true};
std::mutex g_controlSubmitMutex;

DWORD ReadUserSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsRegistryKey, name,
                     RRF_RT_REG_DWORD, &type, &value, &size) != ERROR_SUCCESS) {
        return fallback;
    }
    return value;
}

bool WriteUserSetting(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsRegistryKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegSetValueExW(key, name, 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool SetLoginAutoStart(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegistryKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path)) {
            result = ERROR_INSUFFICIENT_BUFFER;
        } else {
            const std::wstring command = L"\"" + std::wstring(path, length) + L"\"";
            result = RegSetValueExW(
                key, kRunValueName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        result = RegDeleteValueW(key, kRunValueName);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

// 清掉产品改名前的用户级痕迹。MSI 的清理动作跑在 SYSTEM 下够不到各用户的 hive，而托盘
// 每个用户登录都会跑一次，覆盖面反而完整。三处都是改名遗留：Run 值 EGoTouchRevTray、
// Software\EGoTouchRev、以及短暂存在过的带空格键名 Software\OpenEGo Hub。现行的
// Software\OpenEGoHub 与 Software\gaokun-hal 不在清理范围。幂等，失败静默。
void MigrateLegacyUserState() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegistryKey, 0, KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(key, L"EGoTouchRevTray");
        RegCloseKey(key);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\EGoTouchRev");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\OpenEGo Hub");
}

void LoadUserSettings() {
    MigrateLegacyUserState();
    g_app.autoStart = ReadUserSetting(L"AutoStart", 1) != 0;
    g_app.oneNoteCompatibility =
        ReadUserSetting(L"OneNoteCompatibility", 1) != 0;
    g_oneNoteCompatibility.store(g_app.oneNoteCompatibility, std::memory_order_release);
    g_app.deviceNotifications = ReadUserSetting(L"DeviceNotifications", 1) != 0;

    // 默认值也立即落到 Run：安装包只负责放置文件，具体用户是否登录后启用由这个非提权
    // 伴随进程维护，避免 per-machine MSI 把启动项写进安装管理员而非实际用户的 HKCU。
    (void)WriteUserSetting(L"AutoStart", g_app.autoStart ? 1u : 0u);
    (void)WriteUserSetting(L"OneNoteCompatibility",
                           g_app.oneNoteCompatibility ? 1u : 0u);
    (void)SetLoginAutoStart(g_app.autoStart);
}

// ── DPI ──────────────────────────────────────────────────────────────────────

// Device-independent pixels -> physical pixels at the panel's current DPI.
int Scaled(int dip) {
    return MulDiv(dip, static_cast<int>(g_app.dpi), kBaseDpi);
}
float ScaledF(float dip) {
    return dip * (static_cast<float>(g_app.dpi) / static_cast<float>(kBaseDpi));
}
// 卡片尺寸按内容算，不写死：产品图取不到时（没装 PCManager，或型号未识别）如果还按含图的
// 尺寸开窗，左半边就是一大片空白。
bool PenImageReady();   // 定义在下面的 EnsurePenImage 附近

int PenImageHeightDip();   // 由裁剪后的真实比例算出，定义在 EnsurePenImage 附近

int CardWidthDip() { return kPadXDip * 2 + kContentWidthDip; }

int CardHeightDip() {
    int h = kPadYDip * 2 + kInfoRowDip;
    if (PenImageReady()) h += PenImageHeightDip() + kPenGapDip;
    return h;
}

int PanelWidth()  { return Scaled(CardWidthDip() + kShadowPadDip * 2); }
int PanelHeight() { return Scaled(CardHeightDip() + kShadowPadDip * 2); }

// ── helpers ──────────────────────────────────────────────────────────────────

std::wstring Widen(const char* utf8) {
    if (!utf8 || !*utf8) return L"";
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (need <= 0) return L"";
    std::wstring out(static_cast<size_t>(need - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), need);
    return out;
}

std::wstring DisplayName() {
    // 服务发布的已经是产品名（见 PenModuleModelId.h 的 ToDisplayName），这里不再拼装。
    // 型号识别不出来时它是空的——退回不带代数的通用名，而不是显示内部代号。
    const std::wstring name = Widen(g_app.state.modelName);
    return name.empty() ? L"HUAWEI M-Pencil" : name;
}

// ── 配色 ─────────────────────────────────────────────────────────────────────

struct Palette {
    D2D1_COLOR_F card;
    D2D1_COLOR_F cardBorder;
    D2D1_COLOR_F shadow;
    D2D1_COLOR_F text;       // 主次层级由 kText*Alpha 调不透明度，不换颜色
    D2D1_COLOR_F accent;
    D2D1_COLOR_F track;      // 没有 PCManager 图标时自绘电量条的底槽
};

bool ReadDarkMode() {
    // 与资源管理器同一个开关：AppsUseLightTheme=0 表示应用使用深色。键不存在时按浅色，
    // 这也是 Windows 自己的默认。
    DWORD value = 1, size = sizeof(value), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, &type, &value, &size) != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

Palette CurrentPalette() {
    if (g_app.darkMode) {
        return Palette{
            D2D1::ColorF(0.11f, 0.12f, 0.14f, 0.97f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f),
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
            // 深色底上 #0A59F7 太暗，提到 HarmonyOS 深色态常用的 #317AF7。这一个值是为对比度
            // 挑的，不是从 PCManager 抄的——它的悬浮窗只有浅色一种。
            D2D1::ColorF(0x317AF7, 1.0f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f),
        };
    }
    return Palette{
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
        D2D1::ColorF(0x0A59F7, 1.0f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f),
    };
}

// ── 电量图标（取自已安装的 PCManager）────────────────────────────────────────
//
// 图标不随程序分发：它们是华为的美术资源。装了 PCManager 就用，没装就退回自绘电量条，
// 两条路都可用，不会因为缺资源而显示空白。

// PCManager 只提供这些档位，取不小于当前电量的最小一档。
// 方向与原厂一致：选件中心是向上取整，15% 用 20 的图标。此处原先向下取整，同一块电量
// 在我们和华为面板上会显示成不同的图标。
constexpr int kBatterySteps[] = {5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 100};

int BatteryStepFor(uint8_t level) {
    for (int s : kBatterySteps) {
        if (level <= s) return s;
    }
    return kBatterySteps[std::size(kBatterySteps) - 1];
}

int MakeIconKey(int step, bool charging, bool dark) {
    return step * 4 + (charging ? 2 : 0) + (dark ? 1 : 0);
}

std::wstring PCManagerRes() {
    wchar_t root[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, root))) {
        return L"";
    }
    return std::wstring(root) + L"\\Huawei\\PCManager\\";
}

std::wstring BatteryIconPath(int step, bool charging) {
    const std::wstring base = PCManagerRes();
    if (base.empty()) return L"";
    std::wstring p = base +
        L"res\\drawable\\iconnect\\commonResources\\discover\\battery\\battery_white";
    p += std::to_wstring(step);
    if (charging) p += L"_charge";
    p += L".png";
    return p;
}

// 选件中心的设备卡片图按模组 ID 的十进制值命名：283_00.png 是 CD54 (0x00011B)、
// 65819_00.png 是 CD54R (0x01011B)、4468738_00.png 是 CD54S (0x443002)。
// 这是散装文件，一定读得到，但分辨率很低（内容仅 216x12），只作兜底。
std::wstring PenCardImagePath(uint32_t modelId) {
    const std::wstring base = PCManagerRes();
    if (base.empty() || modelId == 0) return L"";
    return base + L"components\\accessories_center\\res\\drawable\\cards\\" +
           std::to_wstring(modelId) + L"_00.png";
}

// 高分辨率的笔照片在选件中心插件 DLL 的 WPF 资源流里，散装文件里没有。
std::wstring PenPluginPath(uint32_t modelId) {
    const std::wstring base = PCManagerRes();
    if (base.empty()) return L"";
    const wchar_t* dll = L"CD54RPenApp.dll";
    switch (modelId) {
    case 0x00011Au: dll = L"CD52PenApp.dll";  break;
    case 0x00011Bu: dll = L"CD54PenApp.dll";  break;
    case 0x01011Bu: dll = L"CD54RPenApp.dll"; break;
    case 0x443002u: dll = L"CD54SPenApp.dll"; break;
    default: break;   // 各插件都带着同一套共享图，取不到型号时随便挑一个即可
    }
    return base + L"components\\accessories_center\\accessories_app\\AccessoryApp\\Lib\\Plugins\\" +
           dll;
}

// 横向的笔照。资源名里的编号是 modelId 左移 8 位的 8 位小写十六进制：pic_00011a00 对应
// 0x00011A (CD52)、pic_00011b00 对应 0x00011B (CD54)、pic_00003100 对应 0x000031（cards 目录
// 下正好有个 49_00.png，49 = 0x31）——两套命名互相印证。
std::wstring PenConnectResourceName(uint32_t modelId) {
    wchar_t buf[64]{};
    std::swprintf(buf, 64, L"resources/pic_%08x_connect@1.5x.png", modelId << 8);
    return buf;
}

// 描边是纯灰阶（R=G=B），电量填充是绿/红。只反转灰阶像素就得到深色底上可用的白色描边版，
// 彩色填充原样保留——因此不需要第二套资源。
void InvertGrayscaleStrokes(std::vector<BYTE>& bgra) {
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const BYTE a = bgra[i + 3];
        if (a == 0) continue;
        // 像素是预乘的，先还原再判断，否则半透明的黑会被当成彩色。
        const int b = bgra[i] * 255 / a;
        const int g = bgra[i + 1] * 255 / a;
        const int r = bgra[i + 2] * 255 / a;
        if (r != g || g != b) continue;
        const int inv = 255 - r;
        bgra[i]     = BYTE(inv * a / 255);
        bgra[i + 1] = BYTE(inv * a / 255);
        bgra[i + 2] = BYTE(inv * a / 255);
    }
}

// 按 alpha 裁掉四周的透明边。选件中心的产品图是 240x128 的画布，笔本身只占 216x12——不裁
// 就等于把大片空气一起缩放，笔会细得像根头发。
void CropToContent(std::vector<BYTE>& bgra, UINT& w, UINT& h) {
    if (w == 0 || h == 0) return;
    UINT x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            if (bgra[(size_t(y) * w + x) * 4 + 3] == 0) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x0 > x1 || y0 > y1) return;   // 整张全透明，保持原样

    const UINT nw = x1 - x0 + 1, nh = y1 - y0 + 1;
    if (nw == w && nh == h) return;

    std::vector<BYTE> out(size_t(nw) * nh * 4);
    for (UINT y = 0; y < nh; ++y) {
        std::memcpy(&out[size_t(y) * nw * 4],
                    &bgra[(size_t(y + y0) * w + x0) * 4],
                    size_t(nw) * 4);
    }
    bgra = std::move(out);
    w = nw;
    h = nh;
}

bool LoadPng(const std::wstring& path, App::ImageCache& cache, int key, bool invertGray,
             bool cropToContent = false) {
    if (cache.unavailable) return false;
    if (cache.key == key && !cache.pixels.empty()) return true;
    if (!g_app.wicFactory || path.empty()) return false;

    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(g_app.wicFactory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) {
        // 一个文件缺失就当整类不可用：混着用会让面板在状态变化时忽然换风格，而且每帧重试
        // 一个不存在的路径没有意义。
        cache.unavailable = true;
        return false;
    }

    bool ok = false;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(g_app.wicFactory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        if (SUCCEEDED(conv->GetSize(&w, &h)) && w && h) {
            std::vector<BYTE> pixels(size_t(w) * h * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data()))) {
                if (invertGray) InvertGrayscaleStrokes(pixels);
                if (cropToContent) CropToContent(pixels, w, h);
                cache.pixels = std::move(pixels);
                cache.width = w;
                cache.height = h;
                cache.key = key;
                ok = true;
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    decoder->Release();
    return ok;
}

bool EnsureBatteryIcon(int step, bool charging, bool dark) {
    return LoadPng(BatteryIconPath(step, charging), g_app.battery,
                   MakeIconKey(step, charging, dark), dark);
}

// 解码一段内存里的 PNG（来自托管资源），与 LoadPng 走同一套后处理。
bool LoadPngBytes(const std::vector<uint8_t>& png, App::ImageCache& cache, int key) {
    if (png.empty() || !g_app.wicFactory) return false;

    IWICStream* stream = nullptr;
    if (FAILED(g_app.wicFactory->CreateStream(&stream)) || !stream) return false;
    bool ok = false;
    if (SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()),
                                               static_cast<DWORD>(png.size())))) {
        IWICBitmapDecoder* decoder = nullptr;
        if (SUCCEEDED(g_app.wicFactory->CreateDecoderFromStream(
                stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
            IWICBitmapFrameDecode* frame = nullptr;
            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(decoder->GetFrame(0, &frame)) &&
                SUCCEEDED(g_app.wicFactory->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                           WICBitmapPaletteTypeCustom))) {
                UINT w = 0, h = 0;
                if (SUCCEEDED(conv->GetSize(&w, &h)) && w && h) {
                    std::vector<BYTE> pixels(size_t(w) * h * 4);
                    if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(pixels.size()),
                                                   pixels.data()))) {
                        CropToContent(pixels, w, h);
                        cache.pixels = std::move(pixels);
                        cache.width = w;
                        cache.height = h;
                        cache.key = key;
                        ok = true;
                    }
                }
            }
            if (conv) conv->Release();
            if (frame) frame->Release();
            decoder->Release();
        }
    }
    stream->Release();
    return ok;
}

bool EnsurePenImage(uint32_t modelId) {
    if (g_app.pen.key == int(modelId)) {
        // 这个型号试过了：要么有像素可用，要么已知取不到，两种情况都不必再试。
        return !g_app.pen.pixels.empty();
    }
    // 窗口在 InitGraphics 之前就要算尺寸，那时 WIC 还没建好。这种「还没准备好」不能记成
    // 「取不到」，否则整条路会被第一次必然失败的调用永久禁用。
    if (!g_app.wicFactory || modelId == 0) return false;

    // 高分辨率的横向笔照只在插件 DLL 的托管资源里（内容 436x26），散装的卡片图仅 216x12。
    // 颜色未必对得上——CD54R 没有自己的横向图，退到 CD54 的银色那张——但形状是对的，这一点
    // 机主确认可以接受。
    const std::wstring plugin = PenPluginPath(modelId);
    if (!plugin.empty()) {
        const std::wstring names[] = {
            PenConnectResourceName(modelId),
            PenConnectResourceName(0x00011Bu),            // CD54，同为二代的实物渲染
            L"resources/pic_vector_connect@1.5x.png",     // 每个插件都带着的通用矢量图
        };
        for (const auto& n : names) {
            std::vector<uint8_t> png;
            if (ManagedRes::ReadEmbeddedResource(plugin, n, png) &&
                LoadPngBytes(png, g_app.pen, int(modelId))) {
                return true;
            }
        }
    }

    // 兜底：散装的卡片图，分辨率低但一定读得到。
    if (LoadPng(PenCardImagePath(modelId), g_app.pen, int(modelId), false, true)) {
        return true;
    }
    g_app.pen.key = int(modelId);   // 记下这个型号已经试过，别每帧重复读 DLL
    g_app.pen.pixels.clear();
    return false;
}

bool PenImageReady() { return EnsurePenImage(g_app.state.modelId); }

// 通栏宽度固定，高度跟着裁剪后的真实比例走，不预设——不同型号的图裁出来比例未必一样。
int PenImageHeightDip() {
    if (!PenImageReady() || g_app.pen.width == 0) return 0;
    return int(float(kContentWidthDip) * float(g_app.pen.height) / float(g_app.pen.width) + 0.5f);
}

// 把缓存的像素画到当前渲染目标。位图不能跨帧缓存（绑在渲染目标上），所以每次现建。
void DrawImage(ID2D1RenderTarget* rt, const App::ImageCache& img, const D2D1_RECT_F& dst) {
    if (img.pixels.empty()) return;
    ID2D1Bitmap* bmp = nullptr;
    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(img.width, img.height), img.pixels.data(),
                                   img.width * 4, &bp, &bmp)) && bmp) {
        rt->DrawBitmap(bmp, dst);
        bmp->Release();
    }
}

// 量一段文字的宽度，用于把「图标 + 百分比」当作一个整体居中。
float MeasureText(const wchar_t* text, IDWriteTextFormat* fmt) {
    if (!text || !*text || !fmt || !g_app.dwriteFactory) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(g_app.dwriteFactory->CreateTextLayout(text, UINT32(wcslen(text)), fmt,
                                                     4096.0f, 4096.0f, &layout)) || !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m{};
    const float w = SUCCEEDED(layout->GetMetrics(&m)) ? m.widthIncludingTrailingWhitespace : 0.0f;
    layout->Release();
    return w;
}

std::wstring StatusLine() {
    if (!g_app.hasState) return L"未连接";
    if (g_app.state.hasChargingState && g_app.state.charging) return L"正在充电";
    if (g_app.state.hasStylusLink && g_app.state.stylusLinked) return L"已连接";
    if (g_app.state.hasDeviceAttached && !g_app.state.deviceAttached) return L"未连接";
    return L"待机";
}

bool CreateTextFormats();

// ── rendering ────────────────────────────────────────────────────────────────

// Draws into a 32bpp premultiplied DIB and hands it to UpdateLayeredWindow. The panel is
// static for its whole lifetime, so this runs once per state change rather than per frame
// — the fade is done by changing the layer alpha, not by redrawing.
void RenderPanel() {
    if (!g_app.d2dFactory) return;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    const int pw = PanelWidth();
    const int ph = PanelHeight();
    bmi.bmiHeader.biWidth = pw;
    bmi.bmiHeader.biHeight = -ph;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(memDc, dib);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    bool drew = false;
    ID2D1DCRenderTarget* rt = nullptr;
    if (SUCCEEDED(g_app.d2dFactory->CreateDCRenderTarget(&props, &rt))) {
        RECT bindRect{0, 0, pw, ph};
        // 画笔在 BeginDraw 之前建。D2D 允许在绘制块外创建资源，而整帧的每一次上色都走这
        // 一支画笔——建不出来就放弃这一帧，让上一帧的内容留在窗口上，不能拿着空指针接着画。
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(rt->BindDC(memDc, &bindRect)) &&
            SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &brush)) &&
            brush) {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0.0f));

            const Palette pal = CurrentPalette();

            const float pad = ScaledF(float(kShadowPadDip));
            const float radius = ScaledF(kCardRadiusDip);
            const D2D1_RECT_F cardRect =
                D2D1::RectF(pad, pad, float(pw) - pad, float(ph) - pad);

            // 投影。DCRenderTarget 是 D2D 1.0 的接口，拿不到 DropShadowEffect（那需要
            // ID2D1DeviceContext），所以用几圈递减透明度的圆角矩形近似。层数少、半径小，
            // 视觉上与 BlurRadius=5 / ShadowDepth=0 接近，代价可忽略。
            constexpr int kShadowLayers = 6;
            for (int i = kShadowLayers; i >= 1; --i) {
                const float grow = ScaledF(float(i));
                const float t = float(i) / float(kShadowLayers);
                D2D1_COLOR_F c = pal.shadow;
                c.a = pal.shadow.a * (1.0f - t) * 0.5f;
                brush->SetColor(c);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cardRect.left - grow, cardRect.top - grow,
                                                  cardRect.right + grow, cardRect.bottom + grow),
                                      radius + grow, radius + grow),
                    brush);
            }

            // 卡片
            D2D1_ROUNDED_RECT card = D2D1::RoundedRect(cardRect, radius, radius);
            brush->SetColor(pal.card);
            rt->FillRoundedRectangle(card, brush);
            brush->SetColor(pal.cardBorder);
            rt->DrawRoundedRectangle(card, brush, ScaledF(1.0f));

            auto textBrush = [&](float alpha) {
                D2D1_COLOR_F c = pal.text;
                c.a = alpha;
                brush->SetColor(c);
            };

            // 笔照通栏压顶，下面一行左名称、右电量。
            const float padX = cardRect.left + ScaledF(float(kPadXDip));
            const float padR = cardRect.right - ScaledF(float(kPadXDip));

            float y = cardRect.top + ScaledF(float(kPadYDip));
            if (PenImageReady()) {
                const float ih = ScaledF(float(PenImageHeightDip()));
                DrawImage(rt, g_app.pen, D2D1::RectF(padX, y, padR, y + ih));
                y += ih + ScaledF(float(kPenGapDip));
            }

            const float rowH = ScaledF(float(kInfoRowDip));
            const bool charging = g_app.state.hasChargingState && g_app.state.charging;
            const bool hasBattery = g_app.state.hasBatteryLevel;

            wchar_t pctText[16]{};
            std::swprintf(pctText, 16, L"%u", unsigned(g_app.state.batteryLevel));

            const bool haveIcon =
                hasBattery &&
                EnsureBatteryIcon(BatteryStepFor(g_app.state.batteryLevel), charging, g_app.darkMode);

            const float gap = ScaledF(8.0f);
            // 只缩不放：图标原图 54x26，按行高拉伸会糊。
            const float iconH = ScaledF(float(kBatteryIconHeightDip));
            const float iconW = haveIcon
                              ? iconH * (float(g_app.battery.width) / float(g_app.battery.height))
                              : ScaledF(48.0f);   // 自绘电量条的宽度

            // 整组（图标 + 数字 + 百分号）右对齐到内容区右端，先量总宽再定起点。
            const float numW = MeasureText(pctText, g_app.fmtPercent);
            const float signW = MeasureText(L"%", g_app.fmtPercentSign) + ScaledF(1.0f);
            const float groupW = hasBattery ? (iconW + gap + numW + signW) : 0.0f;
            float x = padR - groupW;

            // 名称占左侧剩余空间，超长省略——名字比电量更容易变长。
            textBrush(kTextPrimaryAlpha);
            const std::wstring title = DisplayName();
            rt->DrawText(title.c_str(), UINT32(title.size()), g_app.fmtTitle,
                          D2D1::RectF(padX, y, hasBattery ? x - gap : padR, y + rowH), brush);

            if (!hasBattery) {
                // 没有电量可显示时，右侧才退回状态词——有电量时那句话已经被图标说过了。
                textBrush(kTextSecondaryAlpha);
                const std::wstring status = StatusLine();
                rt->DrawText(status.c_str(), UINT32(status.size()), g_app.fmtBody,
                              D2D1::RectF(padX, y, padR, y + rowH), brush);
            } else if (haveIcon) {
                const float top = y + (rowH - iconH) * 0.5f;
                DrawImage(rt, g_app.battery, D2D1::RectF(x, top, x + iconW, top + iconH));
            } else {
                // 没有 PCManager 资源时自绘：一条圆角进度条，语义与图标一致。
                const float barH = ScaledF(10.0f);
                const float barTop = y + (rowH - barH) * 0.5f;
                brush->SetColor(pal.track);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(x, barTop, x + iconW, barTop + barH),
                                      barH * 0.5f, barH * 0.5f), brush);
                const float pct = std::clamp(float(g_app.state.batteryLevel) / 100.0f, 0.0f, 1.0f);
                const float fillRight = x + iconW * pct;
                if (fillRight > x + barH * 0.5f) {
                    // 低电量即便在充电也显示为红：那才是用户最需要注意到的状态。
                    brush->SetColor(g_app.state.batteryLevel <= 20
                                        ? D2D1::ColorF(0xFF3320, 1.0f)
                                        : D2D1::ColorF(0x8AD63F, 1.0f));
                    rt->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(x, barTop, fillRight, barTop + barH),
                                          barH * 0.5f, barH * 0.5f), brush);
                }
            }

            if (hasBattery) {
                // 数字 18px 加粗、百分号 10px 底对齐——原厂 BatteryValueStyle 与
                // BatteryPercentStyle 是分开画的两个元素，整体一个字号会失掉这个层次。
                // 充电时用强调色。
                x += iconW + gap;
                if (charging) {
                    brush->SetColor(pal.accent);
                } else {
                    textBrush(kTextPrimaryAlpha);
                }
                rt->DrawText(pctText, UINT32(wcslen(pctText)), g_app.fmtPercent,
                              D2D1::RectF(x, y, padR, y + rowH), brush);
                rt->DrawText(L"%", 1, g_app.fmtPercentSign,
                              D2D1::RectF(x + numW + ScaledF(1.0f), y, padR,
                                          y + rowH - ScaledF(3.0f)),
                              brush);
            }

            brush->Release();
            rt->EndDraw();
            drew = true;
        }
        rt->Release();
    }

    if (drew) {
        POINT srcPos{0, 0};
        SIZE size{pw, ph};
        RECT wr{};
        GetWindowRect(g_app.hwnd, &wr);
        POINT dstPos{wr.left, wr.top};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, g_app.alpha, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_app.hwnd, screenDc, &dstPos, &size,
                            memDc, &srcPos, 0, &blend, ULW_ALPHA);
    }

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

void ApplyAlphaOnly() {
    BLENDFUNCTION blend{AC_SRC_OVER, 0, g_app.alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_app.hwnd, nullptr, nullptr, nullptr, nullptr, nullptr,
                        0, &blend, ULW_ALPHA);
}

void PositionPanel() {
    // Anchor to the monitor holding the cursor, and adopt that monitor's DPI before
    // computing any size. SPI_GETWORKAREA only describes the primary display, which would
    // misplace the panel on a multi-monitor setup.
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);

    UINT dpiX = kBaseDpi, dpiY = kBaseDpi;
    if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) &&
        dpiX != g_app.dpi) {
        g_app.dpi = dpiX;
        CreateTextFormats();
    }

    // 锚在整块屏幕上而不是工作区：工作区会随任务栏的位置和自动隐藏状态伸缩，面板就会跟着跳。
    MONITORINFO mi{sizeof(mi)};
    RECT screen{};
    if (GetMonitorInfoW(mon, &mi)) {
        screen = mi.rcMonitor;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
    }

    const int w = PanelWidth();
    const int h = PanelHeight();
    const int anchorX = screen.left + int((screen.right - screen.left) * kAnchorXRatio);
    const int anchorY = screen.top + int((screen.bottom - screen.top) * kAnchorYRatio);
    const int x = anchorX - w / 2;
    const int y = anchorY - h / 2;
    SetWindowPos(g_app.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_NOREDRAW);
}

void BeginShow() {
    PositionPanel();
    g_app.phase = Phase::FadeIn;
    g_app.phaseStartTick = GetTickCount();
    g_app.alpha = 0;
    RenderPanel();
    // SW_SHOWNOACTIVATE matters: stealing focus mid-stroke would be worse than not
    // showing the panel at all.
    ShowWindow(g_app.hwnd, SW_SHOWNOACTIVATE);
    SetTimer(g_app.hwnd, kTimerAnim, kAnimIntervalMs, nullptr);
}

void BeginHide() {
    g_app.phase = Phase::FadeOut;
    g_app.phaseStartTick = GetTickCount();
    SetTimer(g_app.hwnd, kTimerAnim, kAnimIntervalMs, nullptr);
}

void FinishHide() {
    g_app.phase = Phase::Hidden;
    KillTimer(g_app.hwnd, kTimerAnim);
    ShowWindow(g_app.hwnd, SW_HIDE);
}

// ── 侧键双击 ─────────────────────────────────────────────────────────────────

std::atomic<bool> g_gestureStop{false};

using Microsoft::WRL::ComPtr;

ComPtr<IUIAutomationCondition> MakeStringCondition(
        IUIAutomation* automation, PROPERTYID property, const wchar_t* text) {
    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(text);
    if (!value.bstrVal) return {};

    ComPtr<IUIAutomationCondition> condition;
    automation->CreatePropertyCondition(property, value, &condition);
    VariantClear(&value);
    return condition;
}

std::wstring ElementName(IUIAutomationElement* element) {
    BSTR raw = nullptr;
    if (!element || FAILED(element->get_CurrentName(&raw)) || !raw) return {};
    std::wstring name(raw, SysStringLen(raw));
    SysFreeString(raw);
    return name;
}

bool IsOneNoteForeground(HWND& rootWindow) {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    rootWindow = GetAncestor(foreground, GA_ROOT);
    if (!rootWindow) rootWindow = foreground;

    DWORD processId = 0;
    GetWindowThreadProcessId(rootWindow, &processId);
    if (!processId) return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;

    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!queried) return false;

    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"ONENOTE.EXE") == 0;
}

bool IsEraserToolName(const std::wstring& name) {
    return name == L"笔划橡皮擦" || name == L"Stroke Eraser" || name == L"Eraser";
}

bool IsPenToolName(const std::wstring& name) {
    return name.rfind(L"笔:", 0) == 0 || name.rfind(L"Pen:", 0) == 0;
}

ComPtr<IUIAutomationElementArray> FindToolboxItems(
        IUIAutomation* automation, IUIAutomationElement* root) {
    auto classCondition = MakeStringCondition(
        automation, UIA_ClassNamePropertyId, L"NetUIToolboxButtonAnchor");
    ComPtr<IUIAutomationElementArray> items;
    if (classCondition) {
        root->FindAll(TreeScope_Descendants, classCondition.Get(), &items);
    }
    return items;
}

bool IsSelected(IUIAutomationElement* element) {
    ComPtr<IUIAutomationSelectionItemPattern> selection;
    if (!element || FAILED(element->GetCurrentPatternAs(
            UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection)))) {
        return false;
    }
    BOOL selected = FALSE;
    return SUCCEEDED(selection->get_CurrentIsSelected(&selected)) && selected;
}

bool InvokeTool(IUIAutomationElement* element) {
    ComPtr<IUIAutomationInvokePattern> invoke;
    return element && SUCCEEDED(element->GetCurrentPatternAs(
               UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) &&
           SUCCEEDED(invoke->Invoke());
}

struct ToolboxScan {
    ComPtr<IUIAutomationElement> eraser;
    ComPtr<IUIAutomationElement> rememberedPen;
    std::wstring selectedPenName;
    bool eraserSelected = false;
};

ToolboxScan ScanToolbox(
        IUIAutomation* automation,
        IUIAutomationElement* root,
        const std::wstring& rememberedName) {
    ToolboxScan result;
    auto items = FindToolboxItems(automation, root);
    if (!items) return result;
    int count = 0;
    items->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> item;
        if (FAILED(items->GetElement(i, &item)) || !item) continue;
        const std::wstring name = ElementName(item.Get());
        if (IsEraserToolName(name)) {
            if (!result.eraser) result.eraser = item;
            if (IsSelected(item.Get())) result.eraserSelected = true;
            continue;
        }
        const bool isPen = IsPenToolName(name);
        if (isPen && !rememberedName.empty() && name == rememberedName &&
            !result.rememberedPen) {
            result.rememberedPen = item;
        }
        if (isPen && result.selectedPenName.empty() && IsSelected(item.Get())) {
            result.selectedPenName = name;
        }
    }
    return result;
}

bool SelectRibbonTab(IUIAutomationElement* tab) {
    ComPtr<IUIAutomationSelectionItemPattern> selection;
    return tab && SUCCEEDED(tab->GetCurrentPatternAs(
               UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection))) &&
           SUCCEEDED(selection->Select());
}

// Office 桌面版 OneNote 不读 penFlags——它连 GetPointerPenInfo 都不导入——但它把绘图工具
// 作为可调用的 Ribbon 元素暴露出来。这里只在 OneNote 已经是前台时触碰它，不激活窗口、
// 不改笔记，也不影响 Journal 等遵循 Windows Ink 状态机的应用。
//
// 「不读 penFlags」不等于「不认橡皮」：它经 RealTimeStylus 读 StylusInfo.bIsInvertedCursor,
// 而那个值同样来自 HID 的 Invert(0x3C)。所以真让厂商 VHF 把那一位置起来，这整套 UIA 就
// 可以删掉，连同 ScopedOneNoteInputSuppression 那套握手。做不到的原因不在 OneNote，
// 见 hal/docs/thp-eraser.md 与 docs/onenote_ink_eraser.md。
//
// TODO 这是权宜之计，代价是绑死了 OneNote 的界面细节：靠 AutomationId "TabInk" 找页签，
// 靠工具名匹配橡皮擦（见 IsEraserToolName 里那张硬编码的语言表），Office 改版或换一种
// 界面语言就会失效，而失效是静默的——Invoke 不到就当作没同步。同步期间还得把笔输入闸掉，
// 否则 UIA 的点击会和正在书写的笔互相干扰，这就是 ScopedOneNoteInputSuppression 的由来。
// 值得研究的替代路径是 OneNote 自己的对象模型，不经界面直接切工具；没查过它是否暴露了
// 绘图工具，也没查过桌面版与 UWP 版的差异。
struct OneNoteToolToggleState {
    std::wstring penName;
    HWND window = nullptr;
    bool armed = false;

    void Disarm() {
        penName.clear();
        window = nullptr;
        armed = false;
    }
};

bool CanSyncForegroundOneNoteTool(
        IUIAutomation* automation,
        HWND oneNoteWindow,
        bool eraserActive,
        const OneNoteToolToggleState& state) {
    if (!automation || !oneNoteWindow) return false;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(oneNoteWindow, &root)) || !root) return false;

    const ToolboxScan tools = ScanToolbox(automation, root.Get(), state.penName);
    if (eraserActive) return !tools.selectedPenName.empty() && tools.eraser;
    return state.armed && state.window == oneNoteWindow && tools.eraserSelected &&
           tools.rememberedPen;
}

bool SyncForegroundOneNoteTool(
        IUIAutomation* automation,
        HWND oneNoteWindow,
        bool eraserActive,
        OneNoteToolToggleState& state) {
    if (!automation || !oneNoteWindow) return false;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(oneNoteWindow, &root)) || !root) return false;

    auto tryInvoke = [&] {
        ToolboxScan tools = ScanToolbox(automation, root.Get(), state.penName);
        if (eraserActive) {
            // 新一轮只能从一支明确选中的笔开始。选择、套索、荧光笔、手动选中的橡皮或
            // Office 新增的未知工具都不能成为“切回”目标。
            state.Disarm();
            if (tools.selectedPenName.empty() || !InvokeTool(tools.eraser.Get())) return false;
            state.penName = std::move(tools.selectedPenName);
            state.window = oneNoteWindow;
            state.armed = true;
            return true;
        }

        // 只恢复本轮由我们切走的那支笔，而且前台仍必须停在我们选中的橡皮上。用户若已
        // 手动换成另一支笔或别的工具，就接受用户选择，绝不能再用旧记忆覆盖它。
        if (!state.armed || state.window != oneNoteWindow ||
            !tools.eraserSelected || !tools.rememberedPen) {
            state.Disarm();
            return false;
        }
        auto pen = tools.rememberedPen;
        state.Disarm();
        return InvokeTool(pen.Get());
    };

    // 最常见路径：用户正在 OneNote 的 Draw 页签里书写，工具节点已经 materialize。一次
    // FindAll 同时找到橡皮擦、当前笔和回退笔，直接 Invoke，不再先选一遍 TabInk。
    if (tryInvoke()) return true;

    auto drawCondition = MakeStringCondition(
        automation, UIA_AutomationIdPropertyId, L"TabInk");
    ComPtr<IUIAutomationElement> drawTab;
    if (!drawCondition || FAILED(root->FindFirst(
            TreeScope_Descendants, drawCondition.Get(), &drawTab)) ||
        !SelectRibbonTab(drawTab.Get())) {
        return false;
    }

    // 只有工具库尚未构建时才走慢路径。前几次密集探测，整体上限约 200 ms；旧实现每次
    // 先切页签并以 50 ms 间隔等满 600 ms。切换后保持 Draw 页签，避免恢复页签再做一轮 UIA。
    constexpr std::array<DWORD, 9> kRetryDelaysMs{0, 10, 10, 15, 20, 25, 30, 40, 50};
    for (const DWORD delay : kRetryDelaysMs) {
        if (delay) Sleep(delay);
        if (tryInvoke()) return true;
    }
    return false;
}

bool WaitForInputSuppression(
        PenStatus::Reader& reader, bool expected, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        PenStatus::State state{};
        if (reader.Read(state) && state.hasInputSuppressed &&
            state.inputSuppressed == expected) {
            return true;
        }
        Sleep(2);
    } while (GetTickCount64() < deadline);
    return false;
}

class ScopedOneNoteInputSuppression {
public:
    explicit ScopedOneNoteInputSuppression(PenStatus::Reader& reader)
        : m_reader(reader), m_submitLock(g_controlSubmitMutex) {}

    bool Begin() {
        m_client.Close();
        if (!m_client.Open() || !m_client.SubmitInputSuppression(
                PenControl::InputSuppressionCommand::BeginOrRenew)) {
            return false;
        }
        m_active = WaitForInputSuppression(m_reader, true, 150);
        if (!m_active) {
            (void)m_client.SubmitInputSuppression(
                PenControl::InputSuppressionCommand::End);
        } else {
            // Begin 已被服务状态确认，后续 UIA 期间允许每秒一次的 provider heartbeat 继续
            // 提交。End 会重新拿锁并等确认，因此不会被紧随其后的 heartbeat 覆盖。
            m_submitLock.unlock();
        }
        return m_active;
    }

    ~ScopedOneNoteInputSuppression() {
        if (!m_active) return;
        m_submitLock.lock();
        if (!m_client.SubmitInputSuppression(
                PenControl::InputSuppressionCommand::End)) {
            return;  // 服务端 1 秒 watchdog 仍会解锁
        }
        (void)WaitForInputSuppression(m_reader, false, 150);
    }

private:
    PenStatus::Reader& m_reader;
    std::unique_lock<std::mutex> m_submitLock;
    PenControl::Client m_client;
    bool m_active = false;
};

// 用自己的 Reader，不共用 g_app.reader：面板轮询在读失败时会 Close()，而这个线程正阻塞
// 在同一个句柄上等待。两者生命周期无关，各持一份最省事。
void GestureWatcherThread() {
    PenStatus::Reader reader;
    OneNoteToolToggleState oneNoteToggle;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IUIAutomation> automation;
    if (SUCCEEDED(comResult)) {
        (void)CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&automation));
    }
    // 有限超时而不是 INFINITE：退出时才能看到停止标志。1 秒的空转在一个本来就每几百毫秒
    // 刷新面板的进程里可以忽略。
    constexpr DWORD kWaitMs = 1000;
    unsigned waitTimeouts = 0;

    while (!g_gestureStop.load(std::memory_order_relaxed)) {
        if (!reader.IsOpen()) {
            // 服务可能比托盘起得晚，或中途重启过。
            if (!reader.Open()) {
                Sleep(kWaitMs);
                continue;
            }
        }
        if (reader.WaitForDoubleClick(kWaitMs)) {
            waitTimeouts = 0;
            PenStatus::State state{};
            if (!reader.Read(state) || !state.hasPenButtonMode) continue;

            const auto mode = PenButtonModeFromNumeric(state.penButtonMode);
            if (mode == PenButtonMode::WindowsInk) {
                PenInk::InjectDoubleClickShortcut();
            } else if (mode == PenButtonMode::ToggleEraser && state.hasEraserActive) {
                // Journal 等标准 Ink 应用直接消费厂商 VHF 的 Invert/Eraser 状态。桌面 OneNote
                // 使用自己的工具状态，只有它仍需要下面的 UIA 兼容路径。
                HWND oneNoteWindow = nullptr;
                if (SUCCEEDED(comResult) &&
                    g_oneNoteCompatibility.load(std::memory_order_acquire) &&
                    automation && IsOneNoteForeground(oneNoteWindow)) {
                    if (CanSyncForegroundOneNoteTool(
                            automation.Get(), oneNoteWindow,
                            state.eraserActive, oneNoteToggle)) {
                        ScopedOneNoteInputSuppression suppression(reader);
                        if (!suppression.Begin() || !SyncForegroundOneNoteTool(
                                automation.Get(), oneNoteWindow,
                                state.eraserActive, oneNoteToggle)) {
                            oneNoteToggle.Disarm();
                        }
                    } else {
                        oneNoteToggle.Disarm();
                    }
                } else {
                    oneNoteToggle.Disarm();
                }
            }
        } else if (++waitTimeouts >= 5) {
            // Writer 重启时旧 mapping/event 会被本 Reader 的句柄续命，等待不会报错却也永远
            // 收不到新服务的边沿。周期性重开可重新解析到当前命名对象。
            reader.Close();
            waitTimeouts = 0;
        }
    }

    if (SUCCEEDED(comResult)) CoUninitialize();
}

// ── channel ──────────────────────────────────────────────────────────────────

void ShowWinUiNotification(EGoTouchTrayIpc::Notification notification, LPARAM payload = 0);
// 失败提示走托盘自己的气泡，不经 WinUI 那条通知链：那条链只有设置窗在跑时才有宿主，而
// 「设置没生效」正是用户最可能只在托盘菜单里操作的一刻。
void ShowTrayBalloon(const wchar_t* title, const wchar_t* text);

// 键盘开关没落地时的说明。分成两条是因为用户能做的事不同。
void ReportKbdDetachFailure(bool unsupported) {
    g_app.kbdDetachPending = false;
    ShowTrayBalloon(L"键盘设置未生效",
                    unsupported ? L"当前键盘固件不支持分离后保持无线连接。"
                                : L"键盘组件无响应，设置未能写入。");
}

void PollChannel() {
    const ULONGLONG now = GetTickCount64();
    if (g_app.reader.IsOpen() &&
        now - g_app.lastReaderReconnectTick >= kReaderReconnectMs) {
        g_app.reader.Close();
    }
    if (!g_app.reader.IsOpen()) {
        // The service may start after us, or restart; keep retrying quietly.
        if (!g_app.reader.Open()) {
            g_app.hasState = false;
            return;
        }
        g_app.lastReaderReconnectTick = now;
    }

    PenStatus::State fresh{};
    if (!g_app.reader.Read(fresh)) {
        g_app.reader.Close();
        return;
    }

    const bool wasCharging = g_app.prevCharging;
    const bool hadBaseline = g_app.prevChargingValid;
    const uint32_t previousNotification = g_app.prevNotificationSequence;
    const bool hadNotificationBaseline = g_app.prevNotificationValid;

    g_app.state = fresh;
    g_app.hasState = true;

    // 「没上报充电状态」和「上报了未充电」对这个边沿来说是同一回事：都不是在充电。分开处理
    // 会让基线在笔从未上报过充电状态时建立不起来，于是第一次吸附被当作基线消耗掉，要到第二
    // 次才弹窗——这正是之前的症状。
    const bool nowCharging = fresh.hasChargingState && fresh.charging;

    const bool notificationChanged = hadNotificationBaseline &&
        fresh.notificationSequence != 0 &&
        fresh.notificationSequence != previousNotification;

    // UI 已迁到 WinUI 进程。服务提供显式通知边沿时优先按 0x28/0x2C/0x2A 的语义显示；旧服务
    // 没有新 ABI 时共享通道会重连失败，因此这里仍保留充电边沿作为同版本内的容错路径。
    if (notificationChanged) {
        if (fresh.notificationKind == PenStatus::NotificationKind::PenDeviation) {
            if (now - g_app.lastDeviationNotificationTick >= 3000) {
                ShowWinUiNotification(EGoTouchTrayIpc::Notification::PenDeviation);
                g_app.lastDeviationNotificationTick = now;
            }
        } else if (fresh.notificationKind == PenStatus::NotificationKind::PenConnected) {
            if (now - g_app.lastConnectionNotificationTick >= 1000) {
                ShowWinUiNotification(EGoTouchTrayIpc::Notification::PenConnected);
                g_app.lastConnectionNotificationTick = now;
            }
        } else if (fresh.notificationKind == PenStatus::NotificationKind::KeyboardConnected) {
            if (now - g_app.lastKeyboardConnectionNotificationTick >= 1000) {
                ShowWinUiNotification(EGoTouchTrayIpc::Notification::KeyboardConnected);
                g_app.lastKeyboardConnectionNotificationTick = now;
            }
        } else if (fresh.notificationKind ==
                       PenStatus::NotificationKind::KbdDetachSupportFailed ||
                   fresh.notificationKind ==
                       PenStatus::NotificationKind::KbdDetachSupportUnsupported) {
            ReportKbdDetachFailure(fresh.notificationKind ==
                                   PenStatus::NotificationKind::KbdDetachSupportUnsupported);
        }
    } else if (hadBaseline && !wasCharging && nowCharging) {
        if (now - g_app.lastConnectionNotificationTick >= 1000) {
            ShowWinUiNotification(EGoTouchTrayIpc::Notification::PenConnected);
            g_app.lastConnectionNotificationTick = now;
        }
    }

    // 工具切换在状态里是一个可重复读的位，不是边沿，所以这里自己求边沿。放在轮询里而不是
    // 手势线程里：笔身自带的切换不发双击事件，只有这一位会动，两种来源在这里合成一条提示。
    if (fresh.hasEraserActive) {
        const bool eraserNow = fresh.eraserActive;
        if (g_app.prevEraserValid && eraserNow != g_app.prevEraserActive) {
            ShowWinUiNotification(EGoTouchTrayIpc::Notification::PenToolChanged, eraserNow);
        }
        g_app.prevEraserActive = eraserNow;
        g_app.prevEraserValid = true;
    } else {
        // 服务不再发布这一位（换了侧键模式，或笔断开）。基线作废，恢复发布时的第一份快照
        // 只用来重建基线——否则切回 ToggleEraser 就会凭空弹一次提示。
        g_app.prevEraserValid = false;
    }

    // 在途的键盘开关：等快照翻转到用户选的那个值才算落地。超时才回弹，回弹时说明原因——
    // 悄悄弹回去正是这个开关此前看起来「点了没反应」的样子。
    if (g_app.kbdDetachPending) {
        if (fresh.hasKbdDetachSupport &&
            fresh.kbdDetachSupport == g_app.kbdDetachPendingValue) {
            g_app.kbdDetachPending = false;
        } else if (now >= g_app.kbdDetachPendingDeadline) {
            ReportKbdDetachFailure(false);
        }
    }

    g_app.prevCharging = nowCharging;
    g_app.prevChargingValid = true;
    g_app.prevNotificationSequence = fresh.notificationSequence;
    g_app.prevNotificationValid = true;
}

// ── tray icon ────────────────────────────────────────────────────────────────

// 任务栏的深浅由 SystemUsesLightTheme 决定，与 ReadDarkMode() 读的 AppsUseLightTheme 是两个
// 独立开关——应用深色、任务栏浅色是合法组合。图标画在任务栏上，跟前者。
bool ReadTaskbarDarkMode() {
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, &type, &value, &size)
        != ERROR_SUCCESS) {
        return true;   // 键不存在时任务栏是深色，这是 Windows 自己的默认
    }
    return value == 0;
}

// 把品牌标记画进当前渲染目标。几何来自 BrandMark.h，那是从 Assets/brand/openego-hub.svg
// 生成的，不要在这里另填一组坐标。
//
// 标记的墨迹在 24 格的画布上占 90% x 79%，所以按 px / 24 等比放就能填满整个图标框——
// 这正是换掉字形的理由之一：图标字体的字形在 em 框内自带行距，怎么调都比相邻的系统托盘
// 图标小一圈，此前那个 0.78 em 是在跟这段留白较劲。
void DrawBrandMark(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, int px) {
    const float scale = float(px) / Brand::kMarkViewBox;
    const bool compact = px <= Brand::kMarkSmallSizePx;
    const size_t count = std::size(Brand::kMarkEllipses);

    for (size_t i = 0; i < count; ++i) {
        // 小尺寸只留首尾两层：三层套在一起到 16 px 上层间净空只剩约 1 px，会糊成一团。
        if (compact && i != 0 && i + 1 != count) continue;

        const Brand::MarkEllipse& e = Brand::kMarkEllipses[i];
        const bool isCore = (i + 1 == count);
        const float coreScale = (compact && isCore) ? Brand::kMarkSmallCoreScale : 1.0f;

        const D2D1_POINT_2F center = D2D1::Point2F(e.cx * scale, e.cy * scale);
        const D2D1_ELLIPSE shape = D2D1::Ellipse(
            center, e.rx * scale * coreScale, e.ry * scale * coreScale);

        // 外轮廓在小尺寸下提到全不透明：半透明的细描边到 16 px 只剩几个灰点。
        const float alpha = (compact && i == 0) ? 1.0f : e.opacity;
        D2D1_COLOR_F color = brush->GetColor();
        color.a = alpha;
        brush->SetColor(color);

        rt->SetTransform(D2D1::Matrix3x2F::Rotation(Brand::kMarkRotationDeg, center));
        if (e.Filled()) {
            rt->FillEllipse(shape, brush);
        } else {
            rt->DrawEllipse(shape, brush, e.strokeWidth * scale);
        }
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

// 现画一个托盘用的 HICON。不随程序带位图是有意的：托盘要按当前 DPI 与任务栏明暗各出一版，
// 备成资源就得为每档 DPI 各存一张，而这个标记只是三个椭圆，现画更省。
HICON CreateBrandIcon(int px, bool darkBackground) {
    if (!g_app.d2dFactory || px <= 0) return nullptr;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = px;
    bmi.bmiHeader.biHeight = -px;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HICON icon = nullptr;
    bool drew = false;

    if (color) {
        HGDIOBJ oldBmp = SelectObject(memDc, color);

        // DPI 写死 96，让 DIP 与像素一比一。px 已经是按系统 DPI 算好的物理像素，再让 D2D
        // 缩放一次会把字形放出位图之外。
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            float(kBaseDpi), float(kBaseDpi));

        ID2D1DCRenderTarget* rt = nullptr;
        if (SUCCEEDED(g_app.d2dFactory->CreateDCRenderTarget(&props, &rt))) {
            RECT bindRect{0, 0, px, px};
            ID2D1SolidColorBrush* brush = nullptr;
            if (SUCCEEDED(rt->BindDC(memDc, &bindRect)) &&
                SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &brush)) &&
                brush) {
                rt->BeginDraw();
                rt->Clear(D2D1::ColorF(0, 0.0f));

                // 托盘里一律单色。这里曾经按八个方向各偏移画一份反色当作轮廓，用意是在取了
                // 强调色的任务栏上保住形状；代价是细节被八份拷贝糊成一团半透明色块，看起来
                // 就是图标带了个灰底。系统自带的托盘图标都是纯单色，靠按任务栏明暗选黑或白
                // 来保证对比，这里照做。
                brush->SetColor(darkBackground ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                               : D2D1::ColorF(0.10f, 0.10f, 0.10f, 1.0f));
                DrawBrandMark(rt, brush, px);

                drew = SUCCEEDED(rt->EndDraw());
                brush->Release();
            }
            rt->Release();
        }

        // 位图还选在 DC 上时 CreateIconIndirect 读不到它，必须先解绑。
        SelectObject(memDc, oldBmp);

        if (drew) {
            // 1bpp 的 AND 掩码全零表示处处不透明，形状交给 32 位色位图的 alpha 通道。
            // 每行按 WORD 对齐，这是 CreateBitmap 对单色位图的要求。
            const int maskStride = ((px + 15) / 16) * 2;
            std::vector<BYTE> maskBits(size_t(maskStride) * size_t(px), 0);
            if (HBITMAP mask = CreateBitmap(px, px, 1, 1, maskBits.data())) {
                ICONINFO info{};
                info.fIcon = TRUE;
                info.hbmMask = mask;
                info.hbmColor = color;
                icon = CreateIconIndirect(&info);
                DeleteObject(mask);
            }
        }
        DeleteObject(color);
    }

    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return icon;
}

// 按当前 DPI 与任务栏主题重画图标。已经加进托盘时顺带 NIM_MODIFY 换上。
// 尺寸和主题都没变就直接返回：WM_SETTINGCHANGE 因为各种无关的设置项频繁到达，每次都重画
// 一遍是白费的。
void RefreshTrayIconImage(bool dark) {
    const int px = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    if (g_app.trayIcon.hIcon && px == g_app.trayIconPx && dark == g_app.trayIconDark) {
        return;
    }

    HICON fresh = CreateBrandIcon(px, dark);
    const bool owned = fresh != nullptr;
    if (!fresh) {
        // D2D 没起来或者位图建不出来。托盘项没有图标就是一块空白，宁可用系统的通用图标。
        fresh = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
    }
    if (!fresh) return;   // 连兜底都拿不到：保留现有图标，别把它换成空

    HICON previous = g_app.trayIcon.hIcon;
    const bool previousOwned = g_app.trayIconOwned;
    g_app.trayIcon.hIcon = fresh;
    g_app.trayIconOwned = owned;
    g_app.trayIconDark = dark;
    g_app.trayIconPx = px;

    if (g_app.trayIconAdded) Shell_NotifyIconW(NIM_MODIFY, &g_app.trayIcon);

    // Shell_NotifyIcon 已经复制了图标，这时销毁旧的那个是安全的。LoadIcon 返回的是共享
    // 图标，销毁它会影响别处，所以只销毁自己画的。
    if (previousOwned && previous) DestroyIcon(previous);
}

void AddTrayIcon() {
    g_app.trayIcon.cbSize = sizeof(g_app.trayIcon);
    g_app.trayIcon.hWnd = g_app.hwnd;
    g_app.trayIcon.uID = 1;
    g_app.trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_app.trayIcon.uCallbackMessage = WM_TRAYICON;
    wcscpy_s(g_app.trayIcon.szTip, L"OpenEGo Hub");

    // 此时 trayIconAdded 仍为 false，只填 hIcon，不会去发 NIM_MODIFY。
    RefreshTrayIconImage(ReadTaskbarDarkMode());

    g_app.trayIconAdded = Shell_NotifyIconW(NIM_ADD, &g_app.trayIcon) != FALSE;
}

// 气泡另填一份 NOTIFYICONDATA，不复用 g_app.trayIcon：那一份还带着 NIF_ICON 与 NIF_TIP，
// 每次换图标都要拿它去 NIM_MODIFY，把 NIF_INFO 留在里面会让同一条提示随后再弹一次。
void ShowTrayBalloon(const wchar_t* title, const wchar_t* text) {
    if (!g_app.trayIconAdded) return;

    NOTIFYICONDATAW balloon{};
    balloon.cbSize = sizeof(balloon);
    balloon.hWnd = g_app.trayIcon.hWnd;
    balloon.uID = g_app.trayIcon.uID;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = NIIF_WARNING;
    wcsncpy_s(balloon.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(balloon.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &balloon);
}

void ReleaseTrayIcon() {
    if (g_app.trayIconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_app.trayIcon);
        g_app.trayIconAdded = false;
    }
    if (g_app.trayIconOwned && g_app.trayIcon.hIcon) {
        DestroyIcon(g_app.trayIcon.hIcon);
        g_app.trayIconOwned = false;
    }
    g_app.trayIcon.hIcon = nullptr;
    g_app.trayIconPx = 0;
}

// ── tray menu ────────────────────────────────────────────────────────────────

// 服务不在时 Open 失败，菜单据此置灰。每次弹出都试一次——服务可能刚起来。
bool EnsureControlChannel() {
    std::lock_guard<std::mutex> submitLock(g_controlSubmitMutex);
    return g_app.control.IsOpen() || g_app.control.Open();
}

// 租约不能长期复用旧句柄：服务重启后旧 mapping 会被本进程的句柄续命，向它写入仍然返回
// 成功，但新 Host 看不到。心跳每次关闭再打开，代价只有每秒两个命名对象查询。
bool SubmitProviderLease(PenControl::ProviderLeaseCommand command) {
    std::lock_guard<std::mutex> submitLock(g_controlSubmitMutex);
    g_app.control.Close();
    if (!g_app.control.Open() || !g_app.control.SubmitProviderLease(command)) {
        g_app.control.Close();
        return false;
    }
    return true;
}

bool SubmitPenButtonModeCommand(uint8_t mode) {
    std::lock_guard<std::mutex> submitLock(g_controlSubmitMutex);
    g_app.control.Close();
    if (!g_app.control.Open() || !g_app.control.SubmitPenButtonMode(mode)) {
        g_app.control.Close();
        return false;
    }
    return true;
}

bool SubmitKbdDetachSupportCommand(bool enabled) {
    std::lock_guard<std::mutex> submitLock(g_controlSubmitMutex);
    g_app.control.Close();
    const auto command = enabled ? PenControl::KbdDetachSupportCommand::Enable
                                 : PenControl::KbdDetachSupportCommand::Disable;
    if (!g_app.control.Open() || !g_app.control.SubmitKbdDetachSupport(command)) {
        g_app.control.Close();
        return false;
    }
    return true;
}

// 键盘开关的唯一入口：托盘菜单和设置窗转发过来的命令都走这里，pending 态才只有一处维护。
bool RequestKbdDetachSupport(bool enabled) {
    if (!SubmitKbdDetachSupportCommand(enabled)) {
        g_app.kbdDetachPending = false;
        ShowTrayBalloon(L"键盘设置未生效", L"服务未响应，设置未能送达。");
        return false;
    }
    g_app.kbdDetachPending = true;
    g_app.kbdDetachPendingValue = enabled;
    g_app.kbdDetachPendingDeadline = GetTickCount64() + kKbdDetachPendingMs;
    return true;
}

// 充电阈值与色域只是转发给服务。托盘是中完整性进程：改充电阈值走 WMI 需要提权，而落地
// 这两件事的是 gaokun-hal 的组件，服务才知道它们装在哪。
bool SubmitChargeLimitCommand(uint8_t percent) {
    std::lock_guard<std::mutex> submitLock(g_controlSubmitMutex);
    g_app.control.Close();
    if (!g_app.control.Open() || !g_app.control.SubmitChargeLimit(percent)) {
        g_app.control.Close();
        return false;
    }
    return true;
}

// ── 华为用户态自启项 ─────────────────────────────────────────────────────────
//
// 「禁用厂商组件」在服务那侧做了两层：杀掉 PC Manager 目录下的进程，以及把七个 SCM 服务
// 设为禁用。第三层落不到服务身上——AcAppDaemon 一类是 HKCU\...\Run 里的自启项，服务跑在
// session 0 的 LocalSystem 下，写 HKCU 会落进 SYSTEM 自己的 hive，用户那份一条也改不到。
// 托盘就在用户会话里，是唯一能写对 hive 的地方。
//
// 必须处理，不只是省内存：AcAppDaemon 会拉起 AccessoryApp，那条链经 PenService.dll /
// KeyboardService.dll 打开与本程序同一个 MCU device path，两边 ReadFile 互吃包，
// 见 docs/KBDMCU_PROTOCOL.md 6.3。进程被杀掉之后 Run 项还在，下次登录照样回来。
constexpr wchar_t kVendorAutorunBackupKey[] =
    L"Software\\OpenEGoHub\\VendorAutorunBackup";

// Run 键的值个数上限。这里跑在 UI 线程上，遍历必须有边界；正常机器上这个键只有十几项。
constexpr DWORD kMaxRunValues = 256;
constexpr DWORD kMaxRunValueNameChars = 16384;   // 注册表值名上限 16383 字符
constexpr DWORD kMaxRunValueDataBytes = 65536;

// 托盘没有日志文件，调试输出是唯一的通道。这些失败对用户没有可操作性（真正的开关状态
// 由服务那半边回报），所以不弹气球提示，只留给调试器。
//
// TODO: 托盘接一份日志文件。整个进程只有这里两行 OutputDebugStringW，用户机器上没人挂
// 调试器，于是自启项处理失败在真实部署里完全不可见：备份写失败会让 Run 项留在原地，用户
// 看到的是「点了禁用，AcAppDaemon 每次登录还是回来」，而日志里一个字都没有。服务侧有
// Common/include/Logger.h 那套，托盘照着接一份即可，不必新造。
void LogAutorun(const wchar_t* format, ...) {
    wchar_t line[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, std::size(line), _TRUNCATE, format, args);
    va_end(args);
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
}

std::wstring ToLowerCopy(const std::wstring& text) {
    std::wstring lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return lowered;
}

// 从 Run 值的命令行里取出可执行文件路径。带引号的直接取引号内；不带引号的按 .exe 后缀
// 断句——不能简单地切到第一个空格，「C:\Program Files\...」本身就带空格。
std::wstring ExtractExecutablePath(const std::wstring& command) {
    const size_t begin = command.find_first_not_of(L" \t");
    if (begin == std::wstring::npos) return {};
    const std::wstring trimmed = command.substr(begin);

    if (trimmed.front() == L'"') {
        const size_t closing = trimmed.find(L'"', 1);
        if (closing == std::wstring::npos) return trimmed.substr(1);
        return trimmed.substr(1, closing - 1);
    }

    const std::wstring lowered = ToLowerCopy(trimmed);
    size_t pos = 0;
    while ((pos = lowered.find(L".exe", pos)) != std::wstring::npos) {
        const size_t end = pos + 4;
        if (end == lowered.size() || lowered[end] == L' ') return trimmed.substr(0, end);
        pos = end;
    }
    // 没有 .exe 后缀（脚本、rundll32 之类）就退回第一个空格前的部分。
    return trimmed.substr(0, trimmed.find(L' '));
}

// 判据与服务侧的 TerminateVendorProcesses 一致：只看目录，不认进程名。写死 AcAppDaemon
// 会漏掉华为以后新加的自启项，而这两个目录的语义是稳定的。
bool IsVendorAutorunCommand(DWORD type, const std::wstring& command) {
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;

    std::wstring expanded = command;
    if (type == REG_EXPAND_SZ) {
        wchar_t buffer[32768]{};
        const DWORD written = ExpandEnvironmentStringsW(
            command.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
        if (written > 0 && written <= static_cast<DWORD>(std::size(buffer))) {
            expanded.assign(buffer);
        }
    }

    const std::wstring path = ToLowerCopy(ExtractExecutablePath(expanded));
    return path.find(L"\\huawei\\pcmanager\\") != std::wstring::npos ||
           path.find(L"\\huawei\\hiview\\") != std::wstring::npos;
}

struct RunValue {
    std::wstring      name;
    DWORD             type = REG_SZ;
    std::vector<BYTE> data;
};

// 枚举一个键下的全部值。删值会打断枚举，所以先收齐再动手。
std::vector<RunValue> EnumerateValues(HKEY key) {
    std::vector<RunValue> values;
    std::wstring       name(kMaxRunValueNameChars, L'\0');
    std::vector<BYTE>  data(kMaxRunValueDataBytes);

    for (DWORD index = 0; index < kMaxRunValues; ++index) {
        DWORD nameChars = static_cast<DWORD>(name.size());
        DWORD dataBytes = static_cast<DWORD>(data.size());
        DWORD type = 0;
        const LONG result = RegEnumValueW(key, index, name.data(), &nameChars, nullptr,
                                          &type, data.data(), &dataBytes);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS) continue;

        RunValue value;
        value.name.assign(name.c_str(), nameChars);
        value.type = type;
        value.data.assign(data.begin(), data.begin() + dataBytes);
        values.push_back(std::move(value));
    }
    return values;
}

std::wstring ValueAsString(const RunValue& value) {
    if (value.data.size() < sizeof(wchar_t)) return {};
    const auto* text = reinterpret_cast<const wchar_t*>(value.data.data());
    const size_t chars = value.data.size() / sizeof(wchar_t);
    return std::wstring(text, wcsnlen(text, chars));
}

bool ValueExists(HKEY key, const std::wstring& name) {
    return RegQueryValueExW(key, name.c_str(), nullptr, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

// 禁用：把匹配到的 Run 项原样（名字 + 类型 + 数据）备份到 VendorAutorunBackup，再删掉。
// 幂等靠两点：已经有备份的不覆盖（覆盖会把华为重新写回来的那份当成原值），备份没写成功
// 就不删——否则这条自启项再也回不来。
bool DisableVendorAutorun() {
    HKEY runKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegistryKey, 0, KEY_READ | KEY_SET_VALUE,
                      &runKey) != ERROR_SUCCESS) {
        LogAutorun(L"[autorun] 打开 Run 键失败: %lu", GetLastError());
        return false;
    }

    std::vector<RunValue> vendorValues;
    for (auto& value : EnumerateValues(runKey)) {
        if (IsVendorAutorunCommand(value.type, ValueAsString(value))) {
            vendorValues.push_back(std::move(value));
        }
    }
    if (vendorValues.empty()) {
        RegCloseKey(runKey);
        return true;
    }

    HKEY backupKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kVendorAutorunBackupKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_SET_VALUE, nullptr,
                        &backupKey, nullptr) != ERROR_SUCCESS) {
        LogAutorun(L"[autorun] 创建备份键失败: %lu", GetLastError());
        RegCloseKey(runKey);
        return false;
    }

    bool allDone = true;
    for (const auto& value : vendorValues) {
        if (!ValueExists(backupKey, value.name)) {
            const LONG written = RegSetValueExW(
                backupKey, value.name.c_str(), 0, value.type, value.data.data(),
                static_cast<DWORD>(value.data.size()));
            if (written != ERROR_SUCCESS) {
                LogAutorun(L"[autorun] 备份 %s 失败: %ld", value.name.c_str(), written);
                allDone = false;
                continue;
            }
        }
        const LONG deleted = RegDeleteValueW(runKey, value.name.c_str());
        if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) {
            LogAutorun(L"[autorun] 删除 %s 失败: %ld", value.name.c_str(), deleted);
            allDone = false;
        }
    }

    RegCloseKey(backupKey);
    RegCloseKey(runKey);
    return allDone;
}

// 恢复：按备份逐条写回 Run，写回成功的立刻从备份里删掉。没有备份就什么也不做——凭空造
// 一条 Run 项会在没装过 PC Manager 的机器上留下指向不存在文件的自启项。
bool RestoreVendorAutorun() {
    HKEY backupKey = nullptr;
    const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER, kVendorAutorunBackupKey, 0,
                                      KEY_READ | KEY_SET_VALUE, &backupKey);
    if (opened == ERROR_FILE_NOT_FOUND) return true;
    if (opened != ERROR_SUCCESS) {
        LogAutorun(L"[autorun] 打开备份键失败: %ld", opened);
        return false;
    }

    const std::vector<RunValue> backups = EnumerateValues(backupKey);
    if (backups.empty()) {
        RegCloseKey(backupKey);
        (void)RegDeleteKeyW(HKEY_CURRENT_USER, kVendorAutorunBackupKey);
        return true;
    }

    HKEY runKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegistryKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &runKey,
                        nullptr) != ERROR_SUCCESS) {
        LogAutorun(L"[autorun] 打开 Run 键失败: %lu", GetLastError());
        RegCloseKey(backupKey);
        return false;
    }

    bool allDone = true;
    for (const auto& value : backups) {
        const LONG written = RegSetValueExW(runKey, value.name.c_str(), 0, value.type,
                                            value.data.data(),
                                            static_cast<DWORD>(value.data.size()));
        if (written != ERROR_SUCCESS) {
            LogAutorun(L"[autorun] 写回 %s 失败: %ld", value.name.c_str(), written);
            allDone = false;
            continue;
        }
        (void)RegDeleteValueW(backupKey, value.name.c_str());
    }

    RegCloseKey(runKey);
    RegCloseKey(backupKey);
    // 全部写回之后备份键已经空了，顺手删掉；有条目没写回就留着，下次还能再试。
    if (allDone) (void)RegDeleteKeyW(HKEY_CURRENT_USER, kVendorAutorunBackupKey);
    return allDone;
}

// 逐个比对整参数，不用 wcsstr：安装目录的路径也在命令行里，子串匹配会把碰巧含有同样字样
// 的路径当成开关。
bool HasCommandLineSwitch(const wchar_t* name) {
    int count = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return false;

    bool found = false;
    for (int i = 1; i < count && !found; ++i) {
        found = _wcsicmp(argv[i], name) == 0;
    }
    LocalFree(argv);
    return found;
}

bool ApplyVendorAutorun(bool disable) {
    return disable ? DisableVendorAutorun() : RestoreVendorAutorun();
}

bool SubmitVendorServicesCommand(bool disable) {
    std::lock_guard<std::mutex> submitLock(g_controlSubmitMutex);
    g_app.control.Close();
    const auto command = disable ? PenControl::VendorServicesCommand::Disable
                                 : PenControl::VendorServicesCommand::Restore;
    if (!g_app.control.Open() || !g_app.control.SubmitVendorServices(command)) {
        g_app.control.Close();
        return false;
    }
    return true;
}

bool SubmitColorModeCommand(PenControl::ColorModeCommand command) {
    std::lock_guard<std::mutex> submitLock(g_controlSubmitMutex);
    g_app.control.Close();
    if (!g_app.control.Open() || !g_app.control.SubmitColorMode(command)) {
        g_app.control.Close();
        return false;
    }
    return true;
}

std::wstring SettingsExecutablePath() {
    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (length == 0 || length >= std::size(modulePath)) return {};

    std::wstring settingsPath(modulePath, length);
    const size_t separator = settingsPath.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    settingsPath.resize(separator + 1);
    settingsPath += L"OpenEGoHubSettings.exe";
    return settingsPath;
}

// 色温、护眼、自然色彩三项由托盘直接执行，不像色域那样经服务转发。理由是它们要落的
// 色彩状态存在 HKCU 下：服务跑在 LocalSystem，那是另一个 hive，写进去用户会话读不到。
//
// 自然色彩的守护进程更是只能在这里跑——它每帧重读状态，在服务的 hive 里会读到「已关闭」
// 然后第一帧就退出。
std::wstring DisplayToolPath() {
    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (length == 0 || length >= std::size(modulePath)) return {};

    std::wstring path(modulePath, length);
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    path.resize(separator + 1);
    path += L"GaokunDisplay.exe";
    return path;
}

// 同步跑一次显示工具。实测一条命令约 57 ms，含拉起进程、加载 qdcmlib、枚举显示器与写 PCC。
bool RunDisplayTool(const std::wstring& arguments) {
    const std::wstring exePath = DisplayToolPath();
    if (exePath.empty()) return false;

    std::wstring commandLine = L"\"" + exePath + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                        &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    // 等它做完再回，否则设置窗紧接着读状态会读到旧值。57 ms 在 IPC 处理里是可以接受的。
    const DWORD waited = WaitForSingleObject(process.hProcess, 5000);
    DWORD exitCode = 1;
    if (waited == WAIT_OBJECT_0) (void)GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return waited == WAIT_OBJECT_0 && exitCode == 0;
}

// 自然色彩的守护进程。它要持续跟随环境光，是这批功能里唯一常驻的一个。
std::mutex g_naturalColorMutex;
HANDLE g_naturalColorDaemon = nullptr;

void StopNaturalColorDaemon() {
    std::lock_guard<std::mutex> lock(g_naturalColorMutex);
    if (!g_naturalColorDaemon) return;
    // 守护进程发现状态被改成 off 会自己退出，但那要等到下一帧；这里直接终止，免得关掉开关
    // 之后它还在写 PCC。它只读状态与写 PCC，没有需要收尾的资源。
    (void)TerminateProcess(g_naturalColorDaemon, 0);
    CloseHandle(g_naturalColorDaemon);
    g_naturalColorDaemon = nullptr;
}

bool StartNaturalColorDaemon() {
    const std::wstring exePath = DisplayToolPath();
    if (exePath.empty()) return false;

    StopNaturalColorDaemon();

    std::wstring commandLine = L"\"" + exePath + L"\" --natural-color-daemon";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                        &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);

    std::lock_guard<std::mutex> lock(g_naturalColorMutex);
    g_naturalColorDaemon = process.hProcess;
    return true;
}

HWND FindSettingsBridge() {
    return FindWindowExW(
        HWND_MESSAGE, nullptr, EGoTouchTrayIpc::kSettingsBridgeWindowClass, nullptr);
}

bool LaunchSettingsProcess(bool background, bool reportError) {
    const std::wstring settingsPath = SettingsExecutablePath();
    if (settingsPath.empty()) return false;

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    std::wstring commandLine = L"\"" + settingsPath + L"\"";
    if (background) commandLine += L" --background";
    if (!CreateProcessW(settingsPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                        &startup, &process)) {
        if (reportError) {
            MessageBoxW(nullptr, L"无法启动 OpenEGo Hub 控制面板。请确认安装文件完整。",
                        L"OpenEGo Hub", MB_OK | MB_ICONERROR);
        }
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// 设置界面在 OpenEGoHubSettings.exe 里，托盘只负责拉起或激活它。此处原先还有一套 Win32
// 自绘面板，被 WinUI 取代后整块不可达，已删除。留两条实测结论，避免有人再往回走：
// 传统子控件是 GDI 表面，把整个客户区扩成 Mica 玻璃后它们不保留 alpha，只能把 Mica 限
// 在非客户区、客户区另铺一层不透明底色；另外 SetWindowTheme 会同步发出 WM_THEMECHANGED，
// DWM 的深色标题栏属性必须在子控件换肤之后再设，否则会被那一轮改回浅色。
void ShowSettingsPanel() {
    if (const HWND settings = FindSettingsBridge()) {
        PostMessageW(settings, EGoTouchTrayIpc::kActivateMessage, 0, 0);
        return;
    }
    (void)LaunchSettingsProcess(false, true);
}

void ShowWinUiNotification(EGoTouchTrayIpc::Notification notification, LPARAM payload) {
    // 工具切换不归「设备接入提醒」管：那个开关说的是配件接入，而这一条是用户刚按下侧键的
    // 反馈，关掉前者的人要的是少一条吸附提示，不是切了橡皮擦却没有回应。
    const bool deviceNotification =
        notification != EGoTouchTrayIpc::Notification::PenToolChanged;

    // 关掉之后在这里就返回，不去找、更不去拉起设置进程：弹窗的宿主是 WinUI 那个进程，
    // 一次都不弹却把它拉起来，等于关了开关反而多跑一个进程。
    //
    // TODO 「未正确吸附」这一条的触发机制没查清，所以现在跟接入提示一起被这个开关管着。
    // 已知的只有链路：服务把 notificationKind 置成 PenDeviation、递增 notificationSequence，
    // 托盘看到序号变化就弹。**服务凭什么判定「未正确吸附」尚未取证**——是 MCU 上报的某个
    // 状态位，还是我们自己按吸附/充电状态推的，要从 PenStatus 的生产侧往回查。
    // 查清之前不把它单列成一个开关：给不出「什么时候会弹」，这个开关就没法解释。
    if (deviceNotification && !g_app.deviceNotifications) return;

    HWND bridge = FindSettingsBridge();
    if (!bridge) {
        if (!LaunchSettingsProcess(true, false)) return;
        // WinUI bootstrap and XAML construction happen in the new process. This runs only
        // on the first notification of a session; subsequent notifications reuse the bridge.
        for (int attempt = 0; attempt < 80 && !bridge; ++attempt) {
            Sleep(25);
            bridge = FindSettingsBridge();
        }
    }
    if (bridge) {
        PostMessageW(bridge, EGoTouchTrayIpc::kNotificationMessage,
                     static_cast<WPARAM>(notification), payload);
    }
}

void RequestSafeExit() {
    if (g_app.exitPending) return;
    g_app.providerDesired = false;
    g_app.exitPending = true;
    g_app.exitDeadlineTick = GetTickCount64() + kSafeExitTimeoutMs;
    (void)SubmitProviderLease(PenControl::ProviderLeaseCommand::Release);
}

void MaintainProviderLease() {
    if (g_app.exitPending) {
        (void)SubmitProviderLease(PenControl::ProviderLeaseCommand::Release);
        PollChannel();
        if (g_app.hasState && g_app.state.hasTouchProvider &&
            g_app.state.touchProvider == PenStatus::TouchProviderState::Huawei) {
            DestroyWindow(g_app.hwnd);
            return;
        }
        if (GetTickCount64() >= g_app.exitDeadlineTick) {
            g_app.exitPending = false;
            MessageBoxW(nullptr,
                        L"未能确认 HuaweiTHP 已恢复，OpenEGo Hub 将继续留在托盘，避免在无触控状态下退出。",
                        L"OpenEGo Hub", MB_OK | MB_ICONERROR);
        }
        return;
    }

    (void)SubmitProviderLease(
        g_app.providerDesired
            ? PenControl::ProviderLeaseCommand::AcquireOrRenew
            : PenControl::ProviderLeaseCommand::Release);
}

void AppendPenButtonItem(HMENU menu, PenButtonMode mode, const wchar_t* label, bool enabled) {
    UINT flags = MF_STRING;
    if (!enabled) flags |= MF_GRAYED;
    // 对勾的数据源是状态广播里服务回报的当前模式，不是本进程提交过什么：提交可能被服务
    // 按枚举校验拒绝，也可能被别处的配置改写。服务没报模式时一项都不勾。
    if (g_app.hasState && g_app.state.hasPenButtonMode &&
        g_app.state.penButtonMode == static_cast<uint8_t>(mode)) {
        flags |= MF_CHECKED;
    }
    AppendMenuW(menu, flags, kTrayMenuPenButtonBase + static_cast<UINT>(mode), label);
}

void ShowTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);

    // 现读现建。轮询最长滞后 400 毫秒，正好覆盖「刚点完一项又打开菜单」这个最容易看出
    // 不对的时刻；菜单本身也不缓存，否则服务改了模式之后对勾还停在旧的那一项。
    PollChannel();
    const bool connected = EnsureControlChannel();

    HMENU penMenu = CreatePopupMenu();
    // 与控制面板上的下拉项逐字一致。同一个设置在两处叫不同的名字，用户会以为是两件事。
    AppendPenButtonItem(penMenu, PenButtonMode::WindowsInk, L"遵循系统笔设置", connected);
    AppendPenButtonItem(penMenu, PenButtonMode::ToggleEraser, L"切换书写与橡皮擦", connected);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kTrayMenuSettings, L"打开控制面板");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // 这一项控制的是托盘租约，也就是触控由本程序接管还是交还原厂。名字里原先带着
    // EGoTouchRev，那是自研驱动时期的叫法，现在走的是原厂算法链的 ARM64EC 宿主。
    AppendMenuW(menu, MF_STRING | (g_app.providerDesired ? MF_CHECKED : 0),
                kTrayMenuProvider, L"接管触控与手写笔");
    // 护眼是新做的几项里最常用的一个，值得一个不用开面板的入口。状态读注册表——那是托盘
    // 自己每次改完写下的，与 GaokunDisplay 的持久化状态同源。
    AppendMenuW(menu, MF_STRING | (ReadUserSetting(L"EyeComfort", 0) ? MF_CHECKED : 0),
                kTrayMenuEyeComfort, L"护眼模式");
    // 顶层这一项不置灰：置灰的弹出项展不开，用户就看不到里面为什么不能选。
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(penMenu),
                connected ? L"笔双击行为" : L"笔双击行为（服务未连接）");
    // 键盘在不在，用 MCU 回报的在位状态判断；开关值是否已知另算。只看后者的话，键盘拔掉
    // 之后这一项仍然可点——那个标志只说明收到过一次应答，不会因为键盘离开而复位。
    {
        // 读键盘的那个 hal 宿主还在不在。服务发布这一位之前（旧服务、或 touch_only）
        // 读不到，此时按「在」处理，否则这一项会凭空变灰。
        const bool hostAlive = !g_app.state.hasHostHealth || g_app.state.kbdHostHealthy;
        const bool kbdKnown =
            connected && g_app.hasState && hostAlive &&
            g_app.state.hasKbdPresent && g_app.state.kbdPresent &&
            g_app.state.hasKbdDetachSupport;
        UINT flags = MF_STRING;
        if (!kbdKnown && !g_app.kbdDetachPending) flags |= MF_GRAYED;
        // 在途请求期间显示用户刚选的那个值，等快照回来再交还给真值。
        const bool checked = g_app.kbdDetachPending
            ? g_app.kbdDetachPendingValue
            : (kbdKnown && g_app.state.kbdDetachSupport);
        if (checked) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, kTrayMenuKbdDetach,
                    hostAlive ? L"键盘分离后保持无线连接"
                              : L"键盘分离后保持无线连接（键盘组件无响应）");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // 与控制面板上那一项同名。两处指向同一个动作，叫法不同会让人以为是两件事。
    AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"退出并交还原厂");

    // Required so the menu dismisses when focus moves elsewhere.
    SetForegroundWindow(g_app.hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.hwnd, nullptr);
    DestroyMenu(menu);   // 子菜单随父菜单一并销毁
}

// ── window ───────────────────────────────────────────────────────────────────

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == kTimerPoll) {
            PollChannel();
        } else if (wParam == kTimerLease) {
            MaintainProviderLease();
        } else if (wParam == kTimerAnim) {
            const DWORD elapsed = GetTickCount() - g_app.phaseStartTick;
            if (g_app.phase == Phase::FadeIn) {
                const float t = std::min(1.0f, float(elapsed) / float(kFadeInMs));
                g_app.alpha = BYTE(t * 255.0f);
                ApplyAlphaOnly();
                if (t >= 1.0f) {
                    g_app.phase = Phase::Dwell;
                    g_app.phaseStartTick = GetTickCount();
                    KillTimer(hwnd, kTimerAnim);
                    SetTimer(hwnd, kTimerDwell, kDwellMs, nullptr);
                }
            } else if (g_app.phase == Phase::FadeOut) {
                const float t = std::min(1.0f, float(elapsed) / float(kFadeOutMs));
                g_app.alpha = BYTE((1.0f - t) * 255.0f);
                ApplyAlphaOnly();
                if (t >= 1.0f) FinishHide();
            }
        } else if (wParam == kTimerDwell) {
            KillTimer(hwnd, kTimerDwell);
            // 悬停时推迟消失，让人能读完。
            //
            // 判据是「光标此刻在不在面板上」，不是 WM_MOUSELEAVE 有没有来过。原先靠那条消息
            // 清 hovered，而它并不保证送达：触摸操作不产生它，光标被别的窗口捕获时也不产生。
            // 漏掉一次，hovered 就永远是真，这个定时器每 500ms 自我续期，面板再也不消失。
            // 直接问光标位置没有这个失败模式——它不依赖任何消息按时到达。
            if (g_app.phase == Phase::Dwell) {
                POINT cursor{};
                RECT panel{};
                const bool over = GetCursorPos(&cursor) && GetWindowRect(hwnd, &panel) &&
                                  PtInRect(&panel, cursor);
                g_app.hovered = over;
                if (over) {
                    SetTimer(hwnd, kTimerDwell, 500, nullptr);
                } else {
                    BeginHide();
                }
            }
        }
        return 0;

    // 唤醒后立刻续一次租，不等下一拍心跳。服务在唤醒时给的宽限期是有限的，而心跳是每秒
    // 一次的 WM_TIMER，恢复得比这条消息晚；早一秒续上，触控就早一秒回来。
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC) {
            MaintainProviderLease();
        }
        return TRUE;

    case WM_MOUSEMOVE:
        if (!g_app.hovered) {
            g_app.hovered = true;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        return 0;

    case WM_MOUSELEAVE:
        g_app.hovered = false;
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            ShowTrayMenu();
        } else if (LOWORD(lParam) == WM_LBUTTONUP) {
            ShowSettingsPanel();
        }
        return 0;

    case EGoTouchTrayIpc::kCommandMessage: {
        const auto command = static_cast<EGoTouchTrayIpc::Command>(wParam);
        switch (command) {
        case EGoTouchTrayIpc::Command::SetProviderEnabled:
            g_app.providerDesired = lParam != 0;
            (void)SubmitProviderLease(
                g_app.providerDesired
                    ? PenControl::ProviderLeaseCommand::AcquireOrRenew
                    : PenControl::ProviderLeaseCommand::Release);
            return 1;

        case EGoTouchTrayIpc::Command::SetPenButtonMode: {
            const auto mode = PenButtonModeFromNumeric(static_cast<int32_t>(lParam));
            if (!mode || (*mode != PenButtonMode::WindowsInk &&
                          *mode != PenButtonMode::ToggleEraser)) {
                return 0;
            }
            return SubmitPenButtonModeCommand(static_cast<uint8_t>(*mode)) ? 1 : 0;
        }

        case EGoTouchTrayIpc::Command::SetOneNoteCompatibility: {
            const bool requested = lParam != 0;
            if (!WriteUserSetting(L"OneNoteCompatibility", requested ? 1u : 0u)) {
                return 0;
            }
            g_app.oneNoteCompatibility = requested;
            g_oneNoteCompatibility.store(requested, std::memory_order_release);
            return 1;
        }

        case EGoTouchTrayIpc::Command::SetAutoStart: {
            const bool requested = lParam != 0;
            const bool previous = g_app.autoStart;
            if (!WriteUserSetting(L"AutoStart", requested ? 1u : 0u)) return 0;
            if (!SetLoginAutoStart(requested)) {
                (void)WriteUserSetting(L"AutoStart", previous ? 1u : 0u);
                (void)SetLoginAutoStart(previous);
                return 0;
            }
            g_app.autoStart = requested;
            return 1;
        }

        case EGoTouchTrayIpc::Command::SetKeyboardWirelessOnDetach:
            return RequestKbdDetachSupport(lParam != 0) ? 1 : 0;

        case EGoTouchTrayIpc::Command::SetVendorServicesDisabled: {
            const bool disable = lParam != 0;
            // 先处理 HKCU 的自启项，再把服务那一半转发出去。两层互相独立：Run 项没处理成
            // 功也要让服务去停服务、杀进程，能做一层是一层。失败只进调试输出，开关状态由
            // 服务回报，这里回滚反而会让界面和实际不符。
            (void)ApplyVendorAutorun(disable);
            return SubmitVendorServicesCommand(disable) ? 1 : 0;
        }

        case EGoTouchTrayIpc::Command::SetChargeLimit: {
            const auto percent = static_cast<uint8_t>(lParam);
            if (!SubmitChargeLimitCommand(percent)) return 0;
            // 0 是「交还智能充电」的哨兵而不是一个上限，记下去会让设置窗下次把滑块摆到 0。
            // 阈值现在能从硬件读回来，这份记录只在服务还没连上时用作初值。
            if (percent != 0) (void)WriteUserSetting(L"ChargeLimit", percent);
            return 1;
        }

        case EGoTouchTrayIpc::Command::SetColorTemperature: {
            const auto kelvin = static_cast<int>(lParam);
            wchar_t args[64];
            if (kelvin <= 0) {
                swprintf_s(args, L"--temperature off");
            } else {
                swprintf_s(args, L"--temperature %d", kelvin);
            }
            if (!RunDisplayTool(args)) return 0;
            (void)WriteUserSetting(L"ColorTemperature", static_cast<DWORD>(kelvin <= 0 ? 0 : kelvin));
            return 1;
        }

        case EGoTouchTrayIpc::Command::SetEyeComfort: {
            const bool enabled = lParam != 0;
            if (!RunDisplayTool(enabled ? L"--eye-comfort on" : L"--eye-comfort off")) return 0;
            (void)WriteUserSetting(L"EyeComfort", enabled ? 1u : 0u);
            return 1;
        }

        case EGoTouchTrayIpc::Command::SetNaturalColor: {
            const bool enabled = lParam != 0;
            // 先落状态再管守护进程：守护进程启动后第一件事就是读状态，顺序反了它会读到
            // 「已关闭」然后立刻退出。
            if (!RunDisplayTool(enabled ? L"--natural-color on" : L"--natural-color off")) return 0;
            if (enabled) {
                if (!StartNaturalColorDaemon()) {
                    // 一次性下发已经生效，只是不会再跟随环境光变化。不回滚状态：那会让
                    // 用户看到开关自己弹回去，而画面其实已经变了。
                    (void)WriteUserSetting(L"NaturalColor", 1u);
                    return 1;
                }
            } else {
                StopNaturalColorDaemon();
            }
            (void)WriteUserSetting(L"NaturalColor", enabled ? 1u : 0u);
            return 1;
        }

        case EGoTouchTrayIpc::Command::SetColorMode: {
            const auto command = static_cast<PenControl::ColorModeCommand>(lParam);
            if (!SubmitColorModeCommand(command)) return 0;
            // 存的是下拉的索引，与 XAML 里三项的顺序对应（Native / sRGB / Display P3），
            // 而不是命令枚举的值。
            const DWORD index = command == PenControl::ColorModeCommand::Srgb        ? 1u
                                : command == PenControl::ColorModeCommand::DisplayP3 ? 2u
                                                                                     : 0u;
            (void)WriteUserSetting(L"ColorMode", index);
            return 1;
        }

        case EGoTouchTrayIpc::Command::SetDeviceNotifications: {
            const bool requested = lParam != 0;
            if (!WriteUserSetting(L"DeviceNotifications", requested ? 1u : 0u)) return 0;
            g_app.deviceNotifications = requested;
            return 1;
        }

        case EGoTouchTrayIpc::Command::RequestSafeExit:
            RequestSafeExit();
            return 1;
        }
        return 0;
    }

    // 再次启动托盘的语义是「打开这个应用」。开始菜单的快捷方式指向托盘，而托盘是单实例的，
    // 第二个实例只能把请求转交给已在运行的这一个，否则点击快捷方式毫无反应。
    case EGoTouchTrayIpc::kActivateMessage:
        ShowSettingsPanel();
        return 0;

    case WM_COMMAND: {
        const UINT id = LOWORD(wParam);
        if (id == kTrayMenuExit) {
            RequestSafeExit();
        } else if (id == kTrayMenuSettings) {
            ShowSettingsPanel();
        } else if (id == kTrayMenuProvider) {
            g_app.providerDesired = !g_app.providerDesired;
            (void)SubmitProviderLease(
                g_app.providerDesired
                    ? PenControl::ProviderLeaseCommand::AcquireOrRenew
                    : PenControl::ProviderLeaseCommand::Release);
        } else if (id == kTrayMenuKbdDetach) {
            // 与双击行为不同，这一项要先显示成用户选的那个值：它的真值在键盘固件里，回读要
            // 走服务、命令管道和 MCU，不显示在途态就会看到对勾原地不动。
            const bool current = g_app.kbdDetachPending ? g_app.kbdDetachPendingValue
                                                        : g_app.state.kbdDetachSupport;
            (void)RequestKbdDetachSupport(!current);
        } else if (id == kTrayMenuEyeComfort) {
            // 与设置窗那条走同一个入口：色彩状态存在 HKCU，必须在用户会话里落地，服务的
            // hive 不是这一份。菜单每次现建，对勾下次打开时自然就对了。
            const bool enable = ReadUserSetting(L"EyeComfort", 0) == 0;
            if (RunDisplayTool(enable ? L"--eye-comfort on" : L"--eye-comfort off")) {
                (void)WriteUserSetting(L"EyeComfort", enable ? 1u : 0u);
            }
        } else if (id >= kTrayMenuPenButtonBase &&
                   id < kTrayMenuPenButtonBase + kTrayMenuPenButtonSpan) {
            // 提交完不动对勾：能不能生效由服务说了算，下一次轮询读回新模式时菜单自然就对了。
            const uint8_t mode = static_cast<uint8_t>(id - kTrayMenuPenButtonBase);
            (void)SubmitPenButtonModeCommand(mode);
        }
        return 0;
    }

    case WM_DPICHANGED:
        // Suggested rect arrives in lParam, but the panel is anchored to a screen corner
        // rather than preserving its position, so recompute placement from scratch.
        g_app.dpi = HIWORD(wParam);
        CreateTextFormats();
        if (g_app.phase != Phase::Hidden) {
            PositionPanel();
            RenderPanel();
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE: {
        // 主题切换也走 WM_SETTINGCHANGE（lParam = "ImmersiveColorSet"）。深浅两套图标是
        // 由同一份 PNG 改色得来的，所以主题一变缓存就作废。
        const bool dark = ReadDarkMode();
        if (dark != g_app.darkMode) {
            g_app.darkMode = dark;
            g_app.battery.key = -1;
        }
        // 托盘图标跟任务栏主题，与面板的应用主题分开判断。显示配置变化也走这里，图标尺寸
        // 跟着系统 DPI，所以一并交给它判断。
        RefreshTrayIconImage(ReadTaskbarDarkMode());
        if (g_app.phase != Phase::Hidden) {
            PositionPanel();
            RenderPanel();
        }
        return 0;
    }

    case WM_DESTROY:
        ReleaseTrayIcon();
        // 自然色彩的守护进程是托盘拉起的，托盘走了它没人管——留着会继续按最后一次读到的
        // 环境光写 PCC，而界面已经不在了，用户无从关掉它。
        StopNaturalColorDaemon();
        // 退出路径统一收在 wWinMain 里，那里靠这个空句柄判断窗口还要不要销毁。
        g_app.hwnd = nullptr;
        PostQuitMessage(0);
        return 0;

    case WM_ENDSESSION:
        if (wParam) {
            // 这里不能等 UI 消息循环完成切换；先显式释放，来不及处理时 5 秒租约仍是兜底。
            g_app.providerDesired = false;
            (void)SubmitProviderLease(PenControl::ProviderLeaseCommand::Release);
            DestroyWindow(hwnd);
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ReleaseTextFormats() {
    if (g_app.fmtPercentSign) { g_app.fmtPercentSign->Release(); g_app.fmtPercentSign = nullptr; }
    if (g_app.fmtPercent) { g_app.fmtPercent->Release(); g_app.fmtPercent = nullptr; }
    if (g_app.fmtBody)    { g_app.fmtBody->Release();    g_app.fmtBody = nullptr; }
    if (g_app.fmtTitle)   { g_app.fmtTitle->Release();   g_app.fmtTitle = nullptr; }
}

// Text formats carry a fixed point size, so they have to be rebuilt whenever the panel
// moves to a monitor with a different DPI — scaling the layout alone would leave the type
// at its old physical size.
bool CreateTextFormats() {
    if (!g_app.dwriteFactory) return false;
    ReleaseTextFormats();

    auto makeFormat = [](float sizeDip, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        // Segoe UI Variable falls back cleanly to Segoe UI on older builds, and both
        // carry the CJK glyphs the status line needs.
        return g_app.dwriteFactory->CreateTextFormat(
            L"Segoe UI Variable", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, ScaledF(sizeDip), L"zh-cn", out);
    };
    // 字号取自 window_main.baml 的样式：名称 CardTextNameStyle 14 常规，电量
    // BatteryValueStyle 18 加粗，百分号 BatteryPercentStyle 10 常规且底对齐——原厂是把数字
    // 和百分号当两个元素画的，不是整体一个字号。
    makeFormat(14.0f, DWRITE_FONT_WEIGHT_NORMAL, &g_app.fmtTitle);
    makeFormat(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &g_app.fmtBody);
    makeFormat(18.0f, DWRITE_FONT_WEIGHT_BOLD, &g_app.fmtPercent);
    makeFormat(10.0f, DWRITE_FONT_WEIGHT_NORMAL, &g_app.fmtPercentSign);

    // 行内各元素字号不同，靠段落居中让它们的基准一致。
    for (auto* f : {g_app.fmtTitle, g_app.fmtBody, g_app.fmtPercent}) {
        if (f) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (g_app.fmtPercentSign) {
        g_app.fmtPercentSign->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    }

    // 文字一律不换行，超长截断加省略号。版式是按单行算高度的，一旦折到第二行整张卡片就乱；
    // 截断只损失可读性，换行会损失结构。原厂 deviceinfopage.baml 用的也是 CharacterEllipsis。
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    for (auto* f : {g_app.fmtTitle, g_app.fmtBody}) {
        if (!f) continue;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        IDWriteInlineObject* sign = nullptr;
        if (SUCCEEDED(g_app.dwriteFactory->CreateEllipsisTrimmingSign(f, &sign)) && sign) {
            f->SetTrimming(&trimming, sign);
            sign->Release();
        }
    }
    return g_app.fmtTitle && g_app.fmtBody && g_app.fmtPercent && g_app.fmtPercentSign;
}

bool InitGraphics() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_app.d2dFactory))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_app.dwriteFactory)))) {
        return false;
    }
    // WIC 只用于读 PCManager 的电量图标。拿不到不算致命：面板退回自绘电量条。
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_app.wicFactory)))) {
        g_app.wicFactory = nullptr;
    }
    g_app.darkMode = ReadDarkMode();
    return CreateTextFormats();
}

void ReleaseGraphics() {
    ReleaseTextFormats();
    if (g_app.wicFactory) g_app.wicFactory->Release();
    if (g_app.dwriteFactory) g_app.dwriteFactory->Release();
    if (g_app.d2dFactory) g_app.d2dFactory->Release();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    // 卸载时把华为的登录自启项还回去，做完就退出。
    //
    // 这一趟只能由托盘跑：备份记在 HKCU，而卸载流程本身以系统身份执行，它读到的是 SYSTEM
    // 自己的 hive，用户那份一条也看不到。安装包因此在删文件之前以用户身份调一次本程序。
    //
    // 放在单实例互斥量之前：卸载时通常还有一个托盘在跑，抢不到互斥量就直接返回的话这趟
    // 什么也不会做，而它正是最后一次能恢复的机会。恢复本身只动注册表，与那个实例不冲突。
    if (HasCommandLineSwitch(L"--restore-vendor-autorun")) {
        return RestoreVendorAutorun() ? 0 : 1;
    }

    // One panel per session is enough; a second instance would fight over the same
    // screen corner.
    HANDLE singleton = CreateMutexW(nullptr, TRUE, kMutexName);
    if (singleton && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已经有一个托盘在跑。安装后开始菜单里的入口指向的就是本程序，用户点它是想打开
        // OpenEGo Hub，而不是希望什么都不发生，所以把请求转交给在跑的那个实例去开设置窗。
        if (const HWND existing = FindWindowW(EGoTouchTrayIpc::kTrayWindowClass, nullptr)) {
            PostMessageW(existing, EGoTouchTrayIpc::kActivateMessage, 0, 0);
        }
        return 0;
    }

    // 升级安装时 Restart Manager 会关掉托盘(WM_ENDSESSION 路径),装完靠 RmRestart 把注册
    // 过的进程拉回来;不注册,升级结束托盘就消失了。RESTART_NO_REBOOT 是因为重启后的拉起
    // 已由 HKCU Run 键负责,两边都来会有第二个实例向主实例投递激活消息,凭空弹出设置窗。
    RegisterApplicationRestart(nullptr, RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // WIC 是 COM 组件，读 PCManager 的电量图标要靠它。
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_app.instance = instance;
    LoadUserSettings();
    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_STANDARD_CLASSES};
    const bool commonControlsReady = InitCommonControlsEx(&commonControls) != FALSE;
    // 只要用户主动或由登录启动项拉起托盘，本次会话就请求 OpenEGo Hub 接管；主开关可在
    // 不退出面板的情况下交还 Huawei。登录启动设置只决定下次是否自动拉起，不改变当前会话。
    g_app.providerDesired = true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW

    const bool classesReady = commonControlsReady && RegisterClassExW(&wc);
    if (classesReady) {
        // WS_EX_NOACTIVATE keeps the panel from ever taking focus; TOOLWINDOW keeps it out
        // of Alt-Tab; TRANSPARENT lets clicks fall through to whatever is underneath.
        g_app.hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kWindowClass, L"EGoTouch Pen Status", WS_POPUP,
            0, 0, CardWidthDip() + kShadowPadDip * 2, CardHeightDip() + kShadowPadDip * 2,
            nullptr, nullptr, instance, nullptr);
    }

    // 启动的每一步都可能失败，但清理只有一条路径。早先 InitGraphics 失败时直接 return，漏掉
    // 了窗口、COM 和互斥句柄——三处清理各写一遍，迟早有一处对不上。
    const bool ready = g_app.hwnd != nullptr && InitGraphics();

    if (ready) {
        AddTrayIcon();
        g_app.reader.Open();      // may fail if the service is not up yet; poll retries
        // 第一次轮询自己就会建立充电边沿的基线，且因为当时还没有基线所以不会弹窗——已经在
        // 充电的笔不会在托盘启动的瞬间弹一个面板出来。这里曾经额外抄了一份 seeding，条件与
        // 轮询里的不一致，笔从未上报过充电状态时基线就建立不起来。
        PollChannel();
        SetTimer(g_app.hwnd, kTimerPoll, kPollIntervalMs, nullptr);
        SetTimer(g_app.hwnd, kTimerLease, kLeaseIntervalMs, nullptr);
        // 不等第一个计时器 tick：托盘已启动就立即请求接管，服务端仍会保证先启动 EGo runtime
        // 再禁用 HuaweiTHP。
        MaintainProviderLease();

        std::thread gestureWatcher(GestureWatcherThread);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        g_gestureStop.store(true, std::memory_order_relaxed);
        gestureWatcher.join();
    }

    // 正常退出时 WM_DESTROY 已经把这两样收掉并将 hwnd 置空；启动失败时它们还在。
    ReleaseTrayIcon();
    if (g_app.hwnd) DestroyWindow(g_app.hwnd);
    g_app.control.Close();
    ReleaseGraphics();
    CoUninitialize();
    if (singleton) CloseHandle(singleton);
    return ready ? 0 : 1;
}
