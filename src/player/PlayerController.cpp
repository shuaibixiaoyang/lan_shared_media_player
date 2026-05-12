#include "player/PlayerController.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QDebug>
#include <QMediaDevices>

#include "audio/AudioBufferDevice.h"
#include "player/PlayerWorker.h"
#include "player/SharedAudioBuffer.h"
#include "player/VideoFrameQueue.h"
#include "render/VideoWidget.h"

namespace
{
// 让状态文本转换逻辑集中到一个小辅助函数，便于后续替换展示文案。
QString playbackStateString(PlaybackState state)
{
    return playbackStateToString(state);
}
}

PlayerController::PlayerController(VideoWidget *videoWidget, QObject *parent)
    : QObject(parent)
    , m_videoWidget(videoWidget)
    , m_audioBuffer(std::make_shared<SharedAudioBuffer>())
    , m_videoQueue(std::make_shared<VideoFrameQueue>())
{
    m_outputDevice = QMediaDevices::defaultAudioOutput();
    m_audioConfig = selectAudioOutputConfig();
    qDebug() << "[PlayerController] selected audio config:"
             << "sampleRate=" << m_audioConfig.sampleRate
             << "channels=" << m_audioConfig.channelCount
             << "sampleFormat=" << m_audioConfig.sampleFormat;

    m_renderTimer.setInterval(10);
    m_positionTimer.setInterval(100);

    // 渲染定时器负责推进视频显示，位置定时器负责刷新进度条和时间标签。
    connect(&m_renderTimer, &QTimer::timeout, this, &PlayerController::onRenderTimer);
    connect(&m_positionTimer, &QTimer::timeout, this, &PlayerController::onPositionTimer);

    rebuildAudioOutput();
}

PlayerController::~PlayerController()
{
    stop();
    destroyWorker();
    delete m_audioSink;
}

void PlayerController::openFile(const QString &filePath)
{
    // 这里会完整重建播放链路：旧 worker 停掉，音视频缓冲清空，
    // 新 worker 在后台用 FFmpeg 解码，主线程继续负责音频输出和画面刷新。
    qDebug() << "[PlayerController] openFile:" << filePath << "playbackRate=" << m_playbackRate;
    m_currentFilePath = filePath;
    stop();
    clearPlaybackBuffers();
    destroyWorker();
    rebuildAudioOutput();

    // 重新打开文件前先彻底回收旧 worker 和缓冲区，避免跨文件残留数据串音串画。
    m_durationMs = 0;
    m_hasAudio = false;
    m_hasVideo = false;
    m_hasPresentedVideoFrame = false;
    m_audioClockValid = false;
    m_audioBasePtsSeconds = 0.0;
    m_videoClockValid = false;
    m_videoBasePtsSeconds = 0.0;
    m_videoAccumulatedMs = 0;

    setState(PlaybackState::Loading);

    m_worker = new PlayerWorker(m_audioBuffer, m_videoQueue, m_audioConfig, m_playbackRate, this);
    connect(m_worker, &PlayerWorker::mediaLoaded, this, &PlayerController::onWorkerMediaLoaded);
    connect(m_worker, &PlayerWorker::stateChanged, this, &PlayerController::onWorkerStateChanged);
    connect(m_worker, &PlayerWorker::errorOccurred, this, &PlayerController::onWorkerError);
    connect(m_worker, &PlayerWorker::buffersFlushed, this, &PlayerController::onWorkerBuffersFlushed);
    connect(m_worker, &PlayerWorker::audioBasePtsChanged, this, &PlayerController::onAudioBasePtsChanged);
    connect(m_worker, &PlayerWorker::videoBasePtsChanged, this, &PlayerController::onVideoBasePtsChanged);
    connect(m_worker, &PlayerWorker::statusMessage, this, &PlayerController::statusMessage);

    if (m_pendingSeekAfterLoadMs >= 0) {
        m_worker->seekToMilliseconds(m_pendingSeekAfterLoadMs);
        m_pendingSeekAfterLoadMs = -1;
    }

    m_worker->openMedia(filePath);
    m_worker->start();
    qDebug() << "[PlayerController] worker started";
}

