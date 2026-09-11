module;

#include <Windows.h>
#define _WIN32_IE 0x0600 // SHSTOCKICONINFO/SIID_* 需要
#include <windowsx.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <shobjidl.h> // SHQueryUserNotificationState / QUERY_USER_NOTIFICATION_STATE
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <string>
#include <cwchar> // _wcsicmp（WM_SETTINGCHANGE 参数比较）

export module window.Window;

import plugin.Config;
import taskbar.Taskbar;
import taskbar.Registry;
import util.Log;
import window.Renderer;

// 歌词更新通知消息（LyricPipeServer 管道线程 PostMessageW 到本窗口）。
// 主线程收到后在 WM_APP+1 分支拉取歌词缓存、写 config 并重绘，
// 保证 config 写与 UI 读全部收敛在主线程。
//
// 布局机制（防 explorer 卡死）：
//   WM_APP+2 已废弃——结构变化/注册表回调只置布局脏标志并唤醒布局线程
//   （原子 + 条件变量，绝不碰 UIA/explorer），由独立布局线程执行全部
//   UIAutomation 跨进程查询（explorer 忙碌时可阻塞，无碍主线程），
//   测量结果经 WM_APP+3 回主线程做纯算术 + MoveWindow。主线程消息循环
//   永不阻塞 → 命中测试/输入始终响应 → 反复右键不会挂起桌面输入链。
//
// explorer 重启取舍（v1）：本窗口是 Shell_TrayWnd 的子窗口，explorer 重启
// 会销毁本窗口，WM_DESTROY → PostQuitMessage 干净退出；v1 不实现
// TaskbarCreated 消息监听与窗口重建，explorer 重启后由用户重新启动本工具。
export class Window {
private:
    HWND hwnd = nullptr;
    std::function<void()> lyricSource{};

    // 布局线程（独立测量 UIA，主线程永不阻塞）
    std::thread layoutThread{};
    std::mutex layoutMutex{};
    std::condition_variable layoutCv{};
    bool layoutDirty = false;
    bool layoutStop = false;
    Taskbar::TaskbarLayout lastLayout{};

    // 垂直任务栏模式（applyLayout 检测方向后设置；锁定/解锁只作用于垂直模式，
    // 水平模式的状态栏歌词永远保持透明+穿透+自动定位）与拖动状态
    bool verticalMode = false;
    bool dragging = false;
    POINT dragOffset{};

    // 全屏隐藏状态：fullscreen_ 由布局线程心跳写入（原子，主线程只读），
    // hidden_ 仅主线程在 applyLayout 中修改（记录当前是否因全屏而隐藏）
    std::atomic<bool> fullscreen_{false};
    bool hidden_ = false;

    // 锁定状态：锁定=透明底+点击穿透（默认）；解锁=半透明底+可拖动
    bool locked = true;

    // 最近一次已应用的主题快照（仅主线程读写；applyTheme 内部 diff 用，
    // 初值 false=深色，与 config 默认生效色一致）
    bool lastThemeLight = false;

    // 托盘图标回调消息（WM_APP+4）与菜单命令 ID
    static constexpr UINT WM_TRAY = WM_APP + 4;
    static constexpr UINT IDM_LOCK = 1;
    static constexpr UINT IDM_UNLOCK = 2;
    static constexpr UINT IDM_EXIT = 3;
    static constexpr UINT IDM_THEME_FOLLOW = 4;
    static constexpr UINT IDM_TRAY_COLORS = 5; // 解除托盘接管，恢复管道颜色配置生效

