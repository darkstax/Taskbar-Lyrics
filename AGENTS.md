# Taskbar-Lyrics — 项目技术文档

> Windows 11 任务栏实时歌词工具:独立 EXE 在任务栏上显示 go-musicfox 的当前播放歌词(含翻译);
> 原为 BetterNCM 插件,已移除插件外壳与 JS 层,改为独立顶层窗口应用。

## 1. 技术栈

| 类别 | 内容 |
|------|------|
| 语言 | C++20(C++ Modules,`.cppm` 模块;`/utf-8 /D_UNICODE /DUNICODE`) |
| 构建 | CMake 3.30+(`CXX_MODULES FILE_SET`)+ Visual Studio 2022(MSVC,生成器 `Visual Studio 17 2022`)+ CMakePresets |
| 依赖 | 仅 Windows 库:d3d11、d2d1、dwrite、dxgi、dcomp、shell32;第三方仅 `third_party/nlohmann/json.hpp`(单头) |
| 平台 | Windows 专用(Win11 优先;PerMonitorV2 DPI) |
| 通信 | 命名管道 `\\.\pipe\go-musicfox.lyric.v1`(JSON Lines,**本工具为服务端**,go-musicfox 为客户端) |

## 2. 架构概览

```
main.cpp(wWinMain)→ Plugin::getInstance().run()(互斥锁 Local\Taskbar-Lyrics 防多开)→ 主线程消息循环
├── LyricPipeServer(管道线程)    # 只解析 JSON 写入缓存 → PostMessageW(WM_APP+1) 通知主线程
├── 布局线程(UIAutomation)       # 每 2s 心跳 + 结构变化事件测量任务栏 → WM_APP+3 回主线程
└── 主线程                      # 歌词写入/窗口重绘全部收敛于此(永不阻塞)
        ├── Window.cppm         # 顶层透明窗口、布局、全屏隐藏
        ├── Renderer.cppm       # Direct2D HwndRenderTarget + LWA_COLORKEY 颜色键透明
        └── Lyrics.cppm         # DirectWrite 文本布局、字号自适应(下限 0.6)
```

- 消息类型:`lyric`(primary/secondary 歌词)、`config`(窗口配置,全字符串);`meta`/`state` 预留。
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
- **透明渲染**:D2D `HwndRenderTarget` 直画客户区 + `LWA_COLORKEY` 颜色键透明(黑色键色 + 灰度抗锯齿);不用 DComp/UpdateLayeredWindow(菜单模态下不可靠)。
- **WM_PAINT 抑制**:任务栏右键菜单模态会抑制 `WM_PAINT`,歌词更新(WM_APP+1)与布局应用(WM_APP+3)直接调 `renderer.onPaint()` 绕过。
- **全屏隐藏**:前台无边框全屏(`SHQueryUserNotificationState` + 几何兜底)时 `SW_HIDE`,退出全屏自动恢复。
- **锁定语义**:锁定=透明底 + 穿透 + 自动定位;解锁=半透明底(LWA_ALPHA 210)+ 可拖动(仅垂直任务栏模式);状态记忆于 `HKCU\Software\Taskbar-Lyrics`。
- **对齐**:AUTO 自检测(图标居中→左空档;开始按钮在左→中间空档),LEFT/RIGHT/CENTER。
- **管道健壮性**:`PIPE_NOWAIT` 非阻塞、`PeekNamedPipe` 探测断开、pending 缓冲 64KB 上限、畸形 JSON 跳过、config 覆盖语义(最后一次为准)。
- v1 取舍:explorer 重启销毁窗口 → 干净退出,不实现 `TaskbarCreated` 重建(需手动重启工具)。

## 7. 开发约定

- 提交信息用中文,带 `feat/fix/chore/docs` 前缀,描述实际变更。
- 改动后同步 `IMPLEMENTATION_PLAN.md`(如涉及阶段验收);架构/结构变化时同步更新本文件与 `CLAUDE.md`。
- 构建/验证在 Windows 工具链(CMake + MSVC)下进行;构建产物不入库。
