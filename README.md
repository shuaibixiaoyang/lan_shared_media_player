# LAN Shared Media Player

基于 `Qt 6 + FFmpeg + OpenGL` 实现的桌面端局域网同看播放器。项目不依赖现成播放器控件，核心链路由 FFmpeg 负责解封装与解码，Qt 负责音频输出和桌面 UI，OpenGL 负责视频帧渲染，Qt Network 负责局域网房间、聊天、文件传输和播放同步。

## 功能特性

- 本地播放：打开本地媒体文件，支持播放、暂停、停止、进度跳转、音量、静音、全屏和倍速。
- 播放列表：支持多文件选择、拖拽导入、上一首/下一首、删除、清空、自动下一条和本地持久化。
- FFmpeg 解码：完成媒体解封装、音视频软解码和音频重采样。
- OpenGL 渲染：使用 Shader 将 `YUV420P` 视频帧转换并绘制到窗口。
- 局域网同看：房主创建房间，观众通过 IP 加入，同步当前视频、播放进度和倍速。
- 文件传输：房主将当前视频分块发送给观众，观众接收后自动缓存并播放。
- 实时聊天：房间内支持成员列表、系统消息和文本聊天。
- 同步优化：观众端对周期同步做阈值校正，减少频繁 seek 导致的闪烁和倒退。

## 技术栈

- `C++17`
- `Qt 6 Widgets`
- `Qt Multimedia`
- `Qt Network`
- `FFmpeg`
- `OpenGL / GLSL`
- `CMake`
- `MinGW`

## 项目结构

```text
src/
  audio/      QAudioSink 读取适配
  network/    局域网房间、聊天、文件传输、播放同步
  player/     播放控制器、FFmpeg 解码线程、音视频队列
  render/     OpenGL 视频显示控件
  ui/         主窗口、播放列表、同看聊天、动态背景
```

## 推荐环境

- Windows 10/11
- Qt `6.10.1` with `mingw_64`
- MinGW `13.1`
- CMake `3.24+`
- FFmpeg shared development package

默认 FFmpeg 路径：

```text
C:/ffmpeg-dev/ffmpeg-8.1-full_build-shared
```

如果你的 Qt 或 FFmpeg 安装路径不同，可以在 CMake 配置时覆盖 `CMAKE_PREFIX_PATH` 和 `FFMPEG_ROOT`。

## 构建

```powershell
.\build_local.ps1
```

构建产物默认生成到：

```text
C:\codex_work\qt_ffmpeg_player_build\QtFfmpegOpenGLPlayer.exe
```

## 运行

```powershell
.\run_local.ps1
```

也可以直接传入媒体文件路径：

```powershell
.\run_local.ps1 "D:\media\sample.mp4"
```

## 在 Qt Creator 中打开

项目目录包含中文路径时，部分 Windows 工具链可能出现 CMake/Ninja 兼容问题。建议通过脚本创建英文路径联接后打开：

```powershell
.\open_in_qtcreator.ps1
```

推荐 Kit：

```text
Qt 6.10.1 MinGW 64-bit
```

## 同看使用流程

1. 房主打开播放器，在 `同看聊天` 页点击 `创建`。
2. 观众点击 `加入`，输入房主显示的局域网 IP。
3. 房主打开视频文件。
4. 观众端自动接收视频文件，完成后加入当前播放进度。
5. 房主播放、暂停、seek、切换倍速时，观众端同步执行。

当前同看模式面向局域网直连。公网房间需要中转服务器或 NAT 穿透能力，当前版本暂不包含。

## 辅助脚本

```powershell
.\convert_to_player_mp4.ps1 "D:\media\input.mov"
```

将视频转为播放器更稳定支持的 `H.264 + AAC + yuv420p` MP4。

```powershell
.\convert_and_play.ps1 "D:\media\input.mov"
```

转码完成后自动启动播放器播放转换结果。

## 注意事项

- 仓库已忽略构建目录、Qt Creator 本地配置、运行日志和媒体样例文件。
- 大视频文件不建议提交到 GitHub。
- 当前文件传输协议使用 `JSON + Base64` 分块，适合课程设计、演示和短视频；大文件场景建议后续升级为二进制协议。
- 当前同步策略以体验稳定为优先，不追求严格帧级同步。

## 后续方向

- 文件传输改为二进制协议，支持取消、重试和断点续传。
- 引入公网中转服务或 WebSocket 服务端。
- 增加字幕、多音轨和硬件解码支持。
- 进一步优化同步算法，减少 seek，改用轻量播放速率微调。
