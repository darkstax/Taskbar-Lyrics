# Taskbar-Lyrics — 项目技术文档

> Windows 11 任务栏实时歌词工具:独立 EXE 在任务栏上显示 go-musicfox 的当前播放歌词(含翻译);
> 原为 BetterNCM 插件,已移除插件外壳与 JS 层,改为独立顶层窗口应用。

## 1. 技术栈

| 类别 | 内容 |
|------|------|
| 语言 | C++20(C++ Modules,`.cppm` 模块;`/utf-8 /D_UNICODE /DUNICODE`) |
| 构建 | CMake 3.30+(`CXX_MODULES FILE_SET`)+ Visual Studio 2022(MSVC,生成器 `Visual Studio 17 2022`)+ CMakePresets |
| 依赖 | 仅 Windows 库:d2d1、dwrite、shell32(user32/gdi32 由 MSVC 默认链接集提供);第三方仅 `third_party/nlohmann/json.hpp`(单头) |
| 平台 | Windows 专用(Win11 优先;PerMonitorV2 DPI) |
| 通信 | 命名管道 `\\.\pipe\go-musicfox.lyric.v1`(JSON Lines,**本工具为服务端**,go-musicfox 为客户端) |

## 2. 架构概览

```
main.cpp(wWinMain)→ Plugin::getInstance().run()(互斥锁 Local\Taskbar-Lyrics 防多开)→ 主线程消息循环
├── LyricPipeServer(管道线程)    # 只解析 JSON 写入缓存 → PostMessageW(WM_APP+1) 通知主线程
├── 布局线程(UIAutomation)       # 每 2s 心跳 + 结构变化事件测量任务栏 → WM_APP+3 回主线程
└── 主线程                      # 歌词写入/窗口重绘全部收敛于此(永不阻塞)
        ├── Window.cppm         # 顶层透明窗口、布局、全屏隐藏
        ├── Renderer.cppm       # 离屏 32bpp 预乘 alpha DIB + UpdateLayeredWindow 逐像素透明
        └── Lyrics.cppm         # DirectWrite 文本布局、字号双向贴合(下限 0.6/上限 4.0)
```

- 消息类型:`lyric`(primary/secondary 歌词)、`config`(窗口配置,全字符串;颜色 key 恒下发,空串=跟随主题);`meta`/`state` 预留。
- **线程纪律**:UIAutomation/D2D 全部在主线程之外或主线程内串行;管道线程只写缓存,绝不碰 UI(避免 explorer 卡死/右键菜单清屏卡死)。
- 关键类:`Plugin`(单例)、`Window`、`Renderer`、`Lyrics`、`LyricPipeServer`、`Taskbar`、`Handler`(UIA 事件)、`Registry`、`Log`。

## 3. 目录结构

```
Taskbar-Lyrics/
├── README.md / IMPLEMENTATION_PLAN.md / LICENSE(MIT)
├── scripts/
│   ├── install.ps1    # 安装到 %LOCALAPPDATA%\Programs\Taskbar-Lyrics + HKCU 开机自启(-NoAutostart 跳过)
│   ├── run.ps1        # 启动器:解除 MOTW(Zone.Identifier)后启动(-NoStart 只洗属性)
│   └── uninstall.ps1  # 卸载:停进程 + 删自启 + 删目录
└── plugin/cpp/
    ├── CMakeLists.txt / CMakePresets.json   # x64/x86 × release/debug
    ├── third_party/nlohmann/json.hpp        # 唯一第三方依赖
    ├── src/
    │   ├── main.cpp / app.manifest          # 入口 + 应用清单(DPI/兼容)
    │   ├── pipe/    LyricPipeServer.cppm
    │   ├── plugin/  Plugin.cppm / Config.cppm
    │   ├── taskbar/ Taskbar.cppm / Handler.cppm / Registry.cppm
    │   ├── util/    Log.cppm
    │   └── window/  Window.cppm / Renderer.cppm / Lyrics.cppm
    └── build/                               # 构建产物(未跟踪,不入库)
```