void PlayerController::togglePlayPause()
{
    if ((m_state == PlaybackState::Idle || m_state == PlaybackState::Stopped) && !m_currentFilePath.isEmpty()) {
        // 空闲或停止状态下再次点击播放，等价于从头重新打开当前文件。
        openFile(m_currentFilePath);
        return;
    }

    if (m_state == PlaybackState::Error) {
        return;
    }

    if (m_state == PlaybackState::Playing) {
        if (m_audioSink) {
            m_audioSink->suspend();
        }
        // 暂停时同时停掉本地视频时钟和界面刷新，避免画面继续推进。
        pauseVideoOnlyClock();
        m_renderTimer.stop();
        m_positionTimer.stop();
        if (m_worker) {
            m_worker->setPaused(true);
        }
        setState(PlaybackState::Paused);
        return;
    }

    if (m_state == PlaybackState::Paused || m_state == PlaybackState::EndOfFile) {
        if (m_state == PlaybackState::EndOfFile) {
            openFile(m_currentFilePath);
            return;
        }

        if (m_audioSink) {
            m_audioSink->resume();
        }
        resumeVideoOnlyClock();
        m_renderTimer.start();
        m_positionTimer.start();
        if (m_worker) {
            m_worker->setPaused(false);
        }
        setState(PlaybackState::Playing);
    }
}

void PlayerController::stop()
{
    m_renderTimer.stop();
    m_positionTimer.stop();

    if (m_audioSink) {
        m_audioSink->stop();
    }

    if (m_worker) {
        m_worker->stopWorker();
        m_worker->wait();
        m_worker->deleteLater();
        m_worker = nullptr;
    }

    clearPlaybackBuffers();
    resetVideoOnlyClock();
    m_durationMs = 0;
    emit durationChanged(0);
    emit positionUpdated(0, 0);
    setState(PlaybackState::Stopped);
}

void PlayerController::seek(qint64 positionMs)
{
    if (!m_worker || m_durationMs <= 0) {
        return;
    }

    // 拖动 seek 时清空音视频缓冲，让 worker 从新位置重新建立同步基准。
    setState(PlaybackState::Seeking);
    clearPlaybackBuffers(true);
    if (m_audioSink) {
        m_audioSink->reset();
        m_audioSink->start(m_audioDevice);
    }

    m_worker->seekToMilliseconds(positionMs);
}

void PlayerController::seekSmooth(qint64 positionMs)
{
    if (!m_worker || m_durationMs <= 0) {
        return;
    }

    // 平滑 seek 会保留当前视频帧，减少网络同步或细微校正时的闪屏感。
    setState(PlaybackState::Seeking);
    clearPlaybackBuffers(false);
    if (m_audioSink) {
        m_audioSink->reset();
        m_audioSink->start(m_audioDevice);
    }

    m_worker->seekToMilliseconds(positionMs);
}

void PlayerController::setVolume(float volume)
{
    m_volumeLevel = std::clamp(volume, 0.0f, 1.0f);
    if (m_audioSink) {
        m_audioSink->setVolume(m_volumeLevel);
    }
}

float PlayerController::volume() const
{
    if (!m_audioSink) {
        return 0.0f;
    }

    return m_audioSink->volume();
}

void PlayerController::setPlaybackRate(double playbackRate)
{
    // 这个播放器用“重采样输出采样率”的方式实现倍速。
    // 为了让音频格式和解码状态都一致，变速时会记录当前位置并重开文件。
    const double clampedRate = std::clamp(playbackRate, 0.25, 4.0);
    if (std::abs(m_playbackRate - clampedRate) < 0.001) {
        return;
    }

    if (!m_worker || m_currentFilePath.isEmpty() || m_durationMs <= 0) {
        m_playbackRate = clampedRate;
        return;
    }

    // 变速后直接按当前位置重开媒体，借此重建音频输出采样率与解码状态。
    const qint64 currentPositionMs = std::max<qint64>(0, static_cast<qint64>(currentPlaybackSeconds() * 1000.0));
    qDebug() << "[PlayerController] setPlaybackRate old=" << m_playbackRate
             << "new=" << clampedRate
             << "positionMs=" << currentPositionMs;
    m_playbackRate = clampedRate;
    m_pendingSeekAfterLoadMs = currentPositionMs;
    openFile(m_currentFilePath);
    emit statusMessage(QStringLiteral("Playback speed set to %1x").arg(m_playbackRate, 0, 'f', 2));
}

double PlayerController::playbackRate() const
{
    return m_playbackRate;
}

