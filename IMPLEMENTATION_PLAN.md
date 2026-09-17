# go-musicfox 任务栏歌词 (Taskbar-Lyrics 改造) 实施计划

> **For Hermes:** 按本计划逐任务实施，每任务完成后 git commit（中文提交信息）。

> **实施状态（截至 2026-09-17，HEAD `b1e76c4`）：** 本计划的 6 个 Task 与 Phase 5 增强全部完成；
> 之后又陆续落地了主题跟随、托盘、垂直任务栏、全屏隐藏、单元测试与一次渲染管线重构。
> 本文件自本状态块以下**保留为历史实施计划**，描述现状的活文档是 `README.md` / `AGENTS.md` / `CLAUDE.md`。
>
> 已完成（按主题，细节见 git log 与 `AGENTS.md`）：
>
> - **渲染管线重构**（`b1e76c4`，2026-09-17）：透明从 `LWA_COLORKEY` 颜色键改为「离屏 32bpp 预乘 alpha DIB +
>   `UpdateLayeredWindow`」逐像素真透明；删除 `themeKeyRgb`/`sanitizeKeyColor` 与"字色禁止等于键色"约束；
>   修正 `Lyrics` 的 `GetSize()` 双重换算（字号因此变大），贴合系数 1.18 → 1.0（不裁切）。
> - **主题跟随**（`c0e565c` 起）：字色默认跟随 Windows 浅色/深色，优先级 显式配置 > 主题默认 > 深色兜底；
>   判定键 `SystemUsesLightTheme`（任务栏跟随）优先、`AppsUseLightTheme` 回退（`c2b7119` 修正取反缺陷）。
> - **托盘菜单**：锁定/解锁、跟随系统主题、恢复管道颜色、退出；配套会话级「托盘颜色接管锁」。
> - **垂直任务栏适配**、**全屏自动隐藏/恢复**、**安装/卸载/洗 MOTW 脚本**。
> - **单元测试入库**（`87789fb`）：`plugin/cpp/tests/test_config.cpp`（`-DTL_BUILD_TESTS=ON` + ctest `config_logic`）。
>
> Phase 5 要点（设计仍有效）：翻译方案 A（primary=当前行、secondary 优先当前行翻译，无翻译回落下一行）；
> 窗口配置经管道 `config` 消息下发（值全为字符串，对齐 0/1/2/3，连接/重连后补发）；
> CENTER=全宽窗口+文字居中；AUTO 自检测（`TaskbarAl`：开始按钮在左→中间空档，图标居中→左侧空档）；
> 字号自适应（**后续改为双向贴合**：目标总高 = 1.0×栏高、下限 0.6/上限 4.0，见 `Lyrics.cppm`）；
> 管道断开探测用 `PeekNamedPipe` 修掉"客户端断开后单实例管道被占死"。
>
> 历史修正记录（Task 1-6 实施期 + Phase 1 审查后，保留备查）：
>
> - 管道名统一为 `\\.\\pipe\\go-musicfox.lyric.v1`（与代码/README 一致，原计划中的 `\\.\\pipe\\musicfox-lyric` 已作废）；
> - server 语义修正：stop() 置标志 + 锁内 CloseHandle 唤醒 + join，runLoop 写入 pipeHandle 后、ConnectNamedPipe 前重查 running，新句柄自灭，杜绝 join 死锁；
> - 歌词更新收敛主线程：管道线程只写内部缓存并 PostMessageW(WM_APP+1)，主线程写 config 并重绘（不再跨线程改 config / 调 UIAutomation）；
> - 窗口消息循环补 WM_DESTROY → PostQuitMessage 干净退出。**（更正）** 原文写"explorer 重启后由用户重启工具，v1 不做 TaskbarCreated 重建"——现窗口是独立顶层窗口，explorer 重启**不销毁**窗口、布局心跳自动归位；托盘图标也已处理：收到 `TaskbarCreated` 广播即重新注册（见 README「已知取舍」）。
> - 窗口创建失败检查、JSON 快速过滤健壮化（直接 parse 后按 type 判断）、pending 缓冲 64KB 上限、Config 显式包含 `<Windows.h>`。

