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

## 运行注意事项（MOTW）

`taskbar-lyrics.exe` 若经由 WSL、网络下载或跨机器拷贝获得，Windows 会为其附加
"来自其他计算机"标记（NTFS 备用数据流 `Zone.Identifier`），直接运行可能触发
SmartScreen / "下载文件"警告——本工具为自建 EXE，属误报，运行前需解除该标记。

- **推荐**：用 `scripts/run.ps1` 启动，脚本会自动解除标记再启动（`-NoStart` 只洗属性不启动）：

  ```powershell
  powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1
  powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1 -NoStart
  ```

- **手动**：右键 exe → 属性 → 勾选"解除锁定"；或执行 `Unblock-File .\taskbar-lyrics.exe`
  （等效于删除 `Zone.Identifier` 备用流）。

## 已知取舍（v1）

- **explorer 重启**：歌词窗口是任务栏（Shell_TrayWnd）的子窗口，explorer 重启会销毁该窗口，工具随即经 `WM_DESTROY` 干净退出。v1 不实现 `TaskbarCreated` 消息监听与窗口重建，explorer 重启后请手动重新启动本工具。