void PlayerController::onRenderTimer()
{
    // 渲染线程不直接解码，只按播放时钟消费 VideoFrameQueue。
    // 如果某些帧已经落后于当前时钟，就连续弹出直到追到应该显示的帧。
    if (!m_hasVideo) {
        return;
    }

    // 视频渲染始终追随当前播放时钟；若帧已稍微落后，就立刻消费掉避免卡顿。
    while (true) {
        const auto nextFrame = m_videoQueue->peek();
        if (!nextFrame.has_value()) {
            break;
        }

        // 首帧解码出来后立即显示，避免音频时钟尚未起跳时界面一直黑屏。
        if (!m_hasPresentedVideoFrame) {
            auto firstFrame = m_videoQueue->pop();
            if (!firstFrame.has_value()) {
                break;
            }
            qDebug() << "[PlayerController] presenting first video frame pts=" << firstFrame->ptsSeconds;
            m_videoWidget->presentFrame(*firstFrame);
            m_hasPresentedVideoFrame = true;
            continue;
        }

        const double playbackSeconds = currentPlaybackSeconds();
        if (nextFrame->ptsSeconds > playbackSeconds + 0.02) {
            break;
        }

        auto frameToDisplay = m_videoQueue->pop();
        if (!frameToDisplay.has_value()) {
            break;
        }
        m_videoWidget->presentFrame(*frameToDisplay);
        m_hasPresentedVideoFrame = true;
    }
}

void PlayerController::onPositionTimer()
{
    const qint64 positionMs = std::min(m_durationMs, static_cast<qint64>(currentPlaybackSeconds() * 1000.0));
    emit positionUpdated(positionMs, m_durationMs);
}

void PlayerController::onWorkerMediaLoaded(qint64 durationMs, bool hasAudio, bool hasVideo, int, int)
{
    qDebug() << "[PlayerController] onWorkerMediaLoaded durationMs=" << durationMs
             << "hasAudio=" << hasAudio
             << "hasVideo=" << hasVideo
             << "pendingSeek=" << m_pendingSeekAfterLoadMs;
    m_durationMs = durationMs;
    m_hasAudio = hasAudio;
    m_hasVideo = hasVideo;
    emit durationChanged(durationMs);

    // 媒体信息准备完成后，正式启动音频设备与渲染计时器。
    if (m_audioSink) {
        m_audioSink->start(m_audioDevice);
        m_audioSink->resume();
    }

    resetVideoOnlyClock();
    if (m_hasVideo) {
        resumeVideoOnlyClock();
    }
    m_renderTimer.start();
    m_positionTimer.start();
    setState(PlaybackState::Playing);
}

void PlayerController::onWorkerStateChanged(PlaybackState state)
{
    qDebug() << "[PlayerController] onWorkerStateChanged:" << playbackStateToString(state);
    if (state == PlaybackState::EndOfFile) {
        m_renderTimer.stop();
        m_positionTimer.stop();
        if (m_audioSink) {
            m_audioSink->suspend();
        }
        pauseVideoOnlyClock();
    }

    if (state == PlaybackState::Playing && m_state == PlaybackState::Seeking) {
        if (m_audioSink) {
            m_audioSink->resume();
        }
        resumeVideoOnlyClock();
        m_renderTimer.start();
        m_positionTimer.start();
    }

    setState(state);
}

void PlayerController::onWorkerError(const QString &message)
{
    qDebug() << "[PlayerController] onWorkerError:" << message;
    setState(PlaybackState::Error);
    emit errorOccurred(message);
}

void PlayerController::onWorkerBuffersFlushed()
{
    clearPlaybackBuffers(false);
    m_hasPresentedVideoFrame = false;
    m_audioClockValid = false;
    m_audioBasePtsSeconds = 0.0;
    m_videoClockValid = false;
    m_videoBasePtsSeconds = 0.0;
    resetVideoOnlyClock();
}

void PlayerController::onAudioBasePtsChanged(double ptsSeconds)
{
    m_audioClockValid = true;
    m_audioBasePtsSeconds = ptsSeconds;
}

void PlayerController::onVideoBasePtsChanged(double ptsSeconds)
{
    m_videoClockValid = true;
    m_videoBasePtsSeconds = ptsSeconds;
    resetVideoOnlyClock();
}

AudioOutputConfig PlayerController::selectAudioOutputConfig() const
{
    AudioOutputConfig config;

    QAudioFormat preferred = m_outputDevice.preferredFormat();
    config.sampleRate = preferred.sampleRate() > 0 ? preferred.sampleRate() : 48000;
    config.channelCount = preferred.channelCount() > 0 ? preferred.channelCount() : 2;
    config.sampleFormat = preferred.sampleFormat() != QAudioFormat::Unknown ? preferred.sampleFormat() : QAudioFormat::Int16;

    // MVP 阶段优先选择 Windows/Qt 上兼容性较好的输出格式，降低设备适配风险。
    if (config.sampleFormat == QAudioFormat::Float || config.sampleFormat == QAudioFormat::Unknown) {
        config.sampleFormat = QAudioFormat::Int16;
    }

    return config;
}

