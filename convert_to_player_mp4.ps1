param(
    [Parameter(Mandatory = $true)]
    [string]$InputFile,
    [string]$OutputFile = ""
)

# 把任意输入媒体尽量转成播放器更友好的 H.264 + AAC + yuv420p MP4。
$ErrorActionPreference = "Stop"

function Resolve-FfmpegExecutable {
    # 依次尝试常见安装路径和 PATH 中的 ffmpeg，尽量减少环境配置门槛。
    $candidates = @(
        "C:\ffmpeg-dev\ffmpeg-8.1-full_build-shared\bin\ffmpeg.exe",
        "C:\Program Files (x86)\ffmpeg\bin\ffmpeg.exe",
        "ffmpeg"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -eq "ffmpeg") {
            $command = Get-Command ffmpeg -ErrorAction SilentlyContinue
            if ($command) {
                return $command.Source
            }
        } elseif (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "ffmpeg.exe was not found. Please install FFmpeg or update the script candidates."
}

$workspaceRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-Path $InputFile)) {
    throw "Input file not found: $InputFile"
}

$resolvedInput = (Resolve-Path $InputFile).Path

if ([string]::IsNullOrWhiteSpace($OutputFile)) {
    $inputBaseName = [System.IO.Path]::GetFileNameWithoutExtension($resolvedInput)
    # 默认输出到项目目录下，方便后续 run 脚本直接找到生成文件。
    $OutputFile = Join-Path $workspaceRoot "$inputBaseName.player.mp4"
}

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputFile)
$ffmpegExe = Resolve-FfmpegExecutable

Write-Host "Input : $resolvedInput"
Write-Host "Output: $resolvedOutput"
Write-Host "FFmpeg: $ffmpegExe"
Write-Host ""

$ffmpegArgs = @(
    "-y",
    "-i",
    $resolvedInput,
    "-map",
    "0:v:0",
    "-map",
    "0:a?",
    # 输出格式尽量贴合当前播放器链路，降低非常规编码导致的兼容性问题。
    "-c:v",
    "libx264",
    "-preset",
    "medium",
    "-crf",
    "23",
    "-pix_fmt",
    "yuv420p",
    "-c:a",
    "aac",
    "-b:a",
    "192k",
    "-movflags",
    "+faststart",
    $resolvedOutput
)

& $ffmpegExe @ffmpegArgs

if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg transcoding failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "Transcoding completed: $resolvedOutput"