**Goal:** 把 Taskbar-Lyrics（BetterNCM 网易云客户端插件）改造为独立的 Windows 任务栏歌词工具，数据源改为 go-musicfox 的歌词输出，使 go-musicfox 播放时在 Windows 11 任务栏显示当前歌词。

**Architecture:** 保留原项目 C++ 渲染/定位层（UIAutomation 定位任务栏 + D2D/DWrite 渲染），砍掉 BetterNCM 插件外壳（DllMain/JS 事件通道），新增两个数据通道：(1) go-musicfox 侧加一个极简歌词输出（命名管道 `\\.\pipe\go-musicfox.lyric.v1`）；(2) 本工具 C++ 侧新增管道客户端读取并驱动渲染。go-musicfox 的 `internal/lyric/service.go` 已有 `State()` 返回当前行索引+片段，只需加个推送点。
（**实施时的偏离**：窗口最终是**独立顶层窗口**而非任务栏子窗口；管道方向最终是**本工具作服务端**、go-musicfox 作客户端；渲染最终是**离屏预乘 alpha + UpdateLayeredWindow**，均与本文其余部分的原始假设不同，以上方状态块与 `AGENTS.md` 为准。）

**Tech Stack:** C++20 (C++ Modules, MSVC/CMake 3.30+), Direct2D/DirectWrite, Win32 UIAutomation, Go 1.26 (go-musicfox 侧)。

---

## 背景与现状

### 上游 Taskbar-Lyrics 结构（已 fork：`darkstax/Taskbar-Lyrics`）

```
plugin/
├── js/            # BetterNCM JS 层：audioplayer.onLoad/onPlayProgress 事件 → 拉网易云 API → setConfig
│   └── src/
│       ├── main.js    # 监听客户端事件，计算 primary/secondary 歌词行
│       ├── lyrics.js  # LyricObserver + fetch 网易云歌词/歌曲详情
│       └── config.js  # 配置读写
└── cpp/           # 原生 DLL（BetterNCMPluginMain 入口）
    └── src/
        ├── DllMain.cpp          # 插件导出入口（将被移除）
        ├── plugin/
        │   ├── Plugin.cppm      # 单例 + 互斥锁 + 窗口线程（保留思想，改 main()）
        │   ├── Config.cppm      # 全局配置结构 + setConfig（保留，加管道写入路径）
        │   └── Receiver.cppm    # JS→C++ 回调桥（将被移除）
        ├── taskbar/
        │   ├── Taskbar.cppm     # UIAutomation 定位任务栏元素（完整保留）
        │   ├── Handler.cppm     # 结构变化事件处理器（完整保留）
        │   └── Registry.cppm    # 注册表读取：居中/小组件开关（完整保留）
        └── window/
            ├── Window.cppm      # 透明子窗口 + 布局计算（完整保留）
            ├── Renderer.cppm    # D2D 渲染器（完整保留）
            └── Lyrics.cppm      # 歌词文本布局（完整保留）
```

### go-musicfox 侧现状

- `internal/lyric/service.go` 的 `Service` 提供：
  - `State()` → `State{Fragments []LRCFragment, CurrentIndex int, TranslatedFragments, YRCLines, YRCLineIndex, ...}`（线程安全，已有）
  - `UpdatePosition(duration)` 由 UI 驱动推进 `currentIndex`
- 播放器状态变化时 UI 已有钩子：`internal/ui/player.go` 的 stateChan 循环（`p.stateHandler.SetPlayingInfo(...)` 处）
- 渲染循环：`internal/ui/lyric_renderer.go` 每帧调 `lyricService.State()`

## 方案决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| 数据通道 | **命名管道** `\\.\pipe\go-musicfox.lyric.v1`，JSON 行协议 | 实时、无文件残留、go-musicfox 已在用命名管道（SMTC 同思路） |
| go-musicfox 输出点 | `internal/ui/player.go` stateChan 循环 + 一个 `LyricPipeWriter` 小部件 | 播放/暂停/切歌状态变化处天然有钩子；歌词行变化在 `UpdatePosition` 驱动下也经此同步 |
| C++ 侧形态 | 独立 **EXE**（不再做 DLL/插件） | 脱离网易云客户端独立运行 |
| JS 层 | **整体移除** | 数据源改为管道，不再需要客户端事件 |
| 歌词内容 | primary=当前行，secondary=下一行（保持原逻辑） | 与上游渲染预期一致 |

