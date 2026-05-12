param(
    [string]$MediaFile = ""
)

# 本脚本用于启动已经构建好的播放器，可选附带一个媒体文件路径。
$ErrorActionPreference = "Stop"

$exePath = "C:\codex_work\qt_ffmpeg_player_build\QtFfmpegOpenGLPlayer.exe"

if (-not (Test-Path $exePath)) {
    throw "Executable not found. Run .\build_local.ps1 first."
}

if ([string]::IsNullOrWhiteSpace($MediaFile)) {
    # 未传入文件时仅启动播放器主界面。
    Start-Process -FilePath $exePath
} else {
    # 传入文件时直接作为命令行参数交给程序打开。
    Start-Process -FilePath $exePath -ArgumentList @($MediaFile)
}
