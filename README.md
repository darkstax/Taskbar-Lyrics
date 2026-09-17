# Taskbar-Lyrics (go-musicfox 适配版)

在 Windows 11 任务栏上显示 go-musicfox 当前播放歌词的独立工具（含歌词翻译）。

## 架构

- **数据源**：go-musicfox 通过命名管道 `\\.\pipe\go-musicfox.lyric.v1` 推送消息
  - `{"type":"lyric","primary":"当前行","secondary":"翻译或下一行"}`——`secondary` 优先当前行翻译（外语歌显示原文+翻译两行），无翻译时为下一行原文
  - `{"type":"config","config":{...}}`——窗口配置下发（对齐/字体/字号/颜色，值全为字符串）
- **颜色跟随系统主题**：默认自动跟随 Windows 浅色/深色主题（浅色任务栏用深字、
  深色任务栏用白字）；优先级 = 用户显式配置色 > 主题默认 > 深色兜底。
  go-musicfox 侧 `taskbarColorPrimary/Secondary` **留空即跟随主题**（下发空串取消覆盖），
  显式设值则固定颜色；也可用托盘菜单“跟随系统主题”开关。实现：`WM_SETTINGCHANGE
  (ImmersiveColorSet)` 主线程响应 + 2s 布局心跳携带主题位做兜底（锁屏/唤醒错过广播也能收敛）
- **渲染**：D2D 离屏 32 位预乘 alpha 表面（`ID2D1DCRenderTarget` + `CreateDIBSection`）+ `UpdateLayeredWindow` 逐像素真透明——窗口背景 alpha=0，字形抗锯齿边缘自由混向透明、由 DWM 与真实任务栏合成。因此**不存在"颜色键"**，也就没有"底色必须等于键色""字色禁止等于键色"这类精度约束：浅/深色主题、壁纸明暗、任务栏透明效果下都不会出现光晕或字发灰。歌词更新路径直接调用 onPaint（不依赖 WM_PAINT，任务栏右键菜单模态下歌词持续流动，已实测）
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

单元测试（`Config.cppm` 纯逻辑：颜色/数值解析、主题四态、托盘接管，审查修复 F1）：

```bash
cmake --preset x64-release -DTL_BUILD_TESTS=ON
cmake --build --preset x64-release
ctest --test-dir build/x64-release -C Release --output-on-failure
```

测试默认 OFF（`option(TL_BUILD_TESTS OFF)`），发布构建产物不含 test_config.exe。

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
# taskbarColorPrimary = ""      # 原文颜色：留空=跟随系统主题（推荐）；AARRGGBB=固定覆盖
# taskbarColorSecondary = ""    # 翻译/下一行颜色：同上
```

- `auto` 自检测：任务栏图标居中 → 歌词在左侧空档；开始按钮在左 → 歌词在中间空档
- 颜色格式（仅十六进制，两种写法均可）：`0xAARRGGBB`（如 `0xFFFFFFFF`）或
  裸 `AARRGGBB`（如 `FFFFFFFF`、`FF4FC3F7`）；裸/前缀的 6 位 `RRGGBB`（如 `4FC3F7`）
  自动补不透明 alpha；其余格式（含纯十进制、其它长度）非法，被忽略且保持当前值
  （日志有警告；go-musicfox 侧也会提前校验，非法值按留空处理并告警）
- 颜色留空 = 本工具按 Windows 浅色/深色主题自动选色（浅色 `0xFF1A1A1A`/`0xB31A1A1A`，
  深色 `0xFFFFFFFF`）；显式设值 = 固定颜色，切主题不受影响；改回留空可取消覆盖
- 改配置后重启 musicfox 生效

### 托盘菜单与颜色接管（重要）

托盘右键菜单：锁定/解锁歌词 · 跟随系统主题 · 恢复管道颜色设置 · 退出。

每次在托盘操作“跟随系统主题”（无论勾选还是取消）后，本工具进入**颜色接管**状态：
此后管道下发的颜色配置（`taskbarColorPrimary/Secondary` 的全量重放）一律被忽略，
直到：

- 在托盘菜单点“恢复管道颜色设置（解除托盘接管）”（解除后立即重放当前管道配置，
  无需等下一行歌词），或
- 重启本工具（接管不持久化：它是会话级决定；重启后 go 侧配置重新生效，
  而“跟随开关”本身持久化于注册表 ThemeFollow）。

没有接管机制时，托盘刚点的“跟随”会被下一行歌词的配置重放静默冲掉（已修复的感知级缺陷）。

> 自动化/测试提示：菜单命令（锁定=1、解锁=2、退出=3、跟随主题=4、恢复管道颜色=5）
> 同样响应外部 `PostMessageW(hwnd, WM_COMMAND, cmd, 0)`（窗口类名 `taskbar_lyrics`，
> 同用户会话内本地进程可发，权限面与手动点菜单等价），便于端到端脚本验证托盘行为。

### 高级配置（第三方管道客户端）

本工具也接受其它命名管道客户端下发 `config.theme_follow`（"true"/"1"/"auto"/"theme" 开启，
"false"/"0" 关闭）：go-musicfox 当前无此下发路径，该 key 供第三方客户端编程控制主题跟随；
注意它不受颜色接管锁影响（接管只拦颜色 key）。

### 手动启动

1. 启动 `taskbar-lyrics.exe`（单实例，重复启动会提示已在运行）
2. 启动 go-musicfox（需包含歌词管道输出功能的版本，见上文配置）播放歌曲
3. 任务栏实时显示当前歌词（外语歌含翻译）

## 发布说明（版本耦合，升级须知）

- **新 go-musicfox + 旧 taskbar-lyrics 会崩溃**：新版 musicfox 始终下发颜色 key（含空串，
  用于表达“回到跟随主题”），旧版 C++ 用 `std::stoul("")` 解析会抛异常直接崩主线程。
  两端必须配套升级到本版本之后（本 EXE 无内嵌版本字符串，以 git commit/日期区分：
  主题跟随功能自 2026-09 提交 c0e565c 起）。
- **旧 go-musicfox + 新 taskbar-lyrics 兼容**：旧版仅在颜色非空时下发，新版解析器
  接受同样格式（hex），行为一致。

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

- **explorer 重启**：歌词窗口是独立顶层窗口（非任务栏子窗口），explorer 重启不会销毁它，
  布局线程每 2s 心跳会用新建的 UIA 实例重测任务栏、窗口自动归位。
  但**托盘图标会随 explorer 重启丢失**（`Shell_NotifyIcon` 注册被销毁，v1 不做 `TaskbarCreated` 重建），
  因此重启 explorer 后托盘菜单（锁定/跟随主题/退出）不可用，需重启本工具恢复。
- **任务栏右键菜单模态**：菜单弹出期间歌词持续流动（渲染走 `UpdateLayeredWindow`，
  不经 WM_PAINT，菜单模态抑制不到它；已实测菜单开着推新歌词画面照常更新，关闭后无跳变）；
  反复右键不会卡死 explorer（UIA 查询全部在独立布局线程，主线程不阻塞）。