### 管道协议（JSON Lines，UTF-8）

go-musicfox → 工具，每行一个 JSON 对象：

```json
{"type":"lyric","primary":"当前歌词行","secondary":"下一行","ts":1754200000000}
{"type":"meta","title":"歌名","artist":"歌手","ts":1754200000000}
{"type":"state","playing":true,"ts":1754200000000}
```

- `lyric`：歌词内容更新（切行时发送；secondary 可为空串）
- `meta`：切歌时发送一次（未来可扩展显示歌名/歌手）
- `state`：播放/暂停状态变化（暂停时歌词可置灰）
- 工具侧只消费 `lyric`（v1 最小实现），`meta`/`state` 预留

---

## 任务清单

### Task 1: 仓库整理——移除 BetterNCM 外壳

**Objective:** 删除 JS 层与插件导出，使仓库成为纯 C++ EXE 项目。

**Files:**
- Delete: `plugin/js/`（整个目录）
- Delete: `plugin/cpp/src/DllMain.cpp`
- Delete: `plugin/cpp/src/plugin/Receiver.cppm`
- Modify: `plugin/cpp/CMakeLists.txt`
- Modify: `plugin/cpp/CMakePresets.json`（如引用 MSVC 预设则保留）

**Step 1: 删除文件**

```bash
cd /home/starl/ai-code/Taskbar-Lyrics
git rm -r plugin/js
git rm plugin/cpp/src/DllMain.cpp plugin/cpp/src/plugin/Receiver.cppm
```

**Step 2: 改写 CMakeLists.txt 为 EXE 目标**

```cmake
cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(/utf-8 /D_UNICODE /DUNICODE)

project(taskbar-lyrics LANGUAGES CXX)

add_executable(taskbar-lyrics WIN32
    "src/main.cpp"
    "src/plugin/Config.cppm"
    "src/taskbar/Taskbar.cppm"
    "src/taskbar/Handler.cppm"
    "src/taskbar/Registry.cppm"
    "src/window/Window.cppm"
    "src/window/Renderer.cppm"
    "src/window/Lyrics.cppm"
)

target_link_libraries(taskbar-lyrics PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp
)
```

**Step 3: 新建 `plugin/cpp/src/main.cpp`**

```cpp
// 独立 EXE 入口：单例窗口 + 管道客户端线程
import plugin.Plugin;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    Plugin::getInstance();  // 内部已有互斥锁防重复实例
    return 0;
}
```

> 注：`Plugin` 类本身已含 `CreateMutex(Global\Taskbar-Lyrics)` 防多开 + 自建线程跑窗口循环，原样复用即可。需要给 `Plugin` 增加 `LyricPipeClient` 成员（Task 3）。

**Step 4: 验证构建**

```bash
cmake --preset <windows-preset>    # 或 cmake -S plugin/cpp -B build
cmake --build build --config Release
```

Expected: `build/Release/taskbar-lyrics.exe` 生成。

**Step 5: Commit**

```bash
git add -A
git commit -m "refactor: 移除 BetterNCM 插件外壳，改为独立 EXE 项目"
```

---

### Task 2: 新增管道客户端模块

**Objective:** 新建 `src/pipe/LyricPipeClient.cppm`，连接 `\\.\pipe\go-musicfox.lyric.v1` 并解析 JSON 行，回调解包写入 `config.lyric_primary/secondary`。

**Files:**
- Create: `plugin/cpp/src/pipe/LyricPipeClient.cppm`
- Modify: `plugin/cpp/CMakeLists.txt`（加入新源文件）

**Step 1: 写模块骨架**

```cpp
export module pipe.LyricPipeClient;

import <Windows.h>;
import <string>;
import <thread>;
import <atomic>;
import <functional>;
import plugin.Config;

export class LyricPipeClient {
public:
    using OnLyric = std::function<void(const std::wstring &primary, const std::wstring &secondary)>;

private:
    std::thread thread{};
    std::atomic<bool> running{false};
    OnLyric onLyric{};

    auto jsonUnescape(const std::string &s) -> std::string; // 处理 \uXXXX 与转义
    auto parseAndApply(const std::string &line) -> void;

public:
    explicit LyricPipeClient(OnLyric cb) : onLyric(std::move(cb)) {}
    ~LyricPipeClient() { stop(); }

    auto start() -> void;
    auto stop() -> void;
};
```

