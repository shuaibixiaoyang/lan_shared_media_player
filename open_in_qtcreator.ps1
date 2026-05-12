param()

$ErrorActionPreference = "Stop"

$qtCreatorExe = "C:\Qt\Tools\QtCreator\bin\qtcreator.exe"
$projectFile = "C:\codex_work\qt_ffmpeg_player_src\CMakeLists.txt"

if (-not (Test-Path $qtCreatorExe)) {
    throw "Qt Creator was not found at $qtCreatorExe"
}

if (-not (Test-Path $projectFile)) {
    throw "Project link path was not found at $projectFile. Run .\build_local.ps1 first."
}

Start-Process -FilePath $qtCreatorExe -ArgumentList @($projectFile)
