param(
    [string]$QtPrefix = "C:\Qt\6.10.1\mingw_64",
    [string]$FfmpegRoot = "C:\ffmpeg-dev\ffmpeg-8.1-full_build-shared"
)

# 本脚本用于在固定目录下创建源码映射、配置 CMake，并生成本地可运行版本。
$ErrorActionPreference = "Stop"

$workspaceRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$linkRoot = "C:\codex_work"
$sourceLink = Join-Path $linkRoot "qt_ffmpeg_player_src"
$buildDir = Join-Path $linkRoot "qt_ffmpeg_player_build"

New-Item -ItemType Directory -Force -Path $linkRoot | Out-Null

if (Test-Path $sourceLink) {
    $sourceItem = Get-Item $sourceLink -Force
    if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        # 如果已存在联接点，优先按联接点方式删除，避免误删真实目录内容。
        cmd /c "rmdir `"$sourceLink`""
    } else {
        Remove-Item -LiteralPath $sourceLink -Force -Recurse
    }
}

cmd /c "mklink /J `"$sourceLink`" `"$workspaceRoot`""

$cmakeBin = "C:\Qt\Tools\CMake_64\bin"
$ninjaBin = "C:\Qt\Tools\Ninja"
$mingwBin = "C:\Qt\Tools\mingw1310_64\bin"
$qtBin = Join-Path $QtPrefix "bin"

$env:Path = "$cmakeBin;$ninjaBin;$mingwBin;$qtBin;$env:Path"

# 显式指定 Qt 和 FFmpeg 根目录，确保在命令行环境下也能稳定找到依赖。
cmake -S $sourceLink -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_PREFIX_PATH=$QtPrefix" "-DFFMPEG_ROOT=$FfmpegRoot"
cmake --build $buildDir

Write-Host ""
Write-Host "Build completed."
Write-Host "Executable: $buildDir\QtFfmpegOpenGLPlayer.exe"
