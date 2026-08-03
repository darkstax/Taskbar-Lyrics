export module plugin.Plugin;

import pipe.LyricPipeServer;
import window.Window;

// 应用单例：EXE 形态下由 main.cpp 在主线程调用 run()，
// 窗口创建与消息循环均运行在主线程（不再使用 detach 线程，避免窗口闪退）。
// 单实例互斥锁由 main.cpp 负责（Local\Taskbar-Lyrics）。
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

    // 必须在主线程调用：创建窗口 → 启动管道服务 → 运行消息循环（直到窗口销毁）
    auto run() -> void {
        this->window = new Window();
        this->window->create();
        this->pipe = new LyricPipeServer([this] {
            // 歌词更新 → 重新计算布局并重绘
            this->window->update();
        });
        this->pipe->start();
        this->window->runner();
    }
};
