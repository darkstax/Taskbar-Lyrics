module;

#include <Windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>

export module pipe.LyricPipeServer;

import plugin.Config;


// 命名管道歌词服务（Server）
//
// go-musicfox 作为客户端连接本管道并按 JSON Lines 协议推送歌词：
//   {"type":"lyric","primary":"当前行","secondary":"下一行","ts":...}
// 本模块只消费 type == "lyric" 的行，更新全局 config 并触发回调。
// 客户端断开后自动重建管道等待重连；stop() 关闭当前句柄唤醒阻塞调用后 join 线程。
export class LyricPipeServer {
public:
    typedef std::function<void()> Callback;

private:
    static constexpr const wchar_t *PIPE_NAME = L"\\\\.\\pipe\\go-musicfox.lyric.v1";
    static constexpr DWORD PIPE_BUFFER_SIZE = 8192;

    std::thread thread{};
    std::atomic<bool> running{false};
    std::mutex mutex{};
    HANDLE pipeHandle = INVALID_HANDLE_VALUE;
    Callback onLyric{};

    static auto utf8ToWString(const std::string &str) -> std::wstring;
    auto parseLine(const std::string &line) -> void;
    auto runLoop() -> void;

public:
    explicit LyricPipeServer(const Callback &callback) : onLyric(callback) {}

    ~LyricPipeServer() {
        this->stop();
    }

    LyricPipeServer(const LyricPipeServer &) = delete;
    auto operator=(const LyricPipeServer &) -> LyricPipeServer & = delete;

    auto start() -> void;
    auto stop() -> void;
};

auto LyricPipeServer::start() -> void {
    if (this->running.exchange(true)) {
        return;
    }
    this->thread = std::thread([this] { this->runLoop(); });
}

auto LyricPipeServer::stop() -> void {
    if (!this->running.exchange(false)) {
        return;
    }
    // 关闭当前管道句柄，唤醒阻塞中的 ConnectNamedPipe / ReadFile
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->pipeHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(this->pipeHandle);
            this->pipeHandle = INVALID_HANDLE_VALUE;
        }
    }
    if (this->thread.joinable()) {
        this->thread.join();
    }
}

auto LyricPipeServer::runLoop() -> void {
    char buffer[PIPE_BUFFER_SIZE]{};
    std::string pending;

    while (this->running.load()) {
        const auto pipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,          // 单实例
            0,          // 出站缓冲
            PIPE_BUFFER_SIZE,
            0,          // 默认超时
            nullptr
        );
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(this->mutex);
            this->pipeHandle = pipe;
        }

        // 等待 go-musicfox 连接；若 stop() 在等待期间关闭了句柄则直接退出本轮
        const auto connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            auto closed = false;
            {
                std::lock_guard<std::mutex> lock(this->mutex);
                if (this->pipeHandle == pipe) {
                    this->pipeHandle = INVALID_HANDLE_VALUE;
                    closed = true;
                }
            }
            if (closed) {
                CloseHandle(pipe);
            }
            continue;
        }

        // 读取字节流并按 \n 切行
        pending.clear();
        while (this->running.load()) {
            DWORD bytes = 0;
            const auto ok = ReadFile(pipe, buffer, sizeof(buffer), &bytes, nullptr);
            if (!ok || bytes == 0) {
                break; // 客户端断开 / 句柄被 stop() 关闭
            }
            pending.append(buffer, bytes);
            size_t pos = 0;
            while ((pos = pending.find('\n')) != std::string::npos) {
                const auto line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                this->parseLine(line);
            }
        }

        {
            auto closed = false;
            std::lock_guard<std::mutex> lock(this->mutex);
            if (this->pipeHandle == pipe) {
                this->pipeHandle = INVALID_HANDLE_VALUE;
                closed = true;
            }
            if (closed) {
                CloseHandle(pipe);
            }
        }
        // 客户端断开后回到循环顶部重建管道，等待重连
    }
}

auto LyricPipeServer::parseLine(const std::string &line) -> void {
    if (line.find("\"type\":\"lyric\"") == std::string::npos) {
        return;
    }
    try {
        const auto json = nlohmann::json::parse(line);
        if (json.value("type", "") != "lyric") {
            return;
        }
        const auto primary = json.value("primary", std::string{});
        const auto secondary = json.value("secondary", std::string{});
        config.lyric_primary = this->utf8ToWString(primary);
        config.lyric_secondary = this->utf8ToWString(secondary);
        if (this->onLyric) {
            this->onLyric();
        }
    } catch (...) {
        // 忽略畸形 JSON 行，不中断管道读取
    }
}

auto LyricPipeServer::utf8ToWString(const std::string &str) -> std::wstring {
    if (str.empty()) {
        return {};
    }
    const auto size = MultiByteToWideChar(
        CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0
    );
    std::wstring result(size, L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, str.data(), static_cast<int>(str.size()), result.data(), size
    );
    return result;
}
