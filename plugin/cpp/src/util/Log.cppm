module;

#include <Windows.h>
#include <cstdio>   // swprintf_s
#include <mutex>
#include <string>

export module util.Log;

// 轻量日志（Phase 2 新增）：
//   - OutputDebugStringW 输出（DebugView 可见）
//   - 追加写 %TEMP%\taskbar-lyrics.log（UTF-8，带时间戳与 PID）
// 仅记录关键事件（启动/退出、窗口创建失败、管道连接/断开/重连、
// JSON 解析错误、pending 超限），避免高频调用（每行歌词不记录）。

export namespace Log {
    inline auto event(const std::wstring &message) -> void {
        SYSTEMTIME st{};
        GetLocalTime(&st);

        wchar_t timestamp[64]{};
        swprintf_s(
            timestamp,
            L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds
        );

        const auto line = std::wstring(timestamp) +
            L" [PID " + std::to_wstring(GetCurrentProcessId()) + L"] " +
            message + L"\n";

        // DebugView 可见（不落盘）
        OutputDebugStringW(line.c_str());

        // 追加写 %TEMP%\taskbar-lyrics.log（UTF-8；static 锁保证多线程
        // 调用不交错，管道线程与主线程都会打日志）
        static std::mutex fileMutex{};
        std::lock_guard<std::mutex> lock(fileMutex);

        wchar_t tempDir[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, tempDir) == 0) {
            return;
        }
        const auto path = std::wstring(tempDir) + L"taskbar-lyrics.log";
        const auto file = CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ, // 允许排查工具/编辑器并发读
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }

        const auto utf8Len = WideCharToMultiByte(
            CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr
        );
        if (utf8Len > 0) {
            std::string utf8(utf8Len, '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                utf8.data(), utf8Len, nullptr, nullptr
            );
            DWORD written = 0;
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
        CloseHandle(file);
    }
}
