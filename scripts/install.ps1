# ============================================================
#  Taskbar-Lyrics 安装脚本（安装模式）
#  用法: pwsh -File install.ps1 [-Source <exe路径>] [-NoAutostart]
#  安装位置: %LOCALAPPDATA%\Programs\Taskbar-Lyrics\ （per-user，无需管理员）
#  默认注册 HKCU Run 开机自启（-NoAutostart 可跳过）
#  卸载: 运行 uninstall.ps1
# ============================================================
param(
    [string]$Source = "",
    [switch]$NoAutostart
)

$ErrorActionPreference = "Stop"

$installDir = Join-Path $env:LOCALAPPDATA "Programs\Taskbar-Lyrics"
$exeName = "taskbar-lyrics.exe"
$destExe = Join-Path $installDir $exeName

# 定位源 exe：优先 -Source，否则默认取仓库构建产物（WSL 路径经 \\wsl.localhost 访问）
if (-not $Source) {
    $candidates = @(
        "\\wsl.localhost\Ubuntu\home\starl\ai-code\Taskbar-Lyrics\plugin\cpp\build\phase3\$exeName",
        "C:\Users\StarL\musicfox-e2e\bin\$exeName"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $Source = $c; break }
    }
}
if (-not $Source -or -not (Test-Path $Source)) {
    Write-Error "找不到 taskbar-lyrics.exe，请用 -Source 指定路径"
}

# 1. 拷贝（先停掉运行中的实例）
Get-Process -Name taskbar-lyrics -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Copy-Item $Source $destExe -Force
Unblock-File $destExe
Write-Host "[install] 已安装到 $destExe"

# 2. 注册开机自启（HKCU Run）
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
if ($NoAutostart) {
    Remove-ItemProperty -Path $runKey -Name "Taskbar-Lyrics" -ErrorAction SilentlyContinue
    Write-Host "[install] 已跳过开机自启（-NoAutostart）"
} else {
    Set-ItemProperty -Path $runKey -Name "Taskbar-Lyrics" -Value "`"$destExe`""
    Write-Host "[install] 已注册开机自启（HKCU Run: Taskbar-Lyrics）"
}

# 3. 信息
Write-Host ""
Write-Host "================ Taskbar-Lyrics 安装完成 ================"
Write-Host "  安装位置 : $installDir"
Write-Host "  开机自启 : $(if ($NoAutostart) { '关闭' } else { '开启' })"
Write-Host "  卸载方式 : 运行 uninstall.ps1"
Write-Host "  手动启动 : $destExe"
Write-Host "========================================================"
