# Taskbar-Lyrics (go-musicfox 适配版)

在 Windows 11 任务栏上显示 go-musicfox 当前播放歌词的独立工具。

## 架构

- 数据源：go-musicfox 通过命名管道 `\\.\pipe\go-musicfox.lyric.v1` 推送歌词（JSON Lines：`{"type":"lyric","primary":"...","secondary":"..."}`）
- 渲染：Direct2D/DirectWrite 透明子窗口，UIAutomation 定位任务栏
- 独立 EXE，无需网易云客户端 / BetterNCM

## 构建

需要 Visual Studio 2022+（C++20 Modules 支持）与 CMake 3.30+：

```bash
cd plugin/cpp
cmake --preset x64-release
cmake --build --preset x64-release
```

产物：`build/x64-release/Release/taskbar-lyrics.exe`

## 使用

1. 启动 `taskbar-lyrics.exe`（单实例，重复启动会提示已在运行）
2. 启动 go-musicfox（需包含歌词管道输出功能的版本）播放歌曲
3. 任务栏实时显示当前歌词与下一行歌词