**Step 2: start() 连接循环（断线重连）**

```cpp
auto LyricPipeClient::start() -> void {
    if (running.exchange(true)) return;
    thread = std::thread([this] {
        while (running) {
            HANDLE pipe = CreateFileW(
                L"\\\\.\\pipe\\go-musicfox.lyric.v1", GENERIC_READ, 0,
                nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                Sleep(1000);  // 服务端未启动，重试
                continue;
            }
            char buf[8192];
            std::string acc;
            DWORD n = 0;
            while (running && ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
                acc.append(buf, n);
                size_t pos;
                while ((pos = acc.find('\n')) != std::string::npos) {
                    parseAndApply(acc.substr(0, pos));
                    acc.erase(0, pos + 1);
                }
            }
            CloseHandle(pipe);
            Sleep(1000);  // 断线重连
        }
    });
}
```

**Step 3: parseAndApply——只认 `lyric` 行，写 config**

```cpp
auto LyricPipeClient::parseAndApply(const std::string &line) -> void {
    if (line.find("\"type\":\"lyric\"") == std::string::npos) return;
    // 极简解析：直接按 key 提取 primary/secondary（不做完整 JSON 解析，v1 够用）
    // primary/secondary 的 value 取引号内原始内容后 jsonUnescape
    // 示例行：{"type":"lyric","primary":"第一行","secondary":"第二行","ts":...}
    std::wstring primary = L" ", secondary = L" ";
    // ... 提取逻辑：找到 "primary":"..." 和 "secondary":"..."
    config.lyric_primary = primary;
    config.lyric_secondary = secondary;
    if (onLyric) onLyric(primary, secondary);
}
```

**Step 4: CMakeLists 追加**

```cmake
add_executable(taskbar-lyrics WIN32
    ...
    "src/pipe/LyricPipeClient.cppm"
)
```

**Step 5: 构建验证**（同 Task 1 Step 4）

**Step 6: Commit**

```bash
git commit -am "feat: 新增命名管道歌词客户端，断线自动重连"
```

---

### Task 3: Plugin 接入管道客户端

**Objective:** `Plugin` 构造时启动 `LyricPipeClient`，歌词更新后触发窗口重绘。

**Files:**
- Modify: `plugin/cpp/src/plugin/Plugin.cppm`

**Step 1: 改造 Plugin**

```cpp
export module plugin.Plugin;

import <Windows.h>;
import <thread>;
import plugin.Config;
import pipe.LyricPipeClient;
import window.Window;

export class Plugin {
public:
    HANDLE mutex = nullptr;
    Window *window = nullptr;
    LyricPipeClient *pipe = nullptr;

private:
    Plugin() {
        this->mutex = CreateMutex(nullptr, true, L"Global\\Taskbar-Lyrics");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(this->mutex);
            this->mutex = nullptr;
        } else {
            this->initialize();
        }
    }

    ~Plugin() {
        if (this->pipe) { delete this->pipe; this->pipe = nullptr; }
        if (this->mutex) { ReleaseMutex(this->mutex); CloseHandle(this->mutex); this->mutex = nullptr; }
        if (this->window) { delete this->window; this->window = nullptr; }
    }

    auto initialize() -> void {
        std::thread([this] {
            this->window = new Window();
            this->window->create();
            // 管道回调：歌词更新 → 窗口重绘
            this->pipe = new LyricPipeClient([this](const std::wstring &, const std::wstring &) {
                if (this->window) this->window->update();
            });
            this->pipe->start();
            this->window->runner();
        }).detach();
    }

public:
    static auto getInstance() -> Plugin & {
        static Plugin instance;
        return instance;
    }
};
```

**Step 2: 构建验证**

**Step 3: Commit**

```bash
git commit -am "feat: Plugin 接入管道客户端，歌词更新触发重绘"
```

---

### Task 4: go-musicfox 侧——歌词管道输出

