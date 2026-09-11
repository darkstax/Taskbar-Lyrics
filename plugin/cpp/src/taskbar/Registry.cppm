module;

#include <Windows.h>
#include <vector>
#include <functional>

export module taskbar.Registry;

export class Registry {
public:
    typedef std::function<void()> Callback;

private:
    Callback callback{};

public:
    static auto onWatch(const Registry::Callback &callback) -> void {
        HKEY key = nullptr;
        HANDLE event = CreateEventW(nullptr, true, false, nullptr);
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion", 0, KEY_NOTIFY, &key);
        while (true) {
            RegNotifyChangeKeyValue(key, true, REG_NOTIFY_CHANGE_LAST_SET, event, true);
            WaitForSingleObject(event, INFINITE);
            ResetEvent(event);
            callback();
        }
    }

    // 浅色主题判定：AppsUseLightTheme（应用/任务栏主题，Win11 任务栏实际跟随
    // 的键）优先；读不到时回退 SystemUsesLightTheme（系统整体主题，即旧行为）。
    // 两键均读失败 → false（深色），与既有行为一致，避免回归。
    static auto readDword(HKEY root, const wchar_t *path, const wchar_t *name, DWORD &out) -> bool {
        DWORD data = 0;
        DWORD size = sizeof(data);
        return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &data, &size) == ERROR_SUCCESS
            && (out = data, true);
    }

    static auto isLightTheme() -> bool {
        const auto path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
        DWORD data = 0;
        if (readDword(HKEY_CURRENT_USER, path, L"AppsUseLightTheme", data)) {
            return data != 0;
        }
        if (readDword(HKEY_CURRENT_USER, path, L"SystemUsesLightTheme", data)) {
            return data != 0;
        }
        return false; // 读失败 → 深色兜底
    }

    static auto isTaskbarCentered() -> bool {
        const auto path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
        const auto name = L"TaskbarAl";
        DWORD data = 0;
        DWORD size = sizeof(data);
        RegGetValue(HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, nullptr, &data, &size);
        return data;
    }

    static auto isWidgetsEnabled() -> bool {
        const auto path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
        const auto name = L"TaskbarDa";
        DWORD data = 0;
        DWORD size = sizeof(data);
        RegGetValue(HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, nullptr, &data, &size);
        return data;
    }
};
