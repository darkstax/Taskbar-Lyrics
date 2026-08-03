module;

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

export module pipe.LyricPipeServer;

import util.Log;


// 命名管道歌词服务（Server）
//
// go-musicfox 作为客户端连接本管道并按 JSON Lines 协议推送消息：
//   {"type":"lyric","primary":"当前行","secondary":"下一行","ts":...}
//   {"type":"config","config":{"window_alignment":"0","font_family":"...",...}}
// 本模块消费 type == "lyric"（歌词）与 type == "config"（配置）两种行，
// 其余 type 忽略。config 消息的值全部为字符串（协议冻结）。
//
// 线程模型（Phase 1 审查后收敛）：
//   - 管道线程：只解析 JSON 行、写入自己的 lastPrimary/lastSecondary 缓存
//     （由 lyricMutex 保护），然后 PostMessageW(目标窗口, WM_APP+1) 通知主线程，
//     不再直接写 config 或调用窗口回调（避免跨线程 UIAutomation/D2D 竞争）。
//   - 主线程：收到 WM_APP+1 后调用 pullLyric() 取缓存副本，写 config 并重绘。
//
// 客户端断开后自动重建管道等待重连。stop() 置 running=false 后 join 线程。
//
// 停止机制说明（Phase 1 审查后实测修正）：本环境（Win11）实测 CloseHandle
// 对阻塞中的 ConnectNamedPipe / ReadFile **不会唤醒**（阻塞 IO 永久悬挂 →
// join 死锁）。因此管道句柄全程使用 PIPE_NOWAIT 非阻塞模式：等待连接与
// 读取均为立即返回 + Sleep 轮询，线程从不阻塞在 IO 上；stop() 置位后
// 线程至多 50ms 内自行退出，join 不再依赖 CloseHandle 唤醒（无 UB 边界）。
// 竞态上：runLoop 写入 pipeHandle 后、任何阻塞调用前重查 running，
// stop() 恰在窗口期发生时新句柄由本线程自灭，杜绝 join 死锁。
export class LyricPipeServer {
private:
    static constexpr const wchar_t *PIPE_NAME = L"\\\\.\\pipe\\go-musicfox.lyric.v1";
    static constexpr DWORD PIPE_BUFFER_SIZE = 8192;
    // pending 缓冲上限：超过说明客户端发送了超长行（无 \n），丢弃并断开重连
    static constexpr size_t MAX_PENDING_BYTES = 64 * 1024;
    // 通知主线程"歌词已更新"的自定义消息（与 Window.cppm 的 WM_APP+1 case 对应）
    static constexpr UINT WM_LYRIC_UPDATED = WM_APP + 1;

    std::thread thread{};
    std::atomic<bool> running{false};
    std::mutex mutex{};          // 保护 pipeHandle（stop 与 runLoop 交互）
    HANDLE pipeHandle = INVALID_HANDLE_VALUE;

    HWND targetWindow = nullptr; // 主线程窗口句柄（构造时由 Plugin 传入，主线程创建后不再变）
    std::mutex lyricMutex{};     // 保护 lastPrimary/lastSecondary/lastConfig（管道线程写、主线程读）
    std::wstring lastPrimary{};
    std::wstring lastSecondary{};
    // config 消息缓存：键值均为 UTF-8 字符串（协议冻结），主线程拉取后逐条 setConfig。
    // 覆盖语义：同 key 新值覆盖旧值（配置变更以最后一次收到的为准）。
    std::vector<std::pair<std::string, std::string>> lastConfig{};

    static auto utf8ToWString(const std::string &str) -> std::wstring;
    auto parseLine(const std::string &line) -> void;
    auto runLoop() -> void;

public:
    explicit LyricPipeServer(const HWND targetWindow) : targetWindow(targetWindow) {}

    ~LyricPipeServer() {
        this->stop();
    }

    LyricPipeServer(const LyricPipeServer &) = delete;
    auto operator=(const LyricPipeServer &) -> LyricPipeServer & = delete;

    auto start() -> void;
    auto stop() -> void;

    // 主线程取走最新歌词缓存（返回拷贝；歌词是短字符串，拷贝成本可忽略）
    auto pullLyric() -> std::pair<std::wstring, std::wstring>;

    // 主线程取走最新 config 缓存（返回拷贝；config 消息不频繁，拷贝成本可忽略；
    // 无 config 消息时返回空 vector）
    auto pullConfig() -> std::vector<std::pair<std::string, std::string>>;
};