**Objective:** go-musicfox 增加 `LyricPipeWriter`，在播放状态/歌词行变化时向 `\\.\pipe\go-musicfox.lyric.v1` 推送 JSON 行。

> 前置：go-musicfox 源码位于 `~/ai-code/go-musicfox/`（需先 clone 到工作区；或直接操作 `/tmp/gmf-src` 现有 v5.0.1 修复版源码）。本任务修改的是 go-musicfox 自己的仓库，若工作区无源码则先 `git clone https://github.com/go-musicfox/go-musicfox`。

**Files:**
- Create: `internal/lyric/pipe_writer.go`
- Modify: `internal/ui/player.go`

**Step 1: 新建 pipe_writer.go**

```go
package lyric

import (
    "encoding/json"
    "fmt"
    "log/slog"
    "time"

    "golang.org/x/sys/windows"
)

// LyricPipeWriter 向任务栏歌词工具推送歌词的命名管道客户端（Windows only）
type LyricPipeWriter struct {
    handle windows.Handle
    enc    *json.Encoder
}

type pipeLyricMsg struct {
    Type      string `json:"type"`
    Primary   string `json:"primary,omitempty"`
    Secondary string `json:"secondary,omitempty"`
    Title     string `json:"title,omitempty"`
    Artist    string `json:"artist,omitempty"`
    Playing   *bool  `json:"playing,omitempty"`
    Ts        int64  `json:"ts"`
}

func NewLyricPipeWriter() *LyricPipeWriter { return &LyricPipeWriter{} }

func (w *LyricPipeWriter) connect() error {
    // CreateFileW("\\\\.\\pipe\\go-musicfox.lyric.v1", GENERIC_WRITE, ...)
    // 失败则重试（工具可能还没启动）
    return nil
}

// PushLyric 推送当前歌词行（primary=当前行, secondary=下一行）
func (w *LyricPipeWriter) PushLyric(primary, secondary string) {
    w.write(pipeLyricMsg{Type: "lyric", Primary: primary, Secondary: secondary, Ts: time.Now().UnixMilli()})
}

func (w *LyricPipeWriter) write(msg pipeLyricMsg) {
    // 管道未连接则尝试 connect；写失败置为断开（下次重连）
}
```

**Step 2: player.go 接入**

在 `internal/ui/player.go` 的 stateChan 循环（现有 `p.stateHandler.SetPlayingInfo(p.PlayingInfo())` 处）追加：

```go
// 推送歌词到任务栏管道
if st := p.lyricService.State(); st.CurrentIndex >= 0 && len(st.Fragments) > 0 {
    primary := st.Fragments[st.CurrentIndex].Content
    secondary := ""
    if st.CurrentIndex+1 < len(st.Fragments) {
        secondary = st.Fragments[st.CurrentIndex+1].Content
    }
    p.lyricPipe.PushLyric(primary, secondary)
}
```

`NewPlayer` 中初始化 `p.lyricPipe = lyric.NewLyricPipeWriter()`。

> 注意：stateChan 只在状态变化时触发；若需逐行精确同步，可在 `lyric_renderer.go` 每帧渲染处（已有 `State()` 调用）追加相同推送，并加"行索引变化才推送"的节流。**v1 采用 stateChan + 行变化节流**，实现简单且够用。

**Step 3: 构建验证**

```bash
cd <go-musicfox 源码目录>
GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=x86_64-w64-mingw32-gcc \
  go build -tags "enable_global_hotkey purego" -o /tmp/musicfox-lyric.exe ./cmd
```

Expected: 编译通过，`strings` 能看到 `go-musicfox.lyric.v1` 管道名。

**Step 4: Commit**（go-musicfox 仓库内）

```bash
git add internal/lyric/pipe_writer.go internal/ui/player.go
git commit -m "feat: 新增任务栏歌词命名管道输出"
```

---

### Task 5: 端到端联调

**Objective:** go-musicfox 播放时，任务栏实时显示当前歌词。

**Step 1: 构建两个产物**

```bash
# 工具侧（Taskbar-Lyrics）
cmake --build <build-dir> --config Release   # → taskbar-lyrics.exe

# go-musicfox 侧
go build ... -o musicfox-lyric.exe ./cmd
```

