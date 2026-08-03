module;

#include <Windows.h>
#include <utility>

export module plugin.Plugin;

import plugin.Config;
import pipe.LyricPipeServer;
import window.Window;

// 应用单例：EXE 形态下由 main.cpp 在主线程调用 run()，
// 窗口创建与消息循环均运行在主线程（不再使用 detach 线程，避免窗口闪退）。
// 单实例互斥锁由 main.cpp 负责（Local\Taskbar-Lyrics）。
//
// 线程模型（Phase 1 审查后）：管道线程只解析歌词并 PostMessageW(WM_APP+1)
// 通知主线程；歌词写 config 与窗口重绘全部在主线程完成（跨线程收敛）。
export class Plugin {
public:
    Window *window = nullptr;
    LyricPipeServer *pipe = nullptr;

private:
    Plugin() = default;

    ~Plugin() {
        // 先停掉管道后台线程，确保不会再触发回调，再销毁窗口
        if (this->pipe) {
            delete this->pipe;
            this->pipe = nullptr;
        }
        if (this->window) {
            delete this->window;
            this->window = nullptr;
        }
    }

public:
    Plugin(const Plugin &) = delete;
    auto operator=(const Plugin &) -> Plugin & = delete;

    static auto getInstance() -> Plugin & {
        static Plugin instance;
        return instance;
    }

    // 必须在主线程调用：创建窗口 → 启动管道服务 → 运行消息循环（直到窗口销毁）。
    // 返回 0 正常退出；返回非 0 表示初始化失败（未进入消息循环）。
    auto run() -> int {
        this->window = new Window();
        if (!this->window->create()) {
            MessageBoxW(
                nullptr,
                L"任务栏歌词窗口创建失败。",
                L"Taskbar-Lyrics",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND
            );
            // 清理：析构 Window（内部 Taskbar 会 CoUninitialize）后返回错误码
            delete this->window;
            this->window = nullptr;
            return 1;
        }

        this->pipe = new LyricPipeServer(this->window->getHWND());
        // 主线程 WM_APP+1 处理：从管道取最新歌词缓存 → 写 config → 重绘
        this->window->setLyricSource([this] {
            if (this->pipe == nullptr) {
                return;
            }
            const auto [primary, secondary] = this->pipe->pullLyric();
            config.lyric_primary = std::move(primary);
            config.lyric_secondary = std::move(secondary);
            this->window->update();
        });
        this->pipe->start();

        this->window->runner();
        return 0;
    }
};
