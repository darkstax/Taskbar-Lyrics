module;

#include <Windows.h>
#include <dwrite.h>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <charconv>
#include <algorithm>
#include <cctype>
#include <limits>

export module plugin.Config;

import util.Log;

auto stringToWString(const std::string &str) {
    std::wstring result(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.c_str(), -1, nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.c_str(), -1, result.data(), result.size());
    return result;
}

export enum TASKBAR_WINDOW_ALIGNMENT {
    TASKBAR_WINDOW_ALIGNMENT_AUTO,
    TASKBAR_WINDOW_ALIGNMENT_LEFT,
    TASKBAR_WINDOW_ALIGNMENT_CENTER,
    TASKBAR_WINDOW_ALIGNMENT_RIGHT
};

// 主题默认色（产品决策）：
// - 浅色主题 primary 0xFF1A1A1A / secondary 0xB31A1A1A（次级比主级浅一档，
//   与深色模式的层次一致）；深色主题保持全白 0xFFFFFFFF（维持既有观感）。
// - 占位色：深色 0xFF9E9E9E / 浅色 0xFF767676。
// 注：这里只决定**字色/占位色/解锁底衬色**——窗口背景是真正的逐像素 alpha 透明
// （见 window.Renderer），不存在"键色"，因此不再有"字色禁止等于底色"这类约束。
export struct ThemeColors {
    unsigned int primary;
    unsigned int secondary;
    unsigned int placeholder;
};
export constexpr ThemeColors THEME_DARK{0xFFFFFFFF, 0xFFFFFFFF, 0xFF9E9E9E};
export constexpr ThemeColors THEME_LIGHT{0xFF1A1A1A, 0xB31A1A1A, 0xFF767676};

export struct Config {
    // 歌词内容（默认空：Lyrics::onDraw 在 empty 时绘制占位文本）
    std::wstring lyric_primary = L"";
    std::wstring lyric_secondary = L"";
    // 通用设置
    std::wstring font_family = L"Microsoft YaHei UI";
    int margin_left = 0;
    int margin_right = 0;
    TASKBAR_WINDOW_ALIGNMENT window_alignment = TASKBAR_WINDOW_ALIGNMENT::TASKBAR_WINDOW_ALIGNMENT_AUTO;
    // 主题跟随状态（均可由托盘菜单/管道 config 修改；持久化于注册表 ThemeFollow）
    // color_theme_follow = 允许跟随 Windows 浅色/深色主题（默认开启）；
    // color_theme_light  = 最近一次检测到的主题（占位色按主题取值，主线程更新）。
    bool color_theme_follow = true;
    bool color_theme_light = false;
    // 主要歌词设置
    // color_primary = 用户显式指定值（explicit 标记是否存在覆盖）；
    // color_primary_active = 每帧实际生效色（显式覆盖 > 主题默认），Lyrics 只读 active。
    // 默认值保持深色主题白色，观感与旧版一致；启动时按当前主题刷新。
    unsigned int color_primary = 0xFFFFFFFF;
    unsigned int color_primary_active = 0xFFFFFFFF;
    bool colorPrimaryExplicit = false;
    // 托盘接管锁（审查修复 Y1）：用户每次在托盘操作颜色相关菜单项后置 true——
    // 此后管道 config 的颜色 key（color_primary/color_secondary）一律忽略，
    // 防止 go 侧全量重放的显式色把托盘意图冲掉（WM_APP+1 每行歌词重放一次）。
    // 解除：托盘菜单“恢复管道颜色”，或重启本工具（锁不持久化：与 ThemeFollow
    // 开关区分——接管是会话级临时决定，重启后管道配置重新生效，取舍已写入 README）。
    bool colorLockedByTray = false;
    // 接管代数：每次托盘接管/解除递增，供日志节流用（每次接管恰好记一条）。
    unsigned int colorTrayLockGen = 0;
    unsigned int colorTrayLogGen = 0;
    int size_primary = 14;
    bool underline_primary = false;
    bool strikethrough_primary = false;
    DWRITE_FONT_WEIGHT weight_primary = DWRITE_FONT_WEIGHT::DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE slope_primary = DWRITE_FONT_STYLE::DWRITE_FONT_STYLE_NORMAL;
    DWRITE_TEXT_ALIGNMENT align_primary = DWRITE_TEXT_ALIGNMENT::DWRITE_TEXT_ALIGNMENT_LEADING;
    // 次要歌词设置
    unsigned int color_secondary = 0xFFFFFFFF;
    unsigned int color_secondary_active = 0xFFFFFFFF;
    bool colorSecondaryExplicit = false;
    int size_secondary = 14;
    bool underline_secondary = false;
    bool strikethrough_secondary = false;
    DWRITE_FONT_WEIGHT weight_secondary = DWRITE_FONT_WEIGHT::DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE slope_secondary = DWRITE_FONT_STYLE::DWRITE_FONT_STYLE_NORMAL;
    DWRITE_TEXT_ALIGNMENT align_secondary = DWRITE_TEXT_ALIGNMENT::DWRITE_TEXT_ALIGNMENT_LEADING;
} config;

