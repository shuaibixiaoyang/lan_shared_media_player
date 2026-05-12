#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QString>

// 播放器核心状态，供 UI、控制器和后台解码线程之间统一沟通。
enum class PlaybackState
{
    Idle,
    Loading,
    Playing,
    Paused,
    Stopped,
    Seeking,
    EndOfFile,
    Error
};

// Qt 音频输出的目标格式，解码线程会尽量把 FFmpeg 输出重采样到这里。
struct AudioOutputConfig
{
    QAudioFormat::SampleFormat sampleFormat = QAudioFormat::Int16;
    int sampleRate = 48000;
    int channelCount = 2;
};

// 供 OpenGL 渲染层消费的一帧视频数据，使用紧凑的 YUV420P 三平面布局。
struct VideoFrameData
{
    // 一帧视频使用 YUV420P 三平面保存，OpenGL 渲染时分别上传为三张纹理。
    int width = 0;
    int height = 0;
    double ptsSeconds = 0.0;
    QByteArray yPlane;
    QByteArray uPlane;
    QByteArray vPlane;
};

QString playbackStateToString(PlaybackState state);