auto LyricPipeServer::start() -> void {
    if (this->running.exchange(true)) {
        return;
    }
    Log::event(L"管道服务启动，等待 go-musicfox 连接 " + std::wstring(PIPE_NAME));
    this->thread = std::thread([this] {
        // 管道线程入口：规范初始化 COM（当前路径未直接使用 COM，
        // 为将来扩展保留；线程退出时配对释放）
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        this->runLoop();
        CoUninitialize();
    });
}

auto LyricPipeServer::stop() -> void {
    if (!this->running.exchange(false)) {
        return;
    }
    // 不再主动 CloseHandle（Phase 2 收尾审查后删除）：管道句柄由 runLoop
    // 独占创建并在其所有退出路径自清（pipeHandle == pipe 检查 + 置 INVALID
    // + CloseHandle）。跨线程关闭存在 use-after-close 窗口期（stop 关闭后
    // runLoop 仍可能对旧句柄做非阻塞 ReadFile/ConnectNamedPipe，且该句柄
    // 可能已被系统复用），删除后该风险消除。
    // 线程退出由 running 标志驱动（非阻塞轮询 ≤50ms 醒来），join 等待线程
    // 结束即保证句柄已由 runLoop 关闭：无泄漏、无 double-close、无死锁。
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
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
            1,          // 单实例
            0,          // 出站缓冲
            PIPE_BUFFER_SIZE,
            0,          // 默认超时
            nullptr
        );
        if (pipe == INVALID_HANDLE_VALUE) {
            // 管道创建失败（如前实例残留未释放）：短等 200ms 后重试
            Log::event(L"管道创建失败 (error " + std::to_wstring(GetLastError()) + L")，200ms 后重试");
            Sleep(200);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(this->mutex);
            this->pipeHandle = pipe;
        }
        Log::event(L"管道已创建，等待客户端连接");

        // 竞态修复（Phase 1 审查·致命1）：句柄已写入、但 stop() 已置 running=false 时，
        // 本线程把句柄从槽位清掉并自行 CloseHandle，再退出循环。
        // （非阻塞模式下句柄此刻无挂起 IO，CloseHandle 安全）
        if (!this->running.load()) {
            {
                std::lock_guard<std::mutex> lock(this->mutex);
                if (this->pipeHandle == pipe) {
                    this->pipeHandle = INVALID_HANDLE_VALUE;
                }
            }
            CloseHandle(pipe);
            break;
        }

        // 非阻塞轮询等待 go-musicfox 连接（PIPE_NOWAIT：立即返回，
        // 由 running 标志驱动退出，不依赖 CloseHandle 唤醒）
        auto connected = false;
        while (this->running.load()) {
            if (ConnectNamedPipe(pipe, nullptr)) {
                connected = true;
                break;
            }
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                // 客户端已在等待接受
                connected = true;
                break;
            }
            if (error != ERROR_PIPE_LISTENING) {
                // 232 = ERROR_NO_DATA：客户端连接后立即断开与轮询窗口的竞态，
                // 单实例管道不能复用需重建，属正常路径，不记日志；
                // 其余非预期错误才记录。
                if (error != ERROR_NO_DATA) {
                    Log::event(L"ConnectNamedPipe 异常 (error " + std::to_wstring(error) + L")");
                }
                break;
            }
            Sleep(50);
        }
        if (connected) {
            Log::event(L"客户端已连接，进入读循环");
        } else {
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

        // 非阻塞读取字节流并按 \n 切行（PIPE_NOWAIT：无数据返回
        // ERROR_NO_DATA，Sleep 后重试；客户端断开返回 ERROR_BROKEN_PIPE）
        pending.clear();
        while (this->running.load()) {
            DWORD bytes = 0;
            const auto readOk = ReadFile(pipe, buffer, sizeof(buffer), &bytes, nullptr);
            if (!readOk) {
                const auto error = GetLastError();
                // NO_DATA 是 PIPE_NOWAIT 下的正常空读（客户端连接但暂无数据），
                // 高频轮询下不记日志避免刷屏；仅记录真实错误与断开探测结果。
                if (error != ERROR_NO_DATA) {
                    Log::event(L"ReadFile 失败 (error " + std::to_wstring(error) + L")");
                }
                if (error == ERROR_NO_DATA) {
                    // PIPE_NOWAIT 下客户端断开后 ReadFile 可能持续返回
                    // ERROR_NO_DATA 而不返回 ERROR_BROKEN_PIPE（本机实测：
                    // 客户端连接后未写数据即退出时，ReadFile 永远 NO_DATA，
                    // 单实例管道被永久占用，后续客户端全部连接失败）。
                    // 用 PeekNamedPipe 探测真实连接状态：客户端已断开时
                    // 其返回 FALSE（ERROR_BROKEN_PIPE），据此退出读循环重建。
                    DWORD available = 0;
                    DWORD total = 0;
                    DWORD remaining = 0;
                    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, &total)) {
                        Log::event(L"PeekNamedPipe 检测到客户端断开 (error " + std::to_wstring(GetLastError()) + L")");
                        break; // 客户端已断开
                    }
                    Sleep(20); // 暂无数据，稍后重试
                    continue;
                }
                break; // 客户端断开 / 句柄已被 stop() 关闭
            }
            if (bytes == 0) {
                break;
            }
            pending.append(buffer, bytes);
            // 上限保护：缓冲超过 64KB 说明收到超长行，丢弃并断开重连（复用外层断线路径）
            if (pending.size() > MAX_PENDING_BYTES) {
                Log::event(L"pending 缓冲超过上限 (64KB)，丢弃并断开重连");
                pending.clear();
                break;
            }
            size_t pos = 0;
            while ((pos = pending.find('\n')) != std::string::npos) {
                const auto line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                this->parseLine(line);
            }
        }

        // 客户端断开/出错：记录后回到循环顶部重建管道等待重连
        // （stop() 置位导致循环退出时不记录，避免停止场景打出误导性的"断开"日志）
        if (this->running.load()) {
            Log::event(L"客户端断开，重建管道等待重连");
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
    Log::event(L"管道服务线程退出");
}

