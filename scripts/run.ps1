# taskbar-lyrics 启动器（含 MOTW 洗属性）
#
# 用途：
#   定位构建产物 taskbar-lyrics.exe（优先 Release 配置）→ 解除"来自其他计算机"
#   标记（Zone.Identifier / MOTW）→ 打印当前状态 → 启动 exe。
#
# MOTW 原理：
#   Windows 对从网络下载、经 WSL/跨机器拷贝而来的文件写入 NTFS 备用数据流
#   Zone.Identifier（ZoneId=3）。资源管理器 / SmartScreen 据此弹出"来自其他
#   计算机，可能不安全"的下载警告。本工具为自建 EXE，属误报：Unblock-File
#   即删除该备用流（等效于"属性→解除锁定"），运行前执行一次即可彻底消除警告。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1           # 洗属性并启动
#   powershell -ExecutionPolicy Bypass -File .\scripts\run.ps1 -NoStart  # 只洗属性，不启动
param(
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'

function Find-TaskbarLyricsExe {
    $repoRoot = Split-Path -Parent $PSScriptRoot

    # 已知候选路径（构建产物落点：CMakePresets.json 的 binaryDir + 实际使用的 build/phase2）
    $candidates = @(
        (Join-Path $repoRoot 'plugin/cpp/build/phase2/taskbar-lyrics.exe'),
        (Join-Path $repoRoot 'plugin/cpp/build/x64-release/Release/taskbar-lyrics.exe'),
        (Join-Path $repoRoot 'plugin/cpp/build/x86-release/Release/taskbar-lyrics.exe'),
        (Join-Path $repoRoot 'plugin/cpp/build/x64-debug/Debug/taskbar-lyrics.exe'),
        (Join-Path $repoRoot 'plugin/cpp/build/x86-debug/Debug/taskbar-lyrics.exe')
    )
    foreach ($p in $candidates) {
        if (Test-Path -LiteralPath $p) { return $p }
    }

    # 兜底：递归查找 build 目录（优先 Release 路径）
    $buildRoot = Join-Path $repoRoot 'plugin/cpp/build'
    if (Test-Path -LiteralPath $buildRoot) {
        $exes = Get-ChildItem -Path $buildRoot -Recurse -Filter 'taskbar-lyrics.exe' -File -ErrorAction SilentlyContinue
        if ($exes) {
            return ($exes | Sort-Object { $_.FullName -match '\\Release\\' } -Descending | Select-Object -First 1).FullName
        }
    }

    throw '未找到构建产物 taskbar-lyrics.exe，请先构建（见 README.md「构建」一节）。'
}

$exe = Find-TaskbarLyricsExe
Write-Host "产物：$exe"

# ---- 解除 MOTW（Zone.Identifier 备用流）----
$hasMotw = $null -ne (Get-Item -LiteralPath $exe -Stream Zone.Identifier -ErrorAction SilentlyContinue)
if ($hasMotw) {
    Unblock-File -Path $exe -ErrorAction SilentlyContinue
    # 双保险：Unblock-File 偶发失败时直接删除备用流（流已不存在时静默忽略）
    Remove-Item -LiteralPath $exe -Stream Zone.Identifier -ErrorAction SilentlyContinue
    Write-Host '[洗属性] 已解除 Zone.Identifier (MOTW) 标记，SmartScreen 警告已消除'
} else {
    Write-Host '[洗属性] 无 Zone.Identifier 标记，无需处理'
}

if (-not $NoStart) {
    Write-Host "启动：$exe"
    Start-Process -FilePath $exe
} else {
    Write-Host '-NoStart：已跳过启动'
}
