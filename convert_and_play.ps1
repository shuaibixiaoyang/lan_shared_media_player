param(
    [Parameter(Mandatory = $true)]
    [string]$InputFile
)

# 先转码成播放器更容易稳定处理的 MP4，再自动启动本地播放器进行播放。
$ErrorActionPreference = "Stop"

$workspaceRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$convertScript = Join-Path $workspaceRoot "convert_to_player_mp4.ps1"
$runScript = Join-Path $workspaceRoot "run_local.ps1"

& $convertScript $InputFile

$resolvedInput = (Resolve-Path $InputFile).Path
$convertedFile = Join-Path $workspaceRoot ("{0}.player.mp4" -f [System.IO.Path]::GetFileNameWithoutExtension($resolvedInput))

if (-not (Test-Path $convertedFile)) {
    throw "Converted file was not created: $convertedFile"
}

& $runScript $convertedFile
