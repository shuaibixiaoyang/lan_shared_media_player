#pragma once

#include <QMutex>
#include <QObject>
#include <QAudioFormat>
#include <QString>
#include <QThread>

#include <atomic>
#include <memory>

#include "player/PlaybackTypes.h"

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct SwsContext;

class SharedAudioBuffer;
class VideoFrameQueue;

// 解码工作线程：负责用 FFmpeg 打开媒体、解码音视频并推送到共享缓冲区/队列。
class PlayerWorker : public QThread
{
    Q_OBJECT

public:
    PlayerWorker(std::shared_ptr<SharedAudioBuffer> audioBuffer,
                 std::shared_ptr<VideoFrameQueue> videoQueue,
                 const AudioOutputConfig &audioOutputConfig,
                 double playbackRate,
                 QObject *parent = nullptr);
    ~PlayerWorker() override;

    // 设置要打开的媒体路径，真正的打开动作发生在线程 run() 中。
    void openMedia(const QString &filePath);
    // 控制后台解码循环是否暂停读取新帧。
    void setPaused(bool paused);
    void setPlaybackRate(double playbackRate);
    // 请求线程尽快退出，析构和停止播放时使用。
    void stopWorker();
    // 请求跳转到指定毫秒位置，实际 seek 在线程循环中完成。
    void seekToMilliseconds(qint64 positionMs);

signals:
    void mediaLoaded(qint64 durationMs, bool hasAudio, bool hasVideo, int width, int height);
    void stateChanged(PlaybackState state);
    void errorOccurred(const QString &message);
    void statusMessage(const QString &message);
    void buffersFlushed();
    void audioBasePtsChanged(double ptsSeconds);
    void videoBasePtsChanged(double ptsSeconds);

protected:
    // 线程主循环：打开媒体、准备音视频流，然后不断读取 packet 并解码成帧。
    void run() override;

private:
    // 按顺序完成：打开输入、初始化流、准备重采样与缩放、循环解码。
    bool openInput();
    // 查找最佳音频/视频流，创建对应解码器上下文。
    bool prepareStreams();
    // 创建音频重采样器，把源音频转换为 Qt 音频设备支持的格式。
    bool prepareAudioResampler();
    // 创建视频缩放器，把源像素格式统一转换为 YUV420P。
    bool prepareVideoScaler();
    // 把一个 packet 送入对应解码器，并取出所有可用 frame。
    bool decodePacket(AVPacket *packet);
    // 文件末尾时冲刷解码器内部缓存，避免丢掉最后几帧。
    bool drainDecoder(AVCodecContext *codecContext, bool isAudioStream);
    // 处理一帧音频：重采样后写入 SharedAudioBuffer。
    bool handleDecodedAudioFrame(AVFrame *frame);
    // 处理一帧视频：转换为 YUV420P 后写入 VideoFrameQueue。
    bool handleDecodedVideoFrame(AVFrame *frame);
    // 执行挂起的 seek 请求，刷新解码器和外部缓冲并重置时钟基点。
    bool processPendingSeek();
    int effectiveOutputSampleRate() const;
    void releaseContexts();
    void clearQueues();
    void reportError(const QString &message);
    qint64 durationMilliseconds() const;

    std::shared_ptr<SharedAudioBuffer> m_audioBuffer;
    std::shared_ptr<VideoFrameQueue> m_videoQueue;

    QString m_filePath;
    mutable QMutex m_stateMutex;

    // 这些状态会被主线程修改，因此用互斥量保护。
    bool m_paused = false;
    double m_playbackRate = 1.0;
    std::atomic_bool m_stopRequested = false;
    bool m_seekRequested = false;
    qint64 m_seekTargetMs = 0;

    AVFormatContext *m_formatContext = nullptr;
    AVCodecContext *m_audioCodecContext = nullptr;
    AVCodecContext *m_videoCodecContext = nullptr;
    SwrContext *m_swrContext = nullptr;
    SwsContext *m_swsContext = nullptr;
    int m_audioStreamIndex = -1;
    int m_videoStreamIndex = -1;
    AVFrame *m_decodedFrame = nullptr;
    AVPacket *m_packet = nullptr;

    // 输出视频尺寸与音频目标格式在初始化后固定，供后续每帧转换使用。
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    QAudioFormat::SampleFormat m_outputSampleFormat = QAudioFormat::Int16;
    int m_outputSampleRate = 48000;
    int m_outputChannelCount = 2;
    bool m_audioBasePtsEmitted = false;
    bool m_videoBasePtsEmitted = false;
    bool m_loggedFirstAudioFrame = false;
    bool m_loggedFirstVideoFrame = false;
};
