module;

#include <Windows.h>
#include <functional>

export module window.Window;

import plugin.Config;
import taskbar.Taskbar;
import taskbar.Registry;
import window.Renderer;

// 歌词更新通知消息（LyricPipeServer 管道线程 PostMessageW 到本窗口）。
// 主线程收到后在 WM_APP+1 分支拉取歌词缓存、写 config 并重绘，
// 保证 config 写与 UI 读全部收敛在主线程。
//
// explorer 重启取舍（v1）：本窗口是 Shell_TrayWnd 的子窗口，explorer 重启
// 会销毁本窗口，WM_DESTROY → PostQuitMessage 干净退出；v1 不实现
// TaskbarCreated 消息监听与窗口重建，explorer 重启后由用户重新启动本工具。
export class Window {
private:
    HWND hwnd = nullptr;
    std::function<void()> lyricSource{};

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
                this->taskbar.setListener(std::bind(&Window::update, this));
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
                // 歌词更新通知（主线程）：拉取管道缓存 → 写 config → 重绘
                if (this->lyricSource) {
                    this->lyricSource();
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
            WS_EX_NOPARENTNOTIFY | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            class_name,
            nullptr,
            WS_CHILD | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            Taskbar::getHWND(),
            nullptr,
            dll_instance,
            this
        );
        if (this->hwnd == nullptr) {
            return false;
        }
        // 初始定位：任务栏结构变化事件可能较晚/不触发（冒烟实测窗口停留 0x0），
        // 主动 update 一次保证窗口立即可见（WM_CREATE 已同步完成 Taskbar 初始化）
        this->update();
        return true;
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

        const auto taskbarFrame = this->taskbar.getRectForTaskbarFrame();
        const auto trayFrameRect = this->taskbar.getRectForTrayFrame();
        const auto widgetsButtonRect = this->taskbar.getRectForWidgetsButton();
        const auto taskListRect = this->taskbar.getRectForTaskList();

        auto offset = 0L;
        auto width = 0L;
        auto height = 0L;

        switch (config.window_alignment) {
            case TASKBAR_WINDOW_ALIGNMENT::TASKBAR_WINDOW_ALIGNMENT_AUTO: [[fallthrough]];
            case TASKBAR_WINDOW_ALIGNMENT::TASKBAR_WINDOW_ALIGNMENT_LEFT: {
                if (Registry::isTaskbarCentered()) {
                    width += taskListRect.left;
                    if (Registry::isWidgetsEnabled()) {
                        offset += widgetsButtonRect.right;
                    }
                    break;
                }
                [[fallthrough]];
            }
            case TASKBAR_WINDOW_ALIGNMENT::TASKBAR_WINDOW_ALIGNMENT_RIGHT: {
                offset += taskListRect.right;
                if (Registry::isTaskbarCentered()) {
                    width += trayFrameRect.left;
                } else if (Registry::isWidgetsEnabled()) {
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

        BringWindowToTop(this->hwnd);
        MoveWindow(this->hwnd, offset, 0, width, height, false);
        RedrawWindow(this->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
};