    static auto CALLBACK WindowProc(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) -> LRESULT {
        if (message == WM_CREATE) [[unlikely]] {
            const auto create = reinterpret_cast<LPCREATESTRUCT>(lParam);
            const auto window = static_cast<Window *>(create->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        }
        if (const auto that = reinterpret_cast<Window *>(GetWindowLongPtr(hwnd, GWLP_USERDATA))) [[likely]] {
            return that->handleMessage(hwnd, message, wParam, lParam);
        }
        return DefWindowProc(hwnd, message, wParam, lParam);
    }

    auto handleMessage(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam) -> LRESULT {
        switch (message) {
            case WM_CREATE: {
                this->hwnd = hwnd;
                this->taskbar.initialize();
                // 结构变化/注册表回调只置脏并唤醒布局线程（原子+条件变量，
                // 事件线程/UIA 线程/注册表线程均不碰 explorer，主线程零阻塞）。
                this->taskbar.setListener([this] {
                    this->update();
                });
                this->renderer.onCreate(hwnd);
                // 主题先行初始化（键色随主题：浅色=白键色，applyLayeredMode 必须
                // 在 color_theme_light 确定后调用，否则浅色启动时底色≠键色不透明）；
                // 审查 G3：经 applyLayeredMode 成对切换，不散写（解锁分支下方会再切 ALPHA）。
                // 主题跟随初始化：读持久化开关 → 按当前主题解析生效色 →
                // 记录快照（后续 WM_SETTINGCHANGE 与布局心跳 diff 兜底）
                config.color_theme_follow = this->loadThemeFollow();
                this->lastThemeLight = Registry::isLightTheme();
                // 审查修复 Y2/G3：先把检测到的主题同步进 config.color_theme_light
                // （否则占位色/解锁底色在首个心跳前仍按深色默认，浅色系统首帧白字闪一下），
                // 再解析生效色。
                config.color_theme_light = this->lastThemeLight;
                ResolveThemeColors(config);
                Log::event(this->lastThemeLight ? L"主题初始化：浅色" : L"主题初始化：深色");
                // 默认分层模式：锁定态 COLORKEY（键色随主题：深色黑/浅色白，与
                // Renderer::onPaint 的 Clear 色一致）
                this->applyLayeredMode(false);
                // 锁定状态与托盘图标
                this->locked = this->loadLocked();
                if (!this->locked) {
                    this->applyLayeredMode(true); // 解锁：LWA_ALPHA 半透明底，底色随主题
                }
                this->addTrayIcon();
                // 审查修复 Y2/G3：主题已在上方同步解析（color_theme_light +
                // ResolveThemeColors），立即绘制首帧，不依赖后续 WM_PAINT/心跳
                // 路径（消除“先深色默认再切浅色”的占位色闪烁窗口）。
                // 注：此刻布局线程尚未启动、快照必为空，几何仍由首个 WM_APP+3
                // 心跳应用；本帧仅保证“首帧即主题正确色”。
                this->renderer.onPaint();
                break;
            }
            case WM_SIZE: {
                const auto width = LOWORD(lParam);
                const auto height = HIWORD(lParam);
                const auto dpi = GetDpiForWindow(hwnd);
                this->renderer.onSize(width, height, dpi);
                break;
            }
            case WM_PAINT: {
                this->renderer.onPaint();
                ValidateRect(hwnd, nullptr);
                break;
            }
            case WM_APP + 1: {
                // 歌词更新通知（主线程）：拉取管道缓存 → 写 config → 直接重绘。
                // 注意：必须直接调用 renderer.onPaint() 而非依赖 RedrawWindow→
                // WM_PAINT——任务栏右键菜单模态会抑制 WM_PAINT 派发（实测
                // 菜单期间 paint 停止、关闭后恢复），直接绘制绕过该机制。
                if (this->lyricSource) {
                    this->lyricSource();
                }
                this->renderer.onPaint();
                break;
            }
            case WM_APP + 3: {
                // 布局测量完成（布局线程 PostMessage）：主线程应用快照（纯算术）
                std::lock_guard<std::mutex> lock(this->layoutMutex);
                this->applyLayout(this->lastLayout);
                break;
            }
            case WM_SETTINGCHANGE: {
                // 主题切换通知：参数串为 "ImmersiveColorSet"（大小写不敏感）时
                // 处理；lParam==0（部分广播不带参数）也做一次带 diff 的检查
                // （applyTheme 幂等，无变化不重绘）。只在主线程执行。
                if (lParam == 0) {
                    this->applyTheme();
                } else {
                    const auto *area = reinterpret_cast<const wchar_t *>(lParam);
                    if (_wcsicmp(area, L"ImmersiveColorSet") == 0) {
                        this->applyTheme();
                    }
                }
                break;
            }
            case WM_COMMAND: {
                // 菜单命令自动化入口：托盘菜单项处理同样响应外部 PostMessage
                // 的 WM_COMMAND（验证脚本 FindWindowW("taskbar_lyrics") 后发
                // IDM_* 模拟点击，比人工 TrackPopupMenu 可靠；同用户会话内
                // 本地进程可发消息，权限面与手动点菜单等价，不引入新攻击面）。
                const auto cmd = LOWORD(wParam);
                switch (cmd) {
                    case IDM_LOCK:
                    case IDM_UNLOCK:
                    case IDM_THEME_FOLLOW:
                    case IDM_TRAY_COLORS:
                    case IDM_EXIT:
                        this->handleTrayCommand(cmd);
                        break;
                    default: break;
                }
                break;
            }
            case WM_NCHITTEST: {
                // 锁定/水平模式：点击穿透；仅垂直模式解锁时可拖动
                return (this->verticalMode && !this->locked) ? HTCLIENT : HTTRANSPARENT;
            }
            case WM_LBUTTONDOWN: {
                // 仅垂直模式解锁状态拖动开始（记录偏移 + 捕获鼠标）
                if (this->verticalMode && !this->locked) {
                    this->dragging = true;
                    this->dragOffset.x = GET_X_LPARAM(lParam);
                    this->dragOffset.y = GET_Y_LPARAM(lParam);
                    SetCapture(hwnd);
                }
                break;
            }
            case WM_MOUSEMOVE: {
                if (this->dragging && (wParam & MK_LBUTTON)) {
                    RECT rc{};
                    GetWindowRect(hwnd, &rc);
                    const auto x = GET_X_LPARAM(lParam);
                    const auto y = GET_Y_LPARAM(lParam);
                    MoveWindow(
                        hwnd,
                        rc.left + x - this->dragOffset.x,
                        rc.top + y - this->dragOffset.y,
                        rc.right - rc.left,
                        rc.bottom - rc.top,
                        false
                    );
                }
                break;
            }
            case WM_LBUTTONUP: {
                if (this->dragging) {
                    this->dragging = false;
                    ReleaseCapture();
                    // 记忆拖动后的完整位置（注册表，布局心跳不再贴边）
                    RECT rc{};
                    GetWindowRect(hwnd, &rc);
                    this->saveVerticalPos(rc.left, rc.top);
                }
                break;
            }
            case WM_DESTROY: {
                // 窗口销毁 → 退出消息循环（explorer 重启等场景的干净退出路径）
                this->removeTrayIcon();
                PostQuitMessage(0);
                break;
            }
            case WM_TRAY: {
                // 托盘图标回调：右键弹菜单（锁定/解锁 · 跟随系统主题 · 退出）
                if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                    HMENU menu = CreatePopupMenu();
                    AppendMenuW(menu, MF_STRING | (this->locked ? MF_CHECKED : 0), IDM_LOCK, L"锁定歌词（透明+穿透）");
                    AppendMenuW(menu, MF_STRING | (this->locked ? 0 : MF_CHECKED), IDM_UNLOCK, L"解锁歌词（可拖动）");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING | (config.color_theme_follow ? MF_CHECKED : 0), IDM_THEME_FOLLOW, L"跟随系统主题（浅色/深色）");
                    // 托盘接管提示（Y1）：接管后管道颜色被忽略、可点击解除；
                    // 未接管时置灰（此时颜色由管道配置/主题跟随/托盘固化决定，
                    // 两种来源都可能在下一行歌词重放时生效，文案不声称“管道控制”）
                    AppendMenuW(menu, MF_STRING, IDM_TRAY_COLORS,
                                config.colorLockedByTray ? L"恢复管道颜色设置（解除托盘接管）" : L"恢复管道颜色设置（未被接管）");
                    EnableMenuItem(menu, IDM_TRAY_COLORS, MF_BYCOMMAND | (config.colorLockedByTray ? MF_ENABLED : MF_GRAYED));
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
                    POINT pt{};
                    GetCursorPos(&pt);
                    SetForegroundWindow(hwnd);
                    const auto cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
                    DestroyMenu(menu);
                    this->handleTrayCommand(cmd);
                } else if (lParam == WM_LBUTTONDBLCLK) {
                    // 双击托盘：切换锁定/解锁
                    this->setLocked(!this->locked);
                }
                break;
            }
            default: return DefWindowProc(hwnd, message, wParam, lParam);
        }
        return 0;
    }

    // 托盘菜单命令分发（TrackPopupMenu 返回值与外部 WM_COMMAND 共用）；仅主线程。
    auto handleTrayCommand(const UINT cmd) -> void {
        if (cmd == IDM_LOCK) {
            this->setLocked(true);
        } else if (cmd == IDM_UNLOCK) {
            this->setLocked(false);
        } else if (cmd == IDM_THEME_FOLLOW) {
            this->setThemeFollow(!config.color_theme_follow);
        } else if (cmd == IDM_TRAY_COLORS) {
            this->restorePipeColors();
        } else if (cmd == IDM_EXIT) {
            PostMessageW(this->hwnd, WM_CLOSE, 0, 0);
        }
    }