// 按当前主题重解析生效色。优先级：用户显式覆盖 > 主题跟随默认 > 冻结。
// - explicit=true：生效色 = 显式值，主题切换不影响；
// - explicit=false 且 follow=true：生效色 = 当前主题默认；
// - explicit=false 且 follow=false：冻结，保持现有生效色不变（托盘取消勾选语义）。
// 只在主线程调用（config 写入全部收敛主线程的既有约定）。
// 注：历史上有 sanitizeKeyColor 把"恰好等于颜色键"的字色偏移一档以逃过 LWA_COLORKEY
// 挖空；现在窗口是逐像素 alpha 透明、不存在键色，显式色按原值生效（含纯黑/纯白）。
export auto ResolveThemeColors(Config &cfg) -> void {
    if (cfg.colorPrimaryExplicit) {
        cfg.color_primary_active = cfg.color_primary;
    } else if (cfg.color_theme_follow) {
        cfg.color_primary_active = cfg.color_theme_light ? THEME_LIGHT.primary : THEME_DARK.primary;
    }
    if (cfg.colorSecondaryExplicit) {
        cfg.color_secondary_active = cfg.color_secondary;
    } else if (cfg.color_theme_follow) {
        cfg.color_secondary_active = cfg.color_theme_light ? THEME_LIGHT.secondary : THEME_DARK.secondary;
    }
}

// 主题切换应用（读到新 light 值后调用，主线程）：更新主题位并重新解析生效色。
// 返回生效色是否发生变化（调用方据此决定是否重绘）。
export auto SetThemeColors(Config &cfg, const bool light) -> bool {
    const auto oldPrimary = cfg.color_primary_active;
    const auto oldSecondary = cfg.color_secondary_active;
    cfg.color_theme_light = light;
    ResolveThemeColors(cfg);
    return cfg.color_primary_active != oldPrimary || cfg.color_secondary_active != oldSecondary;
}

