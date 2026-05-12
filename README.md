# Qt + FFmpeg + OpenGL Player

This project is a desktop media player built with Qt Widgets, FFmpeg, and OpenGL.

## Highlights

- Local file playback
- FFmpeg demuxing and software decoding
- Qt audio output
- OpenGL YUV420P shader rendering
- Play, pause, stop, and seek

## Recommended Local Environment

- Qt `6.10.1` with `mingw_64`
- MinGW `13.1`
- CMake `3.24+`
- FFmpeg shared development package in `C:/ffmpeg-dev/ffmpeg-8.1-full_build-shared`

## Build

```powershell
.\build_local.ps1
```

## Run

```powershell
.\run_local.ps1
```

You can also pass a media file path on startup:

```powershell
.\run_local.ps1 D:\media\sample.mp4
```

## Open In Qt Creator

To avoid Windows toolchain issues caused by the original non-ASCII workspace path, open the project in Qt Creator through the ASCII junction path:

```powershell
.\open_in_qtcreator.ps1
```

Inside Qt Creator, prefer the `Qt 6.10.1 MinGW 64-bit` kit and use the generated CMake presets.

## Convert A Local Video For The Player

If you already have a local video file and want the safest playback format for this player, run:

```powershell
.\convert_to_player_mp4.ps1 D:\path\to\your_video.mp4
```

This creates a compatible file named `original_name.player.mp4` in the project folder.

You can also convert and launch the player in one step:

```powershell
.\convert_and_play.ps1 D:\path\to\your_video.mp4
```

## Notes

- The project is currently focused on the MVP defined in the requirements document.
- Audio is used as the primary sync clock when an audio stream exists.
- The deployment step copies FFmpeg runtime DLLs next to the executable automatically.
- The helper build script uses an ASCII-only junction path under `C:\codex_work` because some Windows CMake/Ninja combinations are unstable when the original project path contains non-ASCII characters.
- The generated release executable is placed under `C:\codex_work\qt_ffmpeg_player_build\QtFfmpegOpenGLPlayer.exe`.
- Qt Creator is intentionally configured around the Release preset only, so it does not fall back to a broken Debug toolchain configuration.
