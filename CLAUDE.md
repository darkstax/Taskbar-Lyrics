# Taskbar-Lyrics — 快捷指南

> 完整技术文档见 `AGENTS.md`;本文件为精简版,与 AGENTS.md 保持同步。

## 速览

- Windows 11 任务栏实时歌词工具(C++20 Modules),与 go-musicfox 经命名管道 `go-musicfox.lyric.v1` 通信(本工具为服务端)。
- 线程模型:管道线程只解析写缓存,布局线程(UIAutomation)测任务栏,主线程独占 UI/重绘。

## 常用命令

```bash
cd plugin/cpp
cmake --preset x64-release && cmake --build --preset x64-release
# 产物:build/x64-release/Release/taskbar-lyrics.exe
```

## 关键约束

- 窗口样式固定组合:`WS_EX_NOPARENTNOTIFY|WS_EX_NOACTIVATE|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED`。
- 点击穿透用 `WM_NCHITTEST → HTTRANSPARENT`;禁止 `WS_EX_TRANSPARENT`。
- 透明用**离屏 32bpp 预乘 alpha DIB + `UpdateLayeredWindow`(ULW_ALPHA)**(灰度抗锯齿):背景恒 `Clear(0,0,0,0)` 真透明,字形边缘混向透明由 DWM 合成——无颜色键,故无"字色禁止等于键色"约束;禁止 `SetLayeredWindowAttributes`(与 ULW 互斥)、禁止 `WS_EX_NOREDIRECTIONBITMAP`。位置/尺寸/显示变化后必须重新 `onPaint()` 提交。
- 颜色默认跟随系统主题(显式配置 > 主题默认 > 深色兜底;浅色 0xFF1A1A1A/0xB31A1A1A):`WM_SETTINGCHANGE(ImmersiveColorSet)` + 布局心跳 lightTheme 快照兜底,仅主线程应用;判定键 **`SystemUsesLightTheme`(任务栏跟随)优先**、`AppsUseLightTheme` 回退;开关持久化 `HKCU\Software\Taskbar-Lyrics\ThemeFollow`;托盘操作后颜色接管锁生效(忽略管道重放,托盘“恢复管道颜色”或重启解除)。
- 配置解析:颜色仅十六进制(`0x` 前缀可选 + 恰 6/8 位;拒绝纯十进制),非法输入保持当前值不崩;数值 from_chars 完整校验;config 写入仅主线程。单元测试 `plugin/cpp/tests/`(`-DTL_BUILD_TESTS=ON` + ctest,默认不入发布构建)。
- 不依赖 `WM_PAINT`:歌词更新/布局应用/主题切换直接调 `renderer.onPaint()`(渲染 + ULW 提交),菜单模态抑制 WM_PAINT 也不影响。
- 管道:`PIPE_NOWAIT` 非阻塞、`PeekNamedPipe` 探测断开、64KB 缓冲上限、畸形 JSON 跳过。
- 自动化测试:`plugin/cpp/tests/test_config.cpp`(ctest `config_logic`,需 `-DTL_BUILD_TESTS=ON`);验证另走人工端到端清单 + `%TEMP%\taskbar-lyrics.log`。

## 开发约定

- 中文提交信息(feat/fix/chore/docs 前缀);构建产物不入库。
- 改动同步 `IMPLEMENTATION_PLAN.md`;架构变化同步更新 `AGENTS.md` 与 `CLAUDE.md`。
