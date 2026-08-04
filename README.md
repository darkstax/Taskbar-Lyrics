# Taskbar-Lyrics (go-musicfox 适配版)

在 Windows 11 任务栏上显示 go-musicfox 当前播放歌词的独立工具（含歌词翻译）。

## 架构

- **数据源**：go-musicfox 通过命名管道 `\\.\pipe\go-musicfox.lyric.v1` 推送消息
  - `{"type":"lyric","primary":"当前行","secondary":"翻译或下一行"}`——`secondary` 优先当前行翻译（外语歌显示原文+翻译两行），无翻译时为下一行原文
  - `{"type":"config","config":{...}}`——窗口配置下发（对齐/字体/字号，值全为字符串）
- **渲染**：D2D HwndRenderTarget 直接绘制窗口客户区 + LWA_COLORKEY 颜色键透明（黑色键色 + 灰度抗锯齿）；歌词更新路径直接调用 onPaint（任务栏右键菜单模态会抑制 WM_PAINT 派发，直接绘制可绕过——菜单弹出期间歌词持续流动）
- **窗口**：独立顶层窗口（TOPMOST/TOOLWINDOW/NOACTIVATE/LAYERED + `WM_NCHITTEST→HTTRANSPARENT` 点击穿透），UIAutomation 定位任务栏（布局独立线程测量，主线程永不阻塞）
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

支持两种模式：**安装模式**（推荐，musicfox 可自动拉起）与**便携模式**。

### 安装模式

```powershell
pwsh -File .\scripts\install.ps1            # 安装到 %LOCALAPPDATA%\Programs\Taskbar-Lyrics + 开机自启
pwsh -File .\scripts\install.ps1 -NoAutostart  # 跳过开机自启
pwsh -File .\scripts\uninstall.ps1         # 卸载
```

- 安装后 go-musicfox（`taskbarPipe=true` 时）启动会自动搜索并拉起本工具，无需手动启动
- 搜索顺序：`taskbarPipeBin` 配置 → 安装目录 → musicfox 同目录 → PATH

### 便携模式

把 `taskbar-lyrics.exe` 放到任意目录直接运行即可（与安装模式互斥，安装会替换为同一份程序）。

### go-musicfox 配置（`config.toml` 的 `[main.lyric]` 段）

```toml
taskbarPipe = true              # 开启任务栏歌词输出（musicfox 会自动拉起本工具）
taskbarAlignment = "auto"       # 对齐：auto(自检测)/left/center/right
taskbarFontFamily = "Microsoft YaHei UI"  # 字体
taskbarFontSizePrimary = 14     # 原文行字号
taskbarFontSizeSecondary = 14   # 翻译/下一行字号
```

- `auto` 自检测：任务栏图标居中 → 歌词在左侧空档；开始按钮在左 → 歌词在中间空档
- 改配置后重启 musicfox 生效

### 手动启动

1. 启动 `taskbar-lyrics.exe`（单实例，重复启动会提示已在运行）
2. 启动 go-musicfox（需包含歌词管道输出功能的版本，见上文配置）播放歌曲
3. 任务栏实时显示当前歌词（外语歌含翻译）

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

- **explorer 重启**：歌词窗口是独立顶层窗口（非任务栏子窗口），explorer 重启时窗口存活，
  布局线程会自动重测任务栏位置；若窗口因故销毁则经 `WM_DESTROY` 干净退出，需手动重启本工具。
- **任务栏右键菜单模态**：菜单弹出期间歌词持续流动（直接 onPaint 绕过 WM_PAINT 抑制）；
  反复右键不会卡死 explorer（UIA 查询全部在独立布局线程，主线程不阻塞）。
