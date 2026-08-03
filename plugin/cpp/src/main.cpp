#include <Windows.h>
import plugin.Plugin;

// 独立 EXE 入口（/SUBSYSTEM:WINDOWS，wWinMain）
//
// 流程：
//   1. Local 命名空间互斥锁防多开（不使用 Global\，普通用户无需特权）
//   2. 创建任务栏歌词窗口（Plugin::run() 内部同步创建）
//   3. 主线程运行窗口消息循环 —— 窗口生命周期与主线程绑定，不会闪退
auto WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) -> int {
    const auto mutex = CreateMutexW(nullptr, TRUE, L"Local\\Taskbar-Lyrics");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(
            nullptr,
            L"Taskbar-Lyrics 已在运行。",
            L"Taskbar-Lyrics",
            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND
        );
        CloseHandle(mutex);
        return 0;
    }

    Plugin::getInstance().run();

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
