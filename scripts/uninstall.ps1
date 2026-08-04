# ============================================================
#  Taskbar-Lyrics 卸载脚本（安装模式）
#  用法: pwsh -File uninstall.ps1
#  删除: 安装目录 + HKCU Run 自启项
#  便携模式（手动放置的 exe）不受影响
# ============================================================
$ErrorActionPreference = "Continue"

$installDir = Join-Path $env:LOCALAPPDATA "Programs\Taskbar-Lyrics"

# 1. 停进程
Get-Process -Name taskbar-lyrics -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

# 2. 删自启项
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
Remove-ItemProperty -Path $runKey -Name "Taskbar-Lyrics" -ErrorAction SilentlyContinue
Write-Host "[uninstall] 已移除开机自启项"

# 3. 删目录
if (Test-Path $installDir) {
    Remove-Item $installDir -Recurse -Force
    Write-Host "[uninstall] 已删除 $installDir"
} else {
    Write-Host "[uninstall] 安装目录不存在（可能已是便携模式）"
}

Write-Host "[uninstall] 完成。"