auto LyricPipeServer::parseLine(const std::string &line) -> void {
    try {
        // 直接完整解析后按 type 字段分支（不再做对空白敏感的快速过滤）
        const auto json = nlohmann::json::parse(line);
        const auto type = json.value("type", std::string{});
        if (type == "lyric") {
            const auto primary = this->utf8ToWString(json.value("primary", std::string{}));
            const auto secondary = this->utf8ToWString(json.value("secondary", std::string{}));
            // 管道线程只更新自己的缓存（锁内短写），不碰 config / 窗口
            {
                std::lock_guard<std::mutex> lock(this->lyricMutex);
                this->lastPrimary = std::move(primary);
                this->lastSecondary = std::move(secondary);
            }
            // 通知主线程拉取缓存并刷新；PostMessage 可能合并多次，
            // WM_APP+1 处理时总是读最新缓存，天然幂等
            PostMessageW(this->targetWindow, WM_LYRIC_UPDATED, 0, 0);
        } else if (type == "config") {
            // config 消息：{"type":"config","config":{key: string, value: string, ...}}
            // 只消费 "config" 对象内的字符串键值对，其余键跳过（协议冻结，值全是字符串）。
            // 覆盖语义：同 key 新值覆盖旧值（配置变更以最后一次收到的为准）。
            const auto it = json.find("config");
            if (it == json.end() || !it->is_object()) {
                return;
            }
            std::vector<std::pair<std::string, std::string>> updates;
            for (auto kv = it->begin(); kv != it->end(); ++kv) {
                if (kv.value().is_string()) {
                    updates.emplace_back(kv.key(), kv.value().get<std::string>());
                }
            }
            if (updates.empty()) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(this->lyricMutex);
                for (auto &[key, value] : updates) {
                    const auto found = std::find_if(
                        this->lastConfig.begin(), this->lastConfig.end(),
                        [&key](const auto &kv) { return kv.first == key; }
                    );
                    if (found != this->lastConfig.end()) {
                        found->second = std::move(value);
                    } else {
                        this->lastConfig.emplace_back(std::move(key), std::move(value));
                    }
                }
            }
            // 与 lyric 共用 WM_APP+1 通知：主线程统一拉缓存、应用配置并刷新窗口
            PostMessageW(this->targetWindow, WM_LYRIC_UPDATED, 0, 0);
        }
        // 其他 type 忽略
    } catch (const std::exception &e) {
        // 忽略畸形 JSON 行，不中断管道读取；记录错误便于排查
        Log::event(L"JSON 解析错误，忽略畸形行：" + this->utf8ToWString(e.what()));
    } catch (...) {
        Log::event(L"JSON 解析错误，忽略畸形行");
    }
}

auto LyricPipeServer::pullLyric() -> std::pair<std::wstring, std::wstring> {
    std::lock_guard<std::mutex> lock(this->lyricMutex);
    return {this->lastPrimary, this->lastSecondary};
}

auto LyricPipeServer::pullConfig() -> std::vector<std::pair<std::string, std::string>> {
    std::lock_guard<std::mutex> lock(this->lyricMutex);
    return this->lastConfig;
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
