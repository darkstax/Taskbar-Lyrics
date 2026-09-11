// test_config.cpp — Config.cppm 纯逻辑单元测试（审查修复 F1：测试入库为交付物）
//
// 构建：cmake -DTL_BUILD_TESTS=ON → ctest 或直接运行 test_config.exe。
// 默认不纳入发布构建（option(TL_BUILD_TESTS OFF)），避免发布包混入测试 exe。
//
// 覆盖矩阵（与上一轮审查清单对应）：
//   R1  ParseColorValue：0x 前缀 / 8 位裸 hex / 6 位补 alpha / 非法输入拒绝 /
//       空串与 theme/auto 清除覆盖 / 纯十进制不再接受
//   R1  sanitizeKeyColor：纯黑偏移（0xFF000000→0xFF010101、0x80000000→alpha 保留）
//   主题 ResolveThemeColors / SetThemeColors：follow+explicit 四态转移、冻结语义
//   Y1  托盘接管：lockedByTray 后管道重放显式色/清除串均被忽略；解除后恢复生效
//   R1  非法输入保持当前值（经 setConfig 全链路）
//   加固 ParseIntValue：合法/非法/越界/尾随垃圾/超长
#include <cstdio>
#include <string>

import plugin.Config;
import util.Log;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) {                                                        \
            ++g_pass;                                                      \
        } else {                                                           \
            ++g_fail;                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(actual, expect)                                                              \
    do {                                                                                      \
        const auto _a = (actual);                                                             \
        const auto _e = (expect);                                                             \
        if (_a == _e) {                                                                       \
            ++g_pass;                                                                         \
        } else {                                                                              \
            ++g_fail;                                                                         \
            std::printf("FAIL %s:%d: %s == %s (got 0x%08X, want 0x%08X)\n", __FILE__,         \
                        __LINE__, #actual, #expect, static_cast<unsigned int>(_a),             \
                        static_cast<unsigned int>(_e));                                       \
        }                                                                                     \
    } while (0)

namespace {

// ---- ParseColorValue：十六进制格式矩阵（R1）----
auto test_parse_color() -> void {
    unsigned int c = 0;
    bool ex = false;

    // 0x 前缀 8 位 = AARRGGBB
    CHECK(ParseColorValue("0xFFFFFFFF", c, ex) && ex);
    CHECK_EQ(c, 0xFFFFFFFFu);
    CHECK(ParseColorValue("0xff000000", c, ex) && ex);
    CHECK_EQ(c, 0xFF000000u);

    // 8 位裸 hex = AARRGGBB（README 教写法：FFFFFFFF / FF4FC3F7 必须被正确接受）
    CHECK(ParseColorValue("FFFFFFFF", c, ex) && ex);
    CHECK_EQ(c, 0xFFFFFFFFu);
    CHECK(ParseColorValue("FF4FC3F7", c, ex) && ex);
    CHECK_EQ(c, 0xFF4FC3F7u);

    // 6 位 = RRGGBB，自动补 FF alpha（含旧"0 开头按十六进制"陷阱回归）
    CHECK(ParseColorValue("4FC3F7", c, ex) && ex);
    CHECK_EQ(c, 0xFF4FC3F7u);
    CHECK(ParseColorValue("0x1A2B3C", c, ex) && ex); // 旧行为 alpha=0 不可见，现在补 FF
    CHECK_EQ(c, 0xFF1A2B3Cu);
    CHECK(ParseColorValue("000000", c, ex) && ex); // 旧陷阱：0 开头被当 0x000000
    CHECK_EQ(c, 0xFF000000u);

    // 空串 / theme / auto（大小写不敏感、含空白）= 清除覆盖
    CHECK(ParseColorValue("", c, ex) && !ex);
    CHECK(ParseColorValue("  ", c, ex) && !ex);
    CHECK(ParseColorValue("theme", c, ex) && !ex);
    CHECK(ParseColorValue("AUTO", c, ex) && !ex);

    // 非法输入：纯十进制（任意长度）、其它长度、非法字符、9 位
    CHECK(!ParseColorValue("12345", c, ex));       // 旧行为：合法十进制 → 现拒绝
    CHECK(!ParseColorValue("4FC3F7890", c, ex));   // 旧行为：9 位十进制被接受 → 现拒绝
    CHECK(!ParseColorValue("abc", c, ex));         // 3 位非法
    CHECK(!ParseColorValue("GGGGGG", c, ex));      // 非 hex 字符
    CHECK(!ParseColorValue("12345", c, ex));
    CHECK(!ParseColorValue("0x", c, ex));          // 前缀后无数字
    CHECK(!ParseColorValue("0x12345", c, ex));     // 5 位
    CHECK(!ParseColorValue("0xFFFFFFFFF", c, ex)); // 9 位 hex 溢出
    CHECK(!ParseColorValue("0x1122334455", c, ex));
    CHECK(!ParseColorValue("12 3456", c, ex));     // 内嵌空白
    CHECK(!ParseColorValue("0XZZZZZZ", c, ex));
}

// ---- 纯黑 COLORKEY 保护（sanitizeKeyColor）----
auto test_sanitize() -> void {
    CHECK_EQ(sanitizeKeyColor(0xFF000000u), 0xFF010101u); // alpha 保留，RGB 偏移
    CHECK_EQ(sanitizeKeyColor(0x80000000u), 0x80010101u);
    CHECK_EQ(sanitizeKeyColor(0x00000000u), 0x00010101u);
    CHECK_EQ(sanitizeKeyColor(0xFF000001u), 0xFF000001u); // 非纯黑不动
    CHECK_EQ(sanitizeKeyColor(0xFFFFFFFFu), 0xFFFFFFFFu);
}

// ---- follow + explicit 四态转移（ResolveThemeColors / SetThemeColors）----
auto test_theme_states() -> void {
    ResetConfigForTest(); // 默认：follow=true, light=false, 无显式覆盖

    // 态1：follow + 深色 → 深色默认白
    ResolveThemeColors(config);
    CHECK_EQ(config.color_primary_active, THEME_DARK.primary);
    CHECK_EQ(config.color_secondary_active, THEME_DARK.secondary);

    // 态2：follow + 浅色 → 浅色默认（SetThemeColors 返回变化标记）
    CHECK(SetThemeColors(config, true));
    CHECK_EQ(config.color_primary_active, THEME_LIGHT.primary);
    CHECK_EQ(config.color_secondary_active, THEME_LIGHT.secondary);

    // 态3：follow=false 取消跟随 → 冻结当前生效色（不重解）
    config.color_theme_follow = false;
    const auto frozen = config.color_primary_active;
    ResolveThemeColors(config);
    CHECK_EQ(config.color_primary_active, frozen);

    // 态4：explicit 覆盖优先于主题：固化后切深色主题不变
    config.colorPrimaryExplicit = true; // color_primary 保持固化值
    CHECK(SetThemeColors(config, false));
    CHECK_EQ(config.color_primary_active, sanitizeKeyColor(config.color_primary));

    // explicit 纯黑保护：固化纯黑 → 生效色偏移至 0x010101 级
    config.color_primary = 0xFF000000u;
    ResolveThemeColors(config);
    CHECK_EQ(config.color_primary_active, 0xFF010101u);

    // 清除 explicit 且 follow=true → 回到主题默认（深色白）
    config.colorPrimaryExplicit = false;
    config.color_theme_follow = true;
    ResolveThemeColors(config);
    CHECK_EQ(config.color_primary_active, THEME_DARK.primary);
}

// ---- setConfig 全链路：非法输入保持当前值（R1）----
auto test_set_config_colors() -> void {
    ResetConfigForTest();

    setConfig("color_primary", "FF4FC3F7"); // 8 位裸 hex 生效
    CHECK(config.colorPrimaryExplicit);
    CHECK_EQ(config.color_primary_active, 0xFF4FC3F7u);

    setConfig("color_primary", "abc"); // 非法 → 保持当前值
    CHECK_EQ(config.color_primary_active, 0xFF4FC3F7u);
    CHECK(config.colorPrimaryExplicit);

    setConfig("color_primary", "12345"); // 纯十进制 → 拒绝并保持
    CHECK_EQ(config.color_primary, 0xFF4FC3F7u);

    setConfig("color_primary", ""); // 空串 → 清除覆盖，回到跟随主题
    CHECK(!config.colorPrimaryExplicit);
    CHECK_EQ(config.color_primary_active, config.color_theme_light ? THEME_LIGHT.primary : THEME_DARK.primary);

    setConfig("color_secondary", "0xFF000000"); // 显式纯黑 → 偏移保护
    CHECK_EQ(config.color_secondary_active, 0xFF010101u);

    setConfig("theme_follow", "false"); // 关闭跟随 → 冻结
    CHECK(!config.color_theme_follow);
    setConfig("theme_follow", "bogus"); // 非法值忽略
    CHECK(!config.color_theme_follow);
    setConfig("theme_follow", "TRUE"); // 大小写不敏感
    CHECK(config.color_theme_follow);
}

// ---- Y1：托盘接管后管道颜色重放被忽略 ----
auto test_tray_takeover() -> void {
    ResetConfigForTest();

    // 模拟 go 侧配了显式色且已被应用
    setConfig("color_primary", "FFFF0000");
    CHECK_EQ(config.color_primary_active, 0xFFFF0000u);

    // 用户托盘勾选"跟随"：清显式覆盖 + 上锁（模拟 setThemeFollow(true) 的 config 侧效果）
    config.color_theme_follow = true;
    config.colorPrimaryExplicit = false;
    config.colorSecondaryExplicit = false;
    config.colorLockedByTray = true;
    ++config.colorTrayLockGen;
    ResolveThemeColors(config);
    const auto followed = config.color_primary_active;

    // WM_APP+1 全量重放：go 缓存的显式色回灌 → 必须被忽略（旧缺陷：冲掉跟随）
    setConfig("color_primary", "FFFF0000");
    setConfig("color_secondary", "FFFF0000");
    CHECK_EQ(config.color_primary_active, followed);
    CHECK(!config.colorPrimaryExplicit);

    // 重放的清除串同样被忽略（不改变已跟随状态，也无副作用）
    setConfig("color_primary", "");
    CHECK_EQ(config.color_primary_active, followed);

    // 非法值在锁定下同样静默（不记非法日志路径，直接接管拦截）
    setConfig("color_primary", "not-a-color");
    CHECK_EQ(config.color_primary_active, followed);

    // 解除接管（模拟 restorePipeColors）→ 管道重放恢复生效
    config.colorLockedByTray = false;
    ++config.colorTrayLockGen;
    setConfig("color_primary", "FF4FC3F7");
    CHECK_EQ(config.color_primary_active, 0xFF4FC3F7u);
    CHECK(config.colorPrimaryExplicit);
}

// ---- ParseIntValue：合法/非法/越界/尾随垃圾 ----
auto test_parse_int() -> void {
    int v = 0;
    CHECK(ParseIntValue("42", v) && v == 42);
    CHECK(ParseIntValue("-7", v) && v == -7);
    CHECK(ParseIntValue("0", v) && v == 0);
    v = 99;
    CHECK(!ParseIntValue("", v) && v == 99);           // 空串
    CHECK(!ParseIntValue("abc", v) && v == 99);        // 非数字
    CHECK(!ParseIntValue("12abc", v) && v == 99);      // 尾随垃圾
    CHECK(!ParseIntValue(" 12", v) && v == 99);        // 前导空白（from_chars 不接受）
    CHECK(!ParseIntValue("99999999999999999999", v));  // 超 long long 范围（且 >16 字符守卫）
    CHECK(!ParseIntValue("2147483648", v));            // int 上溢
    CHECK(!ParseIntValue("-2147483649", v));           // int 下溢
    CHECK(ParseIntValue("2147483647", v) && v == 2147483647); // 边界内
    short s = 0;
    CHECK(!ParseIntValue("40000", s));                 // short 越界
    CHECK(ParseIntValue("300", s) && s == 300);
}

} // namespace

auto main() -> int {
    test_parse_color();
    test_sanitize();
    test_theme_states();
    test_set_config_colors();
    test_tray_takeover();
    test_parse_int();
    std::printf("test_config: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