// 颜色值解析（管道 config color_primary/color_secondary 语义，审查修复 R1）：
// 格式与 README 承诺严格一致——只接受十六进制，不再接受任意十进制：
// - 可选 0x/0X 前缀 + 恰好 8 位十六进制 = AARRGGBB（如 0xFFFFFFFF、FFFFFFFF、
//   FF4FC3F7；README 承诺的两种写法均支持）；
// - 可选前缀 + 恰好 6 位十六进制 = RRGGBB，自动补 FF alpha（如 4FC3F7 →
//   0xFF4FC3F7；旧行为 0x 前缀不补→alpha=0 全透明不可见，属陷阱，已统一修正）；
// - 空串 / "theme" / "auto"（大小写不敏感，含首尾空白）→ outExplicit=false
//   （清除覆盖，回到跟随主题）；
// - 其余一律返回 false（其它长度的纯数字如 "12345"、"4FC3F7890"、非法字符、
//   溢出）→ 调用方保持当前值不变（不崩、不误清空；替代原 std::stoul——
//   畸形输入抛异常直接崩主线程）。与 go-musicfox 侧校验正则
//   ^(0[xX])?[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?$ 完全对齐。
export auto ParseColorValue(const std::string &input, unsigned int &outColor, bool &outExplicit) -> bool {
    auto str = input;
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), notSpace));
    str.erase(std::find_if(str.rbegin(), str.rend(), notSpace).base(), str.end());
    if (str.empty()) {
        outExplicit = false; // 空 = 回到跟随主题
        return true;
    }
    std::string lower(str.size(), '\0');
    std::transform(str.begin(), str.end(), lower.begin(), [](unsigned char c) { return static_cast<unsigned char>(std::tolower(c)); });
    if (lower == "theme" || lower == "auto") {
        outExplicit = false;
        return true;
    }
    // 进制判定：仅十六进制。0x 前缀允许 1~8 位；裸写法只允许恰好 6 位（RGB）
    // 或 8 位（ARGB），消除旧“0 开头按十六进制、其余按十进制”路径的歧义：
    // FFFFFFFF 曾被当十进制溢出静默忽略、FF4FC3F7 被当十进制解析出错误值、
    // 9 位纯数字（4FC3F7890）被当合法十进制接受。
    std::string digits = lower;
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits = digits.substr(2); // 手写判定避开 Windows.h 的 min/max 宏污染
    }
    // 裸写法与 0x 写法统一：只允许恰好 6 位（RRGGBB，补 FF alpha）或
    // 8 位（AARRGGBB），其余长度（含纯十进制 1~9 位歧义串）一律非法。
    if (digits.size() != 6 && digits.size() != 8) {
        return false; // 无数字或长度非法 → 保持当前值
    }
    unsigned long long value = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (ec != std::errc{} || ptr != digits.data() + digits.size() || value > 0xFFFFFFFFull) {
        return false; // 含非法字符 / 溢出 → 保持当前值
    }
    auto argb = static_cast<unsigned int>(value);
    // 6 位写法视为 RRGGBB，补不透明 alpha（旧行为 0x 前缀不补 → alpha=0
    // 全透明不符合直觉；裸/前缀两写法统一补 FF）
    if (digits.size() == 6) {
        argb |= 0xFF000000u;
    }
    outColor = argb;
    outExplicit = true;
    return true;
}

// 数值输入安全解析：非法/溢出/含尾随垃圾 → 返回 false，调用方保持当前值
// （替代原 std::stoi——畸形输入抛异常崩主线程）。导出以便单元验证。
export template <typename T>
auto ParseIntValue(const std::string &str, T &out) -> bool {
    if (str.empty() || str.size() > 16) {
        return false;
    }
    long long value = 0;
    const auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value, 10);
    if (ec != std::errc{} || ptr != str.data() + str.size()) {
        return false;
    }
    // Windows.h 定义了 min/max 宏（本项目未开 NOMINMAX，Window.cppm 依赖），
    // 此处用括号形式抑制宏展开：(std::numeric_limits<T>::min)()
    if (value < static_cast<long long>((std::numeric_limits<T>::min)()) ||
        value > static_cast<long long>((std::numeric_limits<T>::max)())) {
        return false;
    }
    out = static_cast<T>(value);
    return true;
}