void PlayerController::rebuildAudioOutput()
{
    delete m_audioSink;
    m_audioSink = nullptr;

    delete m_audioDevice;
    m_audioDevice = nullptr;

    QAudioFormat format;
    format.setSampleFormat(m_audioConfig.sampleFormat);
    format.setSampleRate(m_audioConfig.sampleRate);
    format.setChannelCount(m_audioConfig.channelCount);

    if (!m_outputDevice.isFormatSupported(format)) {
        qWarning() << "[PlayerController] requested audio format not supported, falling back to preferred format";
        const QAudioFormat preferred = m_outputDevice.preferredFormat();
        m_audioConfig.sampleRate = preferred.sampleRate();
        m_audioConfig.channelCount = preferred.channelCount();
        m_audioConfig.sampleFormat = preferred.sampleFormat() != QAudioFormat::Unknown
            ? preferred.sampleFormat()
            : QAudioFormat::Int16;

        format.setSampleRate(m_audioConfig.sampleRate);
        format.setChannelCount(m_audioConfig.channelCount);
        format.setSampleFormat(m_audioConfig.sampleFormat);
    }

    m_audioDevice = new AudioBufferDevice(m_audioBuffer, this);
    m_audioDevice->open(QIODevice::ReadOnly);
    m_audioSink = new QAudioSink(m_outputDevice, format, this);
    // 缓冲区不要设得过大，否则音频时钟太晚开始推进，会拖慢首帧显示时机。
    m_audioSink->setBufferSize(48000);
    m_audioSink->setVolume(m_volumeLevel);
    qDebug() << "[PlayerController] QAudioSink format:"
             << "sampleRate=" << format.sampleRate()
             << "channels=" << format.channelCount()
             << "sampleFormat=" << format.sampleFormat();
}

void PlayerController::clearPlaybackBuffers(bool clearVideoFrame)
{
    m_audioBuffer->clear();
    m_videoQueue->clear();
    m_hasPresentedVideoFrame = false;
    if (m_videoWidget && clearVideoFrame) {
        m_videoWidget->clearFrame();
    }
}

void PlayerController::destroyWorker()
{
    if (!m_worker) {
        return;
    }

    m_worker->stopWorker();
    m_worker->wait();
    m_worker->deleteLater();
    m_worker = nullptr;
}

void PlayerController::setState(PlaybackState state)
{
    if (m_state == state) {
        return;
    }

    m_state = state;
    emit stateChanged(state);
    emit statusMessage(QStringLiteral("State changed: %1").arg(playbackStateString(state)));
}

void PlayerController::resetVideoOnlyClock()
{
    m_videoAccumulatedMs = 0;
    m_videoClockRunning = false;
    m_videoResumeTimer.invalidate();
}

void PlayerController::pauseVideoOnlyClock()
{
    if (!m_videoClockRunning || !m_videoResumeTimer.isValid()) {
        return;
    }

    m_videoAccumulatedMs += m_videoResumeTimer.elapsed();
    m_videoClockRunning = false;
}

void PlayerController::resumeVideoOnlyClock()
{
    if (m_videoClockRunning) {
        return;
    }

    m_videoResumeTimer.restart();
    m_videoClockRunning = true;
}

double PlayerController::currentPlaybackSeconds() const
{
    // 有音频时优先使用 QAudioSink 已播放时长作为主时钟，音画同步更稳定。
    // 没有音频或音频时钟尚未开始推进时，退回到本地视频计时器。
    if (m_hasAudio && m_audioSink && m_audioClockValid) {
        // 以音频设备已经真正播放掉的时长作为主时钟，更适合同步音画。
        const qint64 processedUs = m_audioSink->processedUSecs();

        // 启动初期或 seek 后音频时钟可能暂时不动，此时退回到本地视频时钟兜底。
        if (processedUs > 20'000 || !m_hasVideo || !m_videoClockValid) {
            return m_audioBasePtsSeconds + (static_cast<double>(processedUs) / 1'000'000.0) * m_playbackRate;
        }
    }

    if (m_hasVideo && m_videoClockValid) {
        // 纯视频场景没有音频设备可参考，因此退回到本地累计计时。
        qint64 elapsedMs = m_videoAccumulatedMs;
        if (m_videoClockRunning && m_videoResumeTimer.isValid()) {
            elapsedMs += m_videoResumeTimer.elapsed();
        }
        return m_videoBasePtsSeconds + (static_cast<double>(elapsedMs) / 1000.0) * m_playbackRate;
    }

    return 0.0;
}