**Step 2: 手动验证清单**

1. 先启动 `taskbar-lyrics.exe`（无 musicfox 时窗口存在但无歌词，不崩溃）
2. 再启动 musicfox，播放一首带歌词的歌
3. 任务栏出现歌词，随歌曲进度切换 primary/secondary
4. 切歌 → 歌词切换为新歌
5. 暂停/继续 → 歌词状态正常
6. 杀掉 musicfox（模拟异常退出）→ 工具不崩溃，等待重连
7. 重新开 musicfox → 歌词恢复
8. 重复启动 `taskbar-lyrics.exe` → 第二个实例被互斥锁拒绝（Global\Taskbar-Lyrics）

**Step 3: 日志验证**

- 工具侧：任务栏窗口存在，`GetLastError` 无管道错误
- musicfox 日志：`mpv listen`/`lyric` 管道推送无报错

**Step 4: Commit（如有修复）**

---

### Task 6: 文档与收尾

**Objective:** README 说明新架构与使用方式。

**Files:**
- Modify: `README.md`

**Step 1: 重写 README 头部**

```markdown
# Taskbar-Lyrics (go-musicfox 适配版)

在 Windows 11 任务栏上显示 go-musicfox 当前播放歌词的独立工具。

## 架构
- 数据源：go-musicfox 命名管道 `\\.\pipe\go-musicfox.lyric.v1`（JSON Lines）
- 渲染：Direct2D/DirectWrite 透明子窗口，UIAutomation 定位任务栏
- 独立 EXE，无需网易云客户端 / BetterNCM

## 构建
（CMake 命令）

## 使用
1. 启动 taskbar-lyrics.exe
2. 启动 go-musicfox（需包含歌词管道输出功能的版本）播放歌曲
```

**Step 2: Commit**

```bash
git add README.md
git commit -m "docs: 更新 README 为新架构说明"
```

---

## 风险与权衡

| 风险 | 影响 | 缓解 |
|---|---|---|
| go-musicfox 源码不在工作区 | 需先 clone | Task 4 前置注明 |
| C++20 Modules 需较新 MSVC/CMake | 构建失败 | 用 CMakePresets 里的 MSVC 预设；必要时降级为传统头文件 |
| 管道 JSON 极简解析脆弱 | 字段提取出错 | v1 只读 primary/secondary 两个字段，格式自控（go-musicfox 侧也是我们写的） |
| 任务栏布局差异（Win11 更新） | 定位偏移 | 复用上游 Taskbar.cppm 的 UIAutomation 逻辑，上游已在跟进 |
| 暂停时歌词不更新 | 显示陈旧 | state 消息预留，v1 可接受；后续加置灰 |
| 高 DPI/多显示器 | 位置错乱 | 上游已有"适配高DPI"提交（fork 最新），保留其逻辑 |
| 构建产物带 Zone.Identifier（MOTW） | 运行弹 SmartScreen"来自其他计算机"警告 | **实施注意事项：** 构建/拷贝的 exe 运行前必须解除该标记（`Unblock-File` 或删 `Zone.Identifier` 备用流）；`scripts/run.ps1` 启动时已自动处理 |

## 验证命令汇总

```bash
# 工具侧构建
cmake -S plugin/cpp -B build && cmake --build build --config Release

# go-musicfox 侧构建（交叉编译）
cd go-musicfox && GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=x86_64-w64-mingw32-gcc \
  go build -tags "enable_global_hotkey purego" -o /tmp/musicfox-lyric.exe ./cmd

# 端到端：taskbar-lyrics.exe + musicfox 播放 → 观察任务栏

# 启动前洗属性（构建产物带 Zone.Identifier/MOTW 时运行会弹 SmartScreen 警告，
# 用 scripts/run.ps1 启动可自动 Unblock-File 解除；-NoStart 只洗不启动）
powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1
powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1 -NoStart
```

## 开放问题

1. go-musicfox 的歌词管道功能是否需要做成配置开关（默认开/关）？建议加 `[main.lyric] taskbarPipe = true/false`。
2. 是否要显示 meta（歌名/歌手）？v1 只做歌词，meta 消息预留。
3. 工具是否要自启动/托盘图标？v1 不做，手动启动。
