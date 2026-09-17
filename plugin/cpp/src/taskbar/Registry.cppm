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

    // 浅色主题判定（决定歌词条的键色/底色/字色，全项目唯一真相源）：
    // SystemUsesLightTheme（设置里的"Windows 模式"）优先——任务栏、开始菜单等
    // shell 表面跟随此键，歌词条画在任务栏上，必须按它取色；
    // AppsUseLightTheme（"应用模式"）只作用于应用窗口，与前者可在自定义模式下
    // 独立选择（实测用户注册表：AppsUseLightTheme=1 / SystemUsesLightTheme=0）。
    //
    // 修复记录（深色任务栏歌词"发灰、看不清"的根因）：旧实现两键弄反——
    // 优先 AppsUseLightTheme 且注释断言它是"任务栏实际跟随的键"。于是
    // "应用模式=浅色 + Windows 模式=深色"被误判为浅色：键色/底色取白(255)、
    // 字色取 THEME_LIGHT.primary(0xFF1A1A1A)。白色底色被 LWA_COLORKEY 挖空 →
    // 露出深色任务栏；近黑字芯在深色背景上不可见，只剩向白渐变的抗锯齿灰边，
    // 整行字呈现为低对比灰影（截图实测：字芯 94 / 边缘 175 正是 #1A1A1A 按
    // 覆盖率 0.70 / 0.35 混白的值）。
    // 读不到时回退 AppsUseLightTheme；两键均失败 → false（深色），保持既有兜底。
    static auto readDword(HKEY root, const wchar_t *path, const wchar_t *name, DWORD &out) -> bool {
        DWORD data = 0;
        DWORD size = sizeof(data);
        return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &data, &size) == ERROR_SUCCESS
            && (out = data, true);
    }

    static auto isLightTheme() -> bool {
        const auto path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
        DWORD data = 0;
        if (readDword(HKEY_CURRENT_USER, path, L"SystemUsesLightTheme", data)) {
            return data != 0;
        }
        if (readDword(HKEY_CURRENT_USER, path, L"AppsUseLightTheme", data)) {
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