## 4. 构建与测试

```bash
cd plugin/cpp
cmake --preset x64-release
cmake --build --preset x64-release
# 产物:build/x64-release/Release/taskbar-lyrics.exe
```

- 需要 VS2022 的 C++20 Modules 支持与 CMake 3.30+。
- **无自动化测试**:验证靠人工端到端清单(IMPLEMENTATION_PLAN.md Task 5)与截图(历史 `build/phase3/smoke/`);调试日志经 `util/Log.cppm`(OutputDebugString + `%TEMP%\taskbar-lyrics.log`)。

## 5. 运行与部署

- 两种模式:安装模式(`scripts/install.ps1`,go-musicfox 配 `taskbarPipe=true` 时自动拉起)/便携模式(直接运行 exe)。
- 集成:先启动 `taskbar-lyrics.exe`,再播放 go-musicfox(其 `[main.lyric]` 配置 `taskbarPipe/taskbarAlignment/taskbarFontFamily/taskbarFontSizePrimary/Secondary`)。
- go-musicfox 侧改动在 go-musicfox 仓库(`internal/lyric/pipe_writer.go` + `internal/ui/player.go`,交叉编译 `-tags "enable_global_hotkey purego"`)。
- MOTW:经 WSL/网络拷贝的 exe 带 `Zone.Identifier` 会触发 SmartScreen,先解除再运行(推荐 `run.ps1`)。

## 6. 关键约束

- **窗口样式**:独立顶层窗口(非任务栏子窗口,避免右键菜单模态冻结合成),组合 `WS_EX_NOPARENTNOTIFY|WS_EX_NOACTIVATE|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED`。
- **点击穿透**:用 `WM_NCHITTEST → HTTRANSPARENT`;**禁止用 `WS_EX_TRANSPARENT`**(实测导致命中穿透、可拖动失效);水平模式永远穿透。
- **逐像素透明**(2026-09 重构,取代原 LWA_COLORKEY 颜色键方案):`ID2D1DCRenderTarget` 画到 top-down 32bpp 预乘 alpha DIB(`CreateDIBSection`, `DXGI_FORMAT_B8G8R8A8_UNORM`+`ALPHA_MODE_PREMULTIPLIED`),再用 `UpdateLayeredWindow(ULW_ALPHA, {AC_SRC_OVER,0,255,AC_SRC_ALPHA})` 提交;背景恒 `Clear(0,0,0,0)` 真透明,字形抗锯齿自然混向透明、由 DWM 与真实任务栏合成。保留灰度抗锯齿。**不要加 `WS_EX_NOREDIRECTIONBITMAP`**(会走 DComp 直排,与 ULW 冲突)。
  - 为什么废弃颜色键:键色透明要求"底色与键色逐字节相同""字色禁止等于键色",且边缘只能向键色渐变——键色与真实背景不一致就出光晕/灰边,主题判错时字芯会压在深色背景上不可见(实测"歌词发灰")。逐像素 alpha 下内色即配置字色、外色自动等于真实背景,这类精度约束整体消失(`themeKeyRgb`/`sanitizeKeyColor` 已删)。
  - 历史结论更正:旧注释称"ULW 提交成功但不上屏、DComp 冻结",那是**任务栏子窗口(WS_CHILD)+ DComp/NOREDIRECTIONBITMAP 混用**时的实测;现在窗口是独立顶层窗口,已实测菜单模态下 ULW 正常更新(菜单开着推新歌词,画面差异 11.4%,关闭后仅 0.1%)。