// 非法颜色输入日志去重（仅主线程读写：config 写入全部收敛主线程）：
// WM_APP+1 每行歌词全量重放 pullConfig，非法值会被反复喂进来，同一 key
// 的同一原文只记一次避免刷屏；审查修复 N1：旧实现是全局单槽且任意合法
// 颜色应用后整槽清空，而 go 侧每次重放恒带两个颜色 key，primary 非法+
// secondary 合法时槽被配对 key 清掉，退化成每行歌词刷一条——改为 per-key
// 槽，仅同 key 收到合法值（或清除串）时清该 key 的槽，“再次变非法”仍能记一条。
inline auto shouldLogBadColor(const std::string &key, const std::string &value) -> bool {
    static std::unordered_map<std::string, std::string> lastBad{};
    auto it = lastBad.find(key);
    if (it != lastBad.end() && it->second == value) {
        return false;
    }
    lastBad[key] = value;
    return true;
}
inline auto resetBadColorLog(const std::string &key) -> void {
    shouldLogBadColor(key, std::string{}); // 借去重槽写入空串，重置该 key 的记录
}

// 颜色配置已被托盘接管日志去重代数（随接管/解除递增，不持久化：重启后
// 进程内 static 自然重置，与新会话首次接管语义一致）。

// 颜色 key 统一入口（仅主线程，由 setConfig 分发表调用）：
// - 托盘接管锁（Y1）：colorLockedByTray=true 时忽略管道颜色 key（合法/非法
//   两路径都拦，防止重放把托盘设定的显式/跟随状态冲掉），每次接管只记一次；
// - 解析成功 → 更新 explicit 标记与显式值，重解生效色；
// - 解析失败 → 保持当前值 + Log::event 警告（颜色值无敏感性，可记原文）。
inline auto applyColorConfig(const std::string &key, const std::string &value) -> void {
    if (config.colorLockedByTray) {
        if (config.colorTrayLockGen != config.colorTrayLogGen) {
            config.colorTrayLogGen = config.colorTrayLockGen;
            Log::event(L"颜色配置已被托盘接管，忽略管道颜色值（托盘菜单“恢复管道颜色”可解除）: " +
                       stringToWString(key) + L"=" + stringToWString(value));
        }
        return;
    }
    unsigned int c = 0;
    bool explicitFlag = config.colorPrimaryExplicit;
    unsigned int *colorSlot = nullptr;
    bool *explicitSlot = nullptr;
    if (key == "color_primary") {
        colorSlot = &config.color_primary;
        explicitSlot = &config.colorPrimaryExplicit;
        explicitFlag = config.colorPrimaryExplicit;
    } else {
        colorSlot = &config.color_secondary;
        explicitSlot = &config.colorSecondaryExplicit;
        explicitFlag = config.colorSecondaryExplicit;
    }
    if (!ParseColorValue(value, c, explicitFlag)) {
        if (shouldLogBadColor(key, value)) {
            Log::event(L"非法颜色配置值，已忽略并保持当前值: " + stringToWString(key) + L"=" + stringToWString(value));
        }
        return;
    }
    resetBadColorLog(key);
    const auto oldRaw = *colorSlot;
    const auto oldExplicit = *explicitSlot;
    const auto oldActive = (colorSlot == &config.color_primary)
                               ? config.color_primary_active
                               : config.color_secondary_active;
    *explicitSlot = explicitFlag;
    if (explicitFlag) {
        *colorSlot = c;
    }
    ResolveThemeColors(config);
    // 解析与应用全链路可观测（审查 R1 真机验证需要）：仅在值/状态真正变化时
    // 记一条（WM_APP+1 每行歌词全量重放，同值不刷屏），便于 grep 日志核对
    // wire 输入 → 解析 → active 生效值；颜色值无敏感性，原文可记。
    const auto newActive = (colorSlot == &config.color_primary)
                               ? config.color_primary_active
                               : config.color_secondary_active;
    if (oldRaw != *colorSlot || oldExplicit != *explicitSlot || oldActive != newActive) {
        wchar_t buf[160]{};
        swprintf_s(buf, L"颜色配置生效: %hs -> 解析 %s (explicit=%d, active=0x%08X)",
                   key.c_str(),
                   explicitFlag ? L"显式覆盖" : L"清除覆盖→跟随主题",
                   explicitFlag ? 1 : 0, newActive);
        Log::event(std::wstring(buf) + L" [输入 " + stringToWString(value) + L"]");
    }
}

