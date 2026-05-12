#pragma once

#include <QAudioDevice>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <memory>

#include "player/PlaybackTypes.h"

class QAudioSink;
class AudioBufferDevice;
class PlayerWorker;
class SharedAudioBuffer;
class VideoFrameQueue;
class VideoWidget;

// 播放控制器位于 UI 和解码线程之间，负责组织音频输出、视频渲染和播放时钟。
class PlayerController : public QObject
{
    Q_OBJECT

public:
    explicit PlayerController(VideoWidget *videoWidget, QObject *parent = nullptr);
    ~PlayerController() override;

    // 打开一个媒体文件：清空旧状态、创建解码线程，并启动音频/视频播放流程。
    void openFile(const QString &filePath);
    // 根据当前状态在播放和暂停之间切换；停止状态下会重新打开当前文件。
    void togglePlayPause();
    // 停止播放并释放当前解码线程与缓冲区。
    void stop();
    // 用户拖动进度条后的精确跳转，会清空当前画面和缓冲。
    void seek(qint64 positionMs);
    // 网络同步使用的平滑跳转，保留当前画面以减少闪烁。
    void seekSmooth(qint64 positionMs);
    void setVolume(float volume);
    float volume() const;
    void setPlaybackRate(double playbackRate);
    double playbackRate() const;

signals:
    void positionUpdated(qint64 positionMs, qint64 durationMs);
    void durationChanged(qint64 durationMs);
    void stateChanged(PlaybackState state);
    void errorOccurred(const QString &message);
    void statusMessage(const QString &message);

private slots:
    // 按当前播放时钟从视频队列取出应该显示的帧，并提交给 OpenGL 控件。
    void onRenderTimer();
    // 周期性把当前播放进度同步给 UI。
    void onPositionTimer();
    // 解码线程读取到媒体信息后，初始化时长、音频输出和渲染计时器。
    void onWorkerMediaLoaded(qint64 durationMs, bool hasAudio, bool hasVideo, int width, int height);
    void onWorkerStateChanged(PlaybackState state);
    void onWorkerError(const QString &message);
    void onWorkerBuffersFlushed();
    void onAudioBasePtsChanged(double ptsSeconds);
    void onVideoBasePtsChanged(double ptsSeconds);

private:
    // 根据系统默认音频设备挑选一个尽量稳定的输出格式。
    AudioOutputConfig selectAudioOutputConfig() const;
    // 重新创建 QAudioSink 和 AudioBufferDevice，用于打开新文件或变速后重建音频链路。
    void rebuildAudioOutput();
    void clearPlaybackBuffers(bool clearVideoFrame = true);
    void destroyWorker();
    void setState(PlaybackState state);
    // 视频专用时钟在纯视频文件、音频时钟未启动或 seek 后短暂兜底使用。
    void resetVideoOnlyClock();
    void pauseVideoOnlyClock();
    void resumeVideoOnlyClock();
    double currentPlaybackSeconds() const;

    VideoWidget *m_videoWidget = nullptr;
    PlayerWorker *m_worker = nullptr;
    std::shared_ptr<SharedAudioBuffer> m_audioBuffer;
    std::shared_ptr<VideoFrameQueue> m_videoQueue;
    AudioBufferDevice *m_audioDevice = nullptr;
    QAudioSink *m_audioSink = nullptr;
    QAudioDevice m_outputDevice;

    QTimer m_renderTimer;
    QTimer m_positionTimer;

    PlaybackState m_state = PlaybackState::Idle;
    AudioOutputConfig m_audioConfig;
    QString m_currentFilePath;
    double m_playbackRate = 1.0;
    qint64 m_pendingSeekAfterLoadMs = -1;
    float m_volumeLevel = 0.85f;

    qint64 m_durationMs = 0;
    bool m_hasAudio = false;
    bool m_hasVideo = false;
    bool m_hasPresentedVideoFrame = false;

    bool m_audioClockValid = false;
    double m_audioBasePtsSeconds = 0.0;

    // 视频专用时钟用于“纯视频”或音频时钟尚未启动时的兜底同步。
    bool m_videoClockValid = false;
    double m_videoBasePtsSeconds = 0.0;
    qint64 m_videoAccumulatedMs = 0;
    bool m_videoClockRunning = false;
    QElapsedTimer m_videoResumeTimer;
};