- **ULW 提交纪律**:位置/尺寸变化(布局 `MoveWindow`、拖动、`SW_SHOWNOACTIVATE` 恢复可见、`WM_DPICHANGED`)后必须重新调 `renderer.onPaint()` 提交,否则窗口空白或错位;`ensureSurface` 是表面(重)建的唯一入口且幂等,`WM_SIZE`/`onPaint` 都走它。**全项目禁止再调 `SetLayeredWindowAttributes`**(与 ULW 互斥,会夺回合成权丢弃逐像素 alpha)。
- **主题跟随**:默认字色跟随 Windows 浅色/深色(优先级:显式配置 > 主题默认 > 深色兜底;浅色 primary `0xFF1A1A1A`/secondary `0xB31A1A1A`,深色全白);检测 = 主线程 `WM_SETTINGCHANGE(ImmersiveColorSet)` + 布局心跳快照携带 lightTheme 兜底;判定键 **`SystemUsesLightTheme`（"Windows 模式"，任务栏跟随）优先**、`AppsUseLightTheme`（"应用模式"）回退——两者可独立设置，弄反会导致"应用浅色+任务栏深色"时字色取值反相(深字压深任务栏,只余抗锯齿灰边);开关持久化 `HKCU\Software\Taskbar-Lyrics\ThemeFollow`,托盘菜单可切换。**托盘接管锁**(`colorLockedByTray`,不持久化):每次托盘操作颜色后忽略管道颜色 key 重放,直到托盘“恢复管道颜色设置”或重启;WM_CREATE 同步 color_theme_light 初始化并立即 onPaint 首帧(消除占位色闪烁);解锁底衬经 `applyOverlayMode(bool)` 切换(只改位图内底衬 alpha,不碰窗口分层属性)。
- **配置解析健壮性**:颜色解析仅接受十六进制——可选 `0x` 前缀 + 恰好 6 位(RGB 补 FF alpha)或 8 位(ARGB),不接受纯十进制任意长度(与 README/go-musicfox 校验正则对齐);数值解析 from_chars 完整校验(ParseIntValue),非法输入忽略保持当前值并记日志(不崩主线程);config 写入仅主线程。单元测试 `plugin/cpp/tests/test_config.cpp`(CMake `option(TL_BUILD_TESTS OFF)`,ctest 跑 config_logic)。
- **不依赖 WM_PAINT**:歌词更新(WM_APP+1)、布局应用(WM_APP+3)、主题切换(applyTheme)直接调 `renderer.onPaint()`(渲染+ULW 提交)。任务栏右键菜单模态会抑制 `WM_PAINT` 派发,ULW 属 win32k 合成路径、本就不经 WM_PAINT,菜单弹出期间歌词持续流动(已实测)。
- **全屏隐藏**:前台无边框全屏(`SHQueryUserNotificationState` + 几何兜底)时 `SW_HIDE`(隐藏期 `onPaint` 直接返回),退出全屏自动恢复并重新提交。
- **锁定语义**:锁定=透明底 + 穿透 + 自动定位;解锁=半透明底衬(位图内 alpha 210,文字保持全不透明)+ 可拖动(仅垂直任务栏模式);状态记忆于 `HKCU\Software\Taskbar-Lyrics`。
- **对齐**:AUTO 自检测(图标居中→左空档;开始按钮在左→中间空档),LEFT/RIGHT/CENTER。
- **管道健壮性**:`PIPE_NOWAIT` 非阻塞、`PeekNamedPipe` 探测断开、pending 缓冲 64KB 上限、畸形 JSON 跳过、config 覆盖语义(最后一次为准)。
- v1 取舍:explorer 重启销毁窗口 → 干净退出,不实现 `TaskbarCreated` 重建(需手动重启工具)。

## 7. 开发约定

- 提交信息用中文,带 `feat/fix/chore/docs` 前缀,描述实际变更。
- 改动后同步 `IMPLEMENTATION_PLAN.md`(如涉及阶段验收);架构/结构变化时同步更新本文件与 `CLAUDE.md`。
- 构建/验证在 Windows 工具链(CMake + MSVC)下进行;构建产物不入库。