public:
    Renderer renderer{};
    Taskbar taskbar{};

    auto getHWND() const -> HWND {
        return this->hwnd;
    }

    // 锁定状态记忆（注册表 HKCU\Software\Taskbar-Lyrics）
    static auto loadLocked() -> bool {
        HKEY key = nullptr;
        DWORD value = 1; // 默认锁定
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Taskbar-Lyrics", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD size = sizeof(value);
            DWORD type = 0;
            if (RegQueryValueExW(key, L"Locked", nullptr, &type, reinterpret_cast<BYTE *>(&value), &size) != ERROR_SUCCESS) {
                value = 1;
            }
            RegCloseKey(key);
        }
        return value != 0;
    }

    static auto saveLocked(const bool locked) -> void {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Taskbar-Lyrics", 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            const auto value = locked ? 1UL : 0UL;
            RegSetValueExW(key, L"Locked", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
            RegCloseKey(key);
        }
    }

    // 主题跟随开关持久化（注册表 HKCU\Software\Taskbar-Lyrics\ThemeFollow，
    // REG_DWORD，仿 loadLocked/saveLocked；缺省=1 开启跟随）
    static auto loadThemeFollow() -> bool {
        HKEY key = nullptr;
        DWORD value = 1; // 默认跟随系统主题
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Taskbar-Lyrics", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD size = sizeof(value);
            DWORD type = 0;
            if (RegQueryValueExW(key, L"ThemeFollow", nullptr, &type, reinterpret_cast<BYTE *>(&value), &size) != ERROR_SUCCESS) {
                value = 1;
            }
            RegCloseKey(key);
        }
        return value != 0;
    }

    static auto saveThemeFollow(const bool follow) -> void {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Taskbar-Lyrics", 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            const auto value = follow ? 1UL : 0UL;
            RegSetValueExW(key, L"ThemeFollow", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
            RegCloseKey(key);
        }
    }

    // 分层模式成对切换（审查修复 G3）：SetLayeredWindowAttributes 与
    // renderer.setAlphaMode 必须同调同步，散写易漏——统一封装：
    // - alpha=true：解锁半透明底（LWA_ALPHA 210，底色随主题，无键色）。
    //   注（审查 Y3）：LWA_ALPHA 路径下 crKey 传 0，依赖系统契约——flags 不含
    //   LWA_COLORKEY 时 crKey 参数被忽略。
    // - alpha=false：锁定 COLORKEY 路径，键色随主题（深色黑/浅色白）——必须与
    //   Renderer::onPaint 的 Clear 色逐字节一致，否则底色无法变透明（整块实色盖住任务栏）。
    // 仅主线程调用（与窗口消息同线程）。
    auto applyLayeredMode(const bool alpha) -> void {
        if (this->hwnd == nullptr) {
            return;
        }
        if (alpha) {
            SetLayeredWindowAttributes(this->hwnd, 0 /* crKey 被忽略：flags 无 LWA_COLORKEY */, 210, LWA_ALPHA);
        } else {
            const auto keyRgb = themeKeyRgb(config.color_theme_light);
            SetLayeredWindowAttributes(
                this->hwnd,
                RGB((keyRgb >> 16) & 0xFF, (keyRgb >> 8) & 0xFF, keyRgb & 0xFF),
                0,
                LWA_COLORKEY);
        }
        this->renderer.setAlphaMode(alpha);
    }

    // 主题检测应用（仅主线程）：读当前浅色/深色 → 与上次比较，变化时重解析
    // 生效色并直接重绘（沿用 WM_APP+1 那套“直接 onPaint 绕过 WM_PAINT 抑制”
    // 纪律）。幂等：无变化时仅一次注册表读取+比较（微秒级），不重绘；
    // 可被广播/心跳两处安全重复调用（审查 Y4：修正“零开销”措辞）。
    auto applyTheme() -> void {
        const bool light = Registry::isLightTheme();
        const bool themeChanged = light != this->lastThemeLight;
        const bool changed = SetThemeColors(config, light);
        if (themeChanged) {
            this->lastThemeLight = light;
            // 键色随主题（深色黑/浅色白）：COLORKEY 路径必须同步重设，否则新底色
            // 不等于旧键色 → 底色不再透明（整块实色盖住任务栏）。
            this->applyLayeredMode(this->verticalMode && !this->locked);
            Log::event(light ? L"主题切换：浅色（已应用）" : L"主题切换：深色（已应用）");
        }
        // 生效色变化或主题位变化都重绘（占位色只看主题位，与歌词色独立）
        if ((changed || themeChanged) && this->hwnd != nullptr) {
            this->renderer.onPaint();
        }
    }

    // 托盘“跟随系统主题”开关（仅主线程）：
    // - 勾选=允许跟随：同时清除显式颜色覆盖（回到跟随主题）；
    // - 取消=保持当前色：把当前生效色固化为显式覆盖（否则取消后无值可用）。
    // 审查修复 Y1：两种操作都置“托盘接管锁”（colorLockedByTray）——此后管道
    // 全量重放的 color_primary/secondary 一律忽略，直到在托盘菜单“恢复管道
    // 颜色设置”手动解除（旧行为：下一行歌词就把 go 缓存的显式色重放回来，
    // 用户刚点的“跟随”被静默冲掉，属用户感知级缺陷）。锁不持久化：重启后
    // 管道配置重新生效（取舍写入 README：接管是会话级决定，避免永久掩盖
    // go 侧配置变更；持久化反而造成“换了配置没反应”的新困惑）。
    auto setThemeFollow(const bool follow) -> void {
        config.color_theme_follow = follow;
        if (follow) {
            config.colorPrimaryExplicit = false;
            config.colorSecondaryExplicit = false;
        } else {
            config.color_primary = config.color_primary_active;
            config.color_secondary = config.color_secondary_active;
            config.colorPrimaryExplicit = true;
            config.colorSecondaryExplicit = true;
        }
        config.colorLockedByTray = true;
        ++config.colorTrayLockGen; // 新一次接管：日志节流重新计一条
        this->saveThemeFollow(follow);
        const auto changed = SetThemeColors(config, Registry::isLightTheme());
        Log::event(follow ? L"托盘：开启跟随系统主题（已接管管道颜色）" : L"托盘：关闭跟随，保持当前色（已接管管道颜色）");
        if (changed) {
            this->renderer.onPaint();
        }
    }

    // 托盘“恢复管道颜色设置”（仅主线程）：解除接管锁，后续管道 config 颜色
    // 重新生效。
    // 解除后立即 PostMessage(WM_APP+1) 触发一次配置重放：go 侧的 config 只存于
    // C++ 管道缓存（go 仅在配置变化时下发），若不主动重放，恢复后要等到
    // 下一行歌词才生效（感知延迟）；锁未解除时 WM_APP+1 每行歌词本来就在
    // 重放，解除后补一次与其自然节奏一致，主线程内重入安全（lyricSource
    // 只读管道缓存 + 写 config + 幂等 update）。
    auto restorePipeColors() -> void {
        if (!config.colorLockedByTray) {
            return; // 幂等：未接管时无事发生
        }
        config.colorLockedByTray = false;
        ++config.colorTrayLockGen;
        Log::event(L"托盘：已解除颜色接管，恢复管道颜色配置生效（立即重放当前管道配置）");
        if (this->hwnd != nullptr) {
            PostMessageW(this->hwnd, WM_APP + 1, 0, 0); // 异步：菜单处理返回后主线程重放
        }
    }

    // 切换/应用锁定状态（仅影响垂直模式：锁定=透明+穿透+自动定位，
    // 解锁=半透明底+可拖动；水平模式始终为锁定行为）
    auto setLocked(const bool locked) -> void {
        this->locked = locked;
        this->saveLocked(locked);
        if (this->verticalMode) {
            if (locked) {
                this->applyLayeredMode(false); // 回到 COLORKEY 路径：键色/底色随主题
                this->update(); // 恢复自动定位（贴边/记忆位置）
            } else {
                this->applyLayeredMode(true); // 解锁底色随主题（深色黑/浅色白）
            }
            this->renderer.onPaint();
        }
    }

    // 托盘图标（右键菜单：锁定/解锁/退出）；图标用系统音乐图标
    auto addTrayIcon() -> void {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = this->hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY;
        wcscpy_s(nid.szTip, L"Taskbar-Lyrics");
        // 图标：优先系统音乐图标（部分环境不可用时退回应用图标）
        nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    auto removeTrayIcon() -> void {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = this->hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    // 装配歌词更新源（由 Plugin 注入：WM_APP+1 时从管道取缓存、写 config 并 update）
    auto setLyricSource(const std::function<void()> &callback) -> void {
        this->lyricSource = callback;
    }

    // 创建窗口；失败返回 false（不再进入消息循环，由调用方清理后退出）
    auto create() -> bool {
        const auto dll_instance = GetModuleHandle(nullptr);
        const auto class_name = L"taskbar_lyrics";
        const WNDCLASSEX wc{
            .cbSize = sizeof(WNDCLASSEX),
            .lpfnWndProc = Window::WindowProc,
            .hInstance = dll_instance,
            .lpszClassName = class_name,
        };
        // 窗口类注册失败（类名冲突等）直接失败返回，由调用方清理后退出
        if (!RegisterClassEx(&wc)) {
            return false;
        }
        this->hwnd = CreateWindowEx(
            // 独立顶层窗口（非任务栏子窗口）：右键菜单模态时 explorer 会暂停
            // 任务栏子窗口的 DComp 合成（歌词冻结、菜单关闭后跳变），
            // 顶层窗口不受影响；TOPMOST 保证不被任务栏遮挡，TOOLWINDOW 不进
            // 任务栏/Alt-Tab，NOACTIVATE+HTTRANSPARENT 保证点击穿透不抢焦点。
            // 注：GDI 渲染路径（HwndRenderTarget）不需要 NOREDIRECTIONBITMAP。
            // LayeredWindow：TOPMOST 保证不被任务栏遮挡，TOOLWINDOW 不进
            // 任务栏/Alt-Tab，NOACTIVATE+HTTRANSPARENT 保证点击穿透不抢焦点。
            // 注：不用 WS_EX_TRANSPARENT——它在本系统会导致命中测试穿透
            // （可拖动模式失效），穿透由 WM_NCHITTEST 返回 HTTRANSPARENT 实现。
            WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            class_name,
            nullptr,
            WS_POPUP | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            nullptr,
            nullptr,
            dll_instance,
            this
        );
        if (this->hwnd == nullptr) {
            return false;
        }
        // 启动布局线程（独立测量 UIA；首次置脏触发立即测量）
        this->layoutThread = std::thread([this] {
            this->layoutThreadLoop();
        });
        // 初始定位：任务栏结构变化事件可能较晚/不触发（冒烟实测窗口停留 0x0），
        // 主动 update 一次保证窗口立即可见（WM_CREATE 已同步完成 Taskbar 初始化）
        this->update();
        return true;
    }

    // 垂直任务栏模式下歌词条的拖动位置记忆（注册表 HKCU\Software\Taskbar-Lyrics）
    // 返回 true 且 x/y 有效时表示用户已自定义位置（布局心跳不再贴边）
    static auto loadVerticalPos(LONG &x, LONG &y) -> bool {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Taskbar-Lyrics", 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return false;
        }
        bool ok = false;
        DWORD type = 0;
        DWORD size = sizeof(x);
        if (RegQueryValueExW(key, L"VerticalPosX", nullptr, &type, reinterpret_cast<BYTE *>(&x), &size) == ERROR_SUCCESS &&
            RegQueryValueExW(key, L"VerticalPosY", nullptr, &type, reinterpret_cast<BYTE *>(&y), &size) == ERROR_SUCCESS) {
            ok = true;
        }
        RegCloseKey(key);
        return ok;
    }

    static auto saveVerticalPos(const LONG x, const LONG y) -> void {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Taskbar-Lyrics", 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(key, L"VerticalPosX", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&x), sizeof(x));
            RegSetValueExW(key, L"VerticalPosY", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&y), sizeof(y));
            RegCloseKey(key);
        }
    }

    // 停止布局线程（析构时调用；等待至多一次测量完成）
    ~Window() {
        {
            std::lock_guard<std::mutex> lock(this->layoutMutex);
            this->layoutStop = true;
        }
        this->layoutCv.notify_all();
        if (this->layoutThread.joinable()) {
            this->layoutThread.join();
        }
    }

    auto runner() -> void {
        MSG msg{};
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    auto update() -> void {
        if (this->hwnd == nullptr) [[unlikely]] {
            return;
        }
        // 仅置脏并唤醒布局线程（主线程零阻塞）；实际测量在线程内完成
        {
            std::lock_guard<std::mutex> lock(this->layoutMutex);
            this->layoutDirty = true;
        }
        this->layoutCv.notify_one();
    }

    // 全屏检测（布局线程心跳调用，微秒级）：任务栏被无边框全屏窗口覆盖时返回 true。
    // 首选系统状态查询 SHQueryUserNotificationState（D3D 独占/无边框全屏时系统通常
    // 报告 QUNS_RUNNING_D3D_FULL_SCREEN；headless 环境为 QUNS_NOT_PRESENT，视为非全屏）；
    // 未命中时几何法兜底：前台窗口矩形与显示器区域 rcMonitor 比较（±2px 容差）。
    // 关键：比较的是 rcMonitor（全屏区域）而非 rcWork（工作区），最大化窗口
    // （不盖任务栏）不会误判。
    auto detectFullscreen() -> bool {
        QUERY_USER_NOTIFICATION_STATE quns = QUNS_NOT_PRESENT;
        if (SUCCEEDED(SHQueryUserNotificationState(&quns)) && quns == QUNS_RUNNING_D3D_FULL_SCREEN) {
            return true;
        }
        // 几何法兜底
        const auto fg = GetForegroundWindow();
        if (fg == nullptr || fg == this->hwnd) {
            return false;
        }
        if (IsIconic(fg)) {
            return false;
        }
        // 排除桌面/任务栏自身（Shell_TrayWnd/Progman/WorkerW 覆盖全屏属正常）
        wchar_t className[64]{};
        if (GetClassNameW(fg, className, 64) == 0 ||
            wcscmp(className, L"Shell_TrayWnd") == 0 ||
            wcscmp(className, L"Progman") == 0 ||
            wcscmp(className, L"WorkerW") == 0) {
            return false;
        }
        MONITORINFO mi{.cbSize = sizeof(mi)};
        const auto monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &mi)) {
            return false;
        }
        RECT fr{};
        if (!GetWindowRect(fg, &fr)) {
            return false;
        }
        // 与 rcMonitor（全屏区域）比较，容差 ±2px
        constexpr LONG tolerance = 2;
        return fr.left <= mi.rcMonitor.left + tolerance &&
               fr.top <= mi.rcMonitor.top + tolerance &&
               fr.right >= mi.rcMonitor.right - tolerance &&
               fr.bottom >= mi.rcMonitor.bottom - tolerance;
    }

    // 布局线程：等待脏标志/定时心跳 → 独立测量 UIA → 缓存 → 通知主线程应用。
    // explorer 忙碌（如右键菜单模态）时测量可阻塞数秒，但只影响本线程。
    auto layoutThreadLoop() -> void {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        while (true) {
            {
                std::unique_lock<std::mutex> lock(this->layoutMutex);
                this->layoutCv.wait_for(lock, std::chrono::seconds(2), [this] {
                    return this->layoutDirty || this->layoutStop;
                });
                if (this->layoutStop) {
                    break;
                }
                this->layoutDirty = false;
            }
            // 全屏检测：前台无边框全屏（任务栏被覆盖）时置位，主线程 applyLayout
            // 据此刻隐藏/恢复歌词条。仅查询系统状态 + 前台窗口矩形，微秒级，
            // 布局线程侧调用不阻塞主线程。
            this->fullscreen_.store(this->detectFullscreen());
            const auto layout = Taskbar::measureLayout();
            {
                std::lock_guard<std::mutex> lock(this->layoutMutex);
                this->lastLayout = layout;
            }
            PostMessageW(this->hwnd, WM_APP + 3, 0, 0);
        }
        CoUninitialize();
    }

    // 主线程应用布局快照：纯算术 + MoveWindow/RedrawWindow（微秒级，不阻塞）
    auto applyLayout(const Taskbar::TaskbarLayout &layout) -> void {
        if (this->hwnd == nullptr) [[unlikely]] {
            return;
        }
        // 主题兜底：布局快照携带主题位（锁屏/唤醒错过 WM_SETTINGCHANGE 广播时，
        // 2s 心跳内 diff 收敛）；applyTheme 幂等，无变化时仅一次注册表读取+
        // 比较（审查 Y4：非“零开销”，但不重绘不重解色）。隐藏分支前也要应用，
        // 保证恢复可见时颜色已就位。
        this->applyTheme();
        // 全屏隐藏/恢复：全屏且未隐藏 → SW_HIDE 并提前退出（跳过定位/重绘）；
        // 退出全屏且已隐藏 → SW_SHOWNOACTIVATE 后继续正常流程（MoveWindow 归位
        // + 末尾 onPaint 重绘）。可见性只在主线程修改（布局线程仅写 atomic 标志）。
        if (this->fullscreen_.load()) {
            if (!this->hidden_) {
                ShowWindow(this->hwnd, SW_HIDE);
                this->hidden_ = true;
            }
            return;
        }
        if (this->hidden_) {
            ShowWindow(this->hwnd, SW_SHOWNOACTIVATE);
            this->hidden_ = false;
        }
        const auto &taskbarFrame = layout.frame;

        // 垂直任务栏适配（Windows 11 26300+ 原生支持任务栏在左/右/上）：
        // 垂直任务栏（高>宽）没有水平空间放两行歌词，歌词条贴在任务栏内侧
        // 的屏幕边缘（横排两行，保持可读性）。检测方向后走独立布局。
        const auto frameW = taskbarFrame.right - taskbarFrame.left;
        const auto frameH = taskbarFrame.bottom - taskbarFrame.top;
        if (frameH > frameW && frameW > 0 && frameH > 0) {
            this->verticalMode = true;
            // 垂直模式：按锁定状态应用分层属性（锁定=透明，解锁=半透明可拖）
            if (this->locked) {
                this->applyLayeredMode(false); // COLORKEY 路径，底色恒黑
            } else {
                this->applyLayeredMode(true); // 解锁底色随主题
                // 解锁：位置自由，布局心跳不干预定位
                this->renderer.onPaint();
                return;
            }
            // 拖动中不干预位置（布局心跳 2s 一次，拖动期间跳过）
            if (this->dragging) {
                return;
            }
            // 垂直任务栏：歌词条默认贴任务栏内侧（方案A），可拖动、位置记忆。
            const auto screenW = GetSystemMetrics(SM_CXSCREEN);
            const auto screenH = GetSystemMetrics(SM_CYSCREEN);
            const auto lyricW = 540L;
            const auto lyricH = 40L;
            const auto gap = 4L; // 与任务栏的间距
            // 垂直位置：用户拖动的记忆位置优先（完整 x/y，自由模式）；
            // 无记忆时默认贴任务栏内侧、屏幕顶部下方 150px（避开顶部 dock 栏）
            LONG savedX = 0;
            LONG savedY = 0;
            const auto hasSaved = this->loadVerticalPos(savedX, savedY);
            LONG posX = 0;
            LONG posY = 150L;
            if (hasSaved) {
                posX = savedX;
                posY = savedY;
            } else {
                if (taskbarFrame.left >= screenW / 2) {
                    posX = taskbarFrame.left - lyricW - gap; // 任务栏在右侧：贴左
                } else {
                    posX = taskbarFrame.right + gap; // 任务栏在左侧：贴右
                }
            }
            const auto w = min(lyricW, max(0L, screenW - posX - gap));
            MoveWindow(this->hwnd, posX, posY, w, lyricH, false);
            RedrawWindow(this->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            this->renderer.onPaint();
            return;
        }

        // 水平模式：恢复颜色键透明（点击穿透不挡任务栏）；COLORKEY 路径，
        // 解锁底色标记同步关闭（永远透明+穿透+自动定位，不受锁定状态影响）
        this->applyLayeredMode(false);
        this->verticalMode = false;
        const auto &trayFrameRect = layout.tray;
        const auto &widgetsButtonRect = layout.widgets;
        const auto &taskListRect = layout.taskList;

        auto offset = 0L;
        auto width = 0L;
        auto height = 0L;

        switch (config.window_alignment) {
            case TASKBAR_WINDOW_ALIGNMENT::TASKBAR_WINDOW_ALIGNMENT_AUTO: [[fallthrough]];
            case TASKBAR_WINDOW_ALIGNMENT::TASKBAR_WINDOW_ALIGNMENT_LEFT: {
                if (layout.centered) {
                    width += taskListRect.left;
                    if (layout.widgetsEnabled) {
                        offset += widgetsButtonRect.right;
                    }
                    break;
                }
                [[fallthrough]];
            }
            case TASKBAR_WINDOW_ALIGNMENT::TASKBAR_WINDOW_ALIGNMENT_RIGHT: {
                offset += taskListRect.right;
                if (layout.centered) {
                    width += trayFrameRect.left;
                } else if (layout.widgetsEnabled) {
                    width += widgetsButtonRect.left;
                } else {
                    width += trayFrameRect.left;
                }
                break;
            }
            case TASKBAR_WINDOW_ALIGNMENT::TASKBAR_WINDOW_ALIGNMENT_CENTER: {
                offset += taskbarFrame.left;
                width += taskbarFrame.right;
                break;
            }
        }

        offset += config.margin_left;
        width -= config.margin_right + offset;
        height += taskbarFrame.bottom - taskbarFrame.top;
        // 顶层窗口使用绝对屏幕坐标（任务栏顶部）；子窗口时代 y=0 相对任务栏
        const auto posY = taskbarFrame.top;

        BringWindowToTop(this->hwnd);
        MoveWindow(this->hwnd, offset, posY, width, height, false);
        RedrawWindow(this->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        // LayeredWindow 首次显示与位置变化后需要主动提交位图（WM_PAINT 不保证触发）
        this->renderer.onPaint();
    }
};