// 测试辅助：复位全局 config 单例到默认值（仅供 tests/ 单元测试用，
// 产品代码不调用；Windows 下各字段默认值均合法，无 OS 交互）。
export auto ResetConfigForTest() -> void {
    config = Config{};
}

export auto setConfig(const std::string &key, const std::string &value) {
    static const auto setters = std::unordered_map<std::string, void(*)(const std::string &)>{
        // 歌词内容
        {"lyric_primary", [](const std::string &str) { config.lyric_primary = stringToWString(str); }},
        {"lyric_secondary", [](const std::string &str) { config.lyric_secondary = stringToWString(str); }},
        // 通用设置（数值非法 → 忽略并保持当前值，不抛异常不崩溃）
        {"font_family", [](const std::string &str) { config.font_family = stringToWString(str); }},
        {"margin_left", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.margin_left = v; }},
        {"margin_right", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.margin_right = v; }},
        {"window_alignment", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v) && v >= TASKBAR_WINDOW_ALIGNMENT_AUTO && v <= TASKBAR_WINDOW_ALIGNMENT_RIGHT) config.window_alignment = static_cast<TASKBAR_WINDOW_ALIGNMENT>(v); }},
        // 主要歌词设置（颜色语义：空串/theme/auto = 回到跟随主题；非法 = 保持当前值）
        {"size_primary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.size_primary = v; }},
        {"color_primary", [](const std::string &str) {
            applyColorConfig("color_primary", str);
        }},
        {"underline_primary", [](const std::string &str) { config.underline_primary = (str == "true"); }},
        {"strikethrough_primary", [](const std::string &str) { config.strikethrough_primary = (str == "true"); }},
        {"weight_primary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.weight_primary = static_cast<DWRITE_FONT_WEIGHT>(v); }},
        {"slope_primary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.slope_primary = static_cast<DWRITE_FONT_STYLE>(v); }},
        {"align_primary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.align_primary = static_cast<DWRITE_TEXT_ALIGNMENT>(v); }},
        // 次要歌词设置
        {"size_secondary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.size_secondary = v; }},
        {"color_secondary", [](const std::string &str) {
            applyColorConfig("color_secondary", str);
        }},
        {"underline_secondary", [](const std::string &str) { config.underline_secondary = (str == "true"); }},
        {"strikethrough_secondary", [](const std::string &str) { config.strikethrough_secondary = (str == "true"); }},
        {"weight_secondary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.weight_secondary = static_cast<DWRITE_FONT_WEIGHT>(v); }},
        {"slope_secondary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.slope_secondary = static_cast<DWRITE_FONT_STYLE>(v); }},
        {"align_secondary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.align_secondary = static_cast<DWRITE_TEXT_ALIGNMENT>(v); }},
        // 主题跟随开关（高级：go-musicfox 无下发路径，供第三方管道客户端使用，
        // 见 README“高级配置”节；本工具内由托盘菜单操作）：
        // "true"/"1"/"auto"/"theme" 开启，"false"/"0" 关闭，其余忽略
        {"theme_follow", [](const std::string &str) {
            std::string lower(str.size(), '\0');
            std::transform(str.begin(), str.end(), lower.begin(), [](unsigned char c) { return static_cast<unsigned char>(std::tolower(c)); });
            if (lower == "true" || lower == "1" || lower == "auto" || lower == "theme") {
                config.color_theme_follow = true;
            } else if (lower == "false" || lower == "0") {
                config.color_theme_follow = false;
            }
            ResolveThemeColors(config); // 开启时立即恢复跟随，关闭时冻结当前值（幂等）
        }},
    };
    if (const auto it = setters.find(key); it != setters.end()) {
        it->second(value);
    }
}
