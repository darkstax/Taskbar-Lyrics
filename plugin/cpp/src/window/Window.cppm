module;

#include <Windows.h>
#include <windowsx.h>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>

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

    // 垂直任务栏模式（applyLayout 检测方向后设置；WM_NCHITTEST 据此切换
    // 点击穿透/可拖动）与拖动状态
    bool verticalMode = false;
    bool dragging = false;
    POINT dragOffset{};

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
            case WM_NCHITTEST: {
                // 水平模式：点击穿透（鼠标事件落回任务栏）；
                // 垂直模式：可拖动（歌词条在任务栏外侧，拦截鼠标用于拖动）
                return this->verticalMode ? HTCLIENT : HTTRANSPARENT;
            }
            case WM_LBUTTONDOWN: {
                // 垂直模式拖动开始（记录偏移 + 捕获鼠标）
                if (this->verticalMode) {
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
                PostQuitMessage(0);
                break;
            }
            default: return DefWindowProc(hwnd, message, wParam, lParam);
        }
        return 0;
    }

public:
    Renderer renderer{};
    Taskbar taskbar{};

    auto getHWND() const -> HWND {
        return this->hwnd;
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
        const auto &taskbarFrame = layout.frame;

        // 垂直任务栏适配（Windows 11 26300+ 原生支持任务栏在左/右/上）：
        // 垂直任务栏（高>宽）没有水平空间放两行歌词，歌词条贴在任务栏内侧
        // 的屏幕边缘（横排两行，保持可读性）。检测方向后走独立布局。
        const auto frameW = taskbarFrame.right - taskbarFrame.left;
        const auto frameH = taskbarFrame.bottom - taskbarFrame.top;
        if (frameH > frameW && frameW > 0 && frameH > 0) {
            this->verticalMode = true;
            // 垂直模式：LWA_ALPHA 整窗半透明（无颜色键穿透，整窗可拖动）
            SetLayeredWindowAttributes(this->hwnd, 0, 210, LWA_ALPHA);
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

        this->verticalMode = false;
        // 水平模式：恢复颜色键透明（点击穿透不挡任务栏）
        SetLayeredWindowAttributes(this->hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
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
