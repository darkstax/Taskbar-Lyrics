module;

#include <Windows.h>
#include <dwrite.h>
#include <string>
#include <unordered_map>
#include <charconv>
#include <algorithm>
#include <cctype>
#include <limits>

export module plugin.Config;

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

// 主题默认色（产品决策硬约束）：
// - 禁止纯黑 RGB(0,0,0) 字色：锁定态 LWA_COLORKEY 键色为精确 RGB(0,0,0)，
//   纯黑像素会被整块挖空导致文字消失（见 Renderer.cppm onCreate 的键色设置）。
// - 浅色主题 primary 0xFF1A1A1A / secondary 0xB31A1A1A（次级比主级浅一档，
//   与深色模式的层次一致）；深色主题保持全白 0xFFFFFFFF（完全维持现状观感）。
// - 占位色：深色 0xFF9E9E9E（现状）/ 浅色 0xFF767676。
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

// 用户显式色保护：RGB 精确 (0,0,0) 会被 COLORKEY 挖空（文字不可见），
// 偏移到 0x010101（肉眼不可辨）；alpha 不参与键色比较，保持原样。
inline auto sanitizeKeyColor(const unsigned int argb) -> unsigned int {
    if ((argb & 0x00FFFFFFu) == 0u) {
        return (argb & 0xFF000000u) | 0x00010101u;
    }
    return argb;
}

// 按当前主题重解析生效色。优先级：用户显式覆盖 > 主题跟随默认 > 冻结。
// - explicit=true：生效色 = 显式值（COLORKEY 保护后），主题切换不影响；
// - explicit=false 且 follow=true：生效色 = 当前主题默认；
// - explicit=false 且 follow=false：冻结，保持现有生效色不变（托盘取消勾选语义）。
// 只在主线程调用（config 写入全部收敛主线程的既有约定）。
export auto ResolveThemeColors(Config &cfg) -> void {
    if (cfg.colorPrimaryExplicit) {
        cfg.color_primary_active = sanitizeKeyColor(cfg.color_primary);
    } else if (cfg.color_theme_follow) {
        cfg.color_primary_active = cfg.color_theme_light ? THEME_LIGHT.primary : THEME_DARK.primary;
    }
    if (cfg.colorSecondaryExplicit) {
        cfg.color_secondary_active = sanitizeKeyColor(cfg.color_secondary);
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

// 颜色值解析（管道 config color_primary/color_secondary 语义）：
// - 合法数值（0xAARRGGBB / 0RRGGBB 按十六进制、其余按十进制，≤8 位十六进制）
//   → outExplicit=true，outColor=解析值（显式覆盖，切主题不受影响）；
// - 空串 / "theme" / "auto" → outExplicit=false（清除覆盖，回到跟随主题）；
// - 非法输入（含非法字符、溢出、超长）→ 返回 false，调用方保持当前值不变
//   （不崩、不误清空；替代原 std::stoul——畸形输入抛异常直接崩主线程）。
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
    std::transform(str.begin(), str.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "theme" || lower == "auto") {
        outExplicit = false;
        return true;
    }
    // 进制判定：0x/0X 前缀十六进制；0 开头（如 0RRGGBB 写法）也按十六进制；否则十进制
    bool hex = false;
    std::string digits = lower;
    if (digits.rfind("0x", 0) == 0) {
        hex = true;
        digits = digits.substr(2);
    } else if (digits.front() == '0' && digits.size() > 1) {
        hex = true;
        digits = digits.substr(1);
    }
    if (digits.empty() || digits.size() > 8) {
        return false; // 无数字或超长 → 非法，保持当前值
    }
    unsigned long long value = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
    if (ec != std::errc{} || ptr != digits.data() + digits.size() || value > 0xFFFFFFFFull) {
        return false; // 含非法字符 / 溢出 → 保持当前值
    }
    outColor = static_cast<unsigned int>(value);
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
            unsigned int c = 0;
            bool explicitFlag = config.colorPrimaryExplicit;
            if (ParseColorValue(str, c, explicitFlag)) {
                config.colorPrimaryExplicit = explicitFlag;
                if (explicitFlag) {
                    config.color_primary = c;
                }
                ResolveThemeColors(config);
            }
        }},
        {"underline_primary", [](const std::string &str) { config.underline_primary = (str == "true"); }},
        {"strikethrough_primary", [](const std::string &str) { config.strikethrough_primary = (str == "true"); }},
        {"weight_primary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.weight_primary = static_cast<DWRITE_FONT_WEIGHT>(v); }},
        {"slope_primary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.slope_primary = static_cast<DWRITE_FONT_STYLE>(v); }},
        {"align_primary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.align_primary = static_cast<DWRITE_TEXT_ALIGNMENT>(v); }},
        // 次要歌词设置
        {"size_secondary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.size_secondary = v; }},
        {"color_secondary", [](const std::string &str) {
            unsigned int c = 0;
            bool explicitFlag = config.colorSecondaryExplicit;
            if (ParseColorValue(str, c, explicitFlag)) {
                config.colorSecondaryExplicit = explicitFlag;
                if (explicitFlag) {
                    config.color_secondary = c;
                }
                ResolveThemeColors(config);
            }
        }},
        {"underline_secondary", [](const std::string &str) { config.underline_secondary = (str == "true"); }},
        {"strikethrough_secondary", [](const std::string &str) { config.strikethrough_secondary = (str == "true"); }},
        {"weight_secondary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.weight_secondary = static_cast<DWRITE_FONT_WEIGHT>(v); }},
        {"slope_secondary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.slope_secondary = static_cast<DWRITE_FONT_STYLE>(v); }},
        {"align_secondary", [](const std::string &str) { int v = 0; if (ParseIntValue(str, v)) config.align_secondary = static_cast<DWRITE_TEXT_ALIGNMENT>(v); }},
        // 主题跟随开关："true"/"1"/"auto"/"theme" 开启，"false"/"0" 关闭，其余忽略
        {"theme_follow", [](const std::string &str) {
            std::string lower(str.size(), '\0');
            std::transform(str.begin(), str.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
