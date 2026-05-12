#include "player/PlayerWorker.h"

#include <QDebug>
#include <QMutexLocker>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "player/SharedAudioBuffer.h"
#include "player/VideoFrameQueue.h"

namespace
{
// 把 FFmpeg 错误码翻译成人类可读字符串，方便直接给 UI 和日志使用。
QString ffmpegErrorString(int errorCode)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, errorCode);
    return QString::fromLocal8Bit(buffer);
}

// 将时间戳 pts 按流的 time_base 转换成秒，便于控制器建立统一时钟。
double secondsFromPts(long long pts, AVRational timeBase)
{
    if (pts == AV_NOPTS_VALUE) {
        return 0.0;
    }
    return static_cast<double>(pts) * av_q2d(timeBase);
}

// Qt 音频格式与 FFmpeg 音频格式之间的映射。
AVSampleFormat avSampleFormatFromQt(QAudioFormat::SampleFormat sampleFormat)
{
    switch (sampleFormat) {
    case QAudioFormat::UInt8:
        return AV_SAMPLE_FMT_U8;
    case QAudioFormat::Int16:
        return AV_SAMPLE_FMT_S16;
    case QAudioFormat::Int32:
        return AV_SAMPLE_FMT_S32;
    case QAudioFormat::Float:
        return AV_SAMPLE_FMT_FLT;
    case QAudioFormat::Unknown:
    default:
        return AV_SAMPLE_FMT_S16;
    }
}

// 不同输出采样格式下每个样本占用的字节数。
int bytesPerSample(QAudioFormat::SampleFormat sampleFormat)
{
    switch (sampleFormat) {
    case QAudioFormat::UInt8:
        return 1;
    case QAudioFormat::Int16:
        return 2;
    case QAudioFormat::Int32:
    case QAudioFormat::Float:
        return 4;
    case QAudioFormat::Unknown:
    default:
        return 2;
    }
}
}

PlayerWorker::PlayerWorker(std::shared_ptr<SharedAudioBuffer> audioBuffer,
                           std::shared_ptr<VideoFrameQueue> videoQueue,
                           const AudioOutputConfig &audioOutputConfig,
                           double playbackRate,
                           QObject *parent)
    : QThread(parent)
    , m_audioBuffer(std::move(audioBuffer))
    , m_videoQueue(std::move(videoQueue))
{
    m_outputSampleFormat = audioOutputConfig.sampleFormat;
    m_outputSampleRate = audioOutputConfig.sampleRate;
    m_outputChannelCount = audioOutputConfig.channelCount;
    m_playbackRate = std::max(0.25, playbackRate);
}

PlayerWorker::~PlayerWorker()
{
    stopWorker();
    wait();
}

void PlayerWorker::openMedia(const QString &filePath)
{
    QMutexLocker locker(&m_stateMutex);
    m_filePath = filePath;
}

void PlayerWorker::setPaused(bool paused)
{
    QMutexLocker locker(&m_stateMutex);
    m_paused = paused;
}

void PlayerWorker::setPlaybackRate(double playbackRate)
{
    QMutexLocker locker(&m_stateMutex);
    m_playbackRate = std::max(0.25, playbackRate);
}

void PlayerWorker::stopWorker()
{
    m_stopRequested.store(true);
}

void PlayerWorker::seekToMilliseconds(qint64 positionMs)
{
    QMutexLocker locker(&m_stateMutex);
    m_seekTargetMs = std::max<qint64>(0, positionMs);
    m_seekRequested = true;
}

void PlayerWorker::run()
{
    qDebug() << "[PlayerWorker] run() start for" << m_filePath;
    // 线程每次启动都重置首帧日志和时钟基点发射标记。
    m_stopRequested.store(false);
    m_audioBasePtsEmitted = false;
    m_videoBasePtsEmitted = false;
    m_loggedFirstAudioFrame = false;
    m_loggedFirstVideoFrame = false;

    if (!openInput()) {
        qDebug() << "[PlayerWorker] openInput() failed";
        releaseContexts();
        return;
    }

    if (m_seekRequested) {
        qDebug() << "[PlayerWorker] applying pending start seek before mediaLoaded";
        if (!processPendingSeek()) {
            releaseContexts();
            return;
        }
    }

    qDebug() << "[PlayerWorker] openInput() ok. durationMs=" << durationMilliseconds()
             << "audioStreamIndex=" << m_audioStreamIndex
             << "videoStreamIndex=" << m_videoStreamIndex
             << "videoSize=" << m_videoWidth << "x" << m_videoHeight;
    emit mediaLoaded(durationMilliseconds(),
                     m_audioStreamIndex >= 0,
                     m_videoStreamIndex >= 0,
                     m_videoWidth,
                     m_videoHeight);
    qDebug() << "[PlayerWorker] mediaLoaded emitted";
    emit stateChanged(PlaybackState::Playing);
    qDebug() << "[PlayerWorker] Playing emitted";

    while (!m_stopRequested.load()) {
        {
            QMutexLocker locker(&m_stateMutex);
            if (m_paused) {
                locker.unlock();
                // 暂停时不做忙等，短暂休眠让出 CPU。
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }

        if (processPendingSeek()) {
            emit stateChanged(PlaybackState::Playing);
            continue;
        }

        const int readResult = av_read_frame(m_formatContext, m_packet);
        if (readResult == AVERROR_EOF) {
            if (m_audioCodecContext) {
                if (!drainDecoder(m_audioCodecContext, true)) {
                    break;
                }
            }
            if (m_videoCodecContext) {
                if (!drainDecoder(m_videoCodecContext, false)) {
                    break;
                }
            }
            emit stateChanged(PlaybackState::EndOfFile);
            break;
        }

        if (readResult < 0) {
            reportError(QStringLiteral("Failed to read frame: %1").arg(ffmpegErrorString(readResult)));
            break;
        }

        if (!decodePacket(m_packet)) {
            av_packet_unref(m_packet);
            break;
        }
        av_packet_unref(m_packet);

        // 限制缓冲深度，避免 seek/暂停后后台还囤积过多旧数据导致响应变慢。
        while (!m_stopRequested.load() &&
               (m_videoQueue->size() > 24 || m_audioBuffer->bufferedBytes() > 48000 * 4 * 3)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    releaseContexts();
}

bool PlayerWorker::openInput()
{
    releaseContexts();

    const QByteArray filePathUtf8 = m_filePath.toUtf8();
    qDebug() << "[PlayerWorker] avformat_open_input() with" << m_filePath;
    int result = avformat_open_input(&m_formatContext, filePathUtf8.constData(), nullptr, nullptr);
    if (result < 0) {
        reportError(QStringLiteral("Failed to open media file: %1").arg(ffmpegErrorString(result)));
        return false;
    }

    qDebug() << "[PlayerWorker] avformat_find_stream_info() start";
    result = avformat_find_stream_info(m_formatContext, nullptr);
    if (result < 0) {
        reportError(QStringLiteral("Failed to read stream info: %1").arg(ffmpegErrorString(result)));
        return false;
    }
    qDebug() << "[PlayerWorker] avformat_find_stream_info() ok";

    emit statusMessage(QStringLiteral("Opened file: %1").arg(m_filePath));
    return prepareStreams();
}

bool PlayerWorker::prepareStreams()
{
    qDebug() << "[PlayerWorker] prepareStreams()";
    m_audioStreamIndex = av_find_best_stream(m_formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    m_videoStreamIndex = av_find_best_stream(m_formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (m_audioStreamIndex < 0 && m_videoStreamIndex < 0) {
        reportError(QStringLiteral("No playable audio or video stream was found."));
        return false;
    }

    if (m_audioStreamIndex >= 0) {
        qDebug() << "[PlayerWorker] preparing audio stream" << m_audioStreamIndex;
        AVStream *audioStream = m_formatContext->streams[m_audioStreamIndex];
        const AVCodec *audioCodec = avcodec_find_decoder(audioStream->codecpar->codec_id);
        if (!audioCodec) {
            reportError(QStringLiteral("No audio decoder is available for the selected file."));
            return false;
        }

        m_audioCodecContext = avcodec_alloc_context3(audioCodec);
        if (!m_audioCodecContext ||
            avcodec_parameters_to_context(m_audioCodecContext, audioStream->codecpar) < 0 ||
            avcodec_open2(m_audioCodecContext, audioCodec, nullptr) < 0) {
            reportError(QStringLiteral("Failed to initialize audio decoder."));
            return false;
        }

        if (!prepareAudioResampler()) {
            return false;
        }
    }

    if (m_videoStreamIndex >= 0) {
        qDebug() << "[PlayerWorker] preparing video stream" << m_videoStreamIndex;
        AVStream *videoStream = m_formatContext->streams[m_videoStreamIndex];
        const AVCodec *videoCodec = avcodec_find_decoder(videoStream->codecpar->codec_id);
        if (!videoCodec) {
            reportError(QStringLiteral("No video decoder is available for the selected file."));
            return false;
        }

        m_videoCodecContext = avcodec_alloc_context3(videoCodec);
        if (!m_videoCodecContext ||
            avcodec_parameters_to_context(m_videoCodecContext, videoStream->codecpar) < 0 ||
            avcodec_open2(m_videoCodecContext, videoCodec, nullptr) < 0) {
            reportError(QStringLiteral("Failed to initialize video decoder."));
            return false;
        }

        m_videoWidth = m_videoCodecContext->width;
        m_videoHeight = m_videoCodecContext->height;

        if (!prepareVideoScaler()) {
            return false;
        }
    }

    m_decodedFrame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_decodedFrame || !m_packet) {
        reportError(QStringLiteral("Failed to allocate FFmpeg frame or packet objects."));
        return false;
    }

    qDebug() << "[PlayerWorker] prepareStreams() complete";

    return true;
}

bool PlayerWorker::prepareAudioResampler()
{
    // 输入文件声道布局可能不完整，这里兜底成双声道以保证后续重采样能继续。
    AVChannelLayout sourceLayout{};
    if (av_channel_layout_copy(&sourceLayout, &m_audioCodecContext->ch_layout) < 0 ||
        sourceLayout.nb_channels <= 0) {
        av_channel_layout_uninit(&sourceLayout);
        av_channel_layout_default(&sourceLayout, 2);
    }

    AVChannelLayout targetLayout{};
    av_channel_layout_default(&targetLayout, m_outputChannelCount);

    int result = swr_alloc_set_opts2(
        &m_swrContext,
        &targetLayout,
        avSampleFormatFromQt(m_outputSampleFormat),
        effectiveOutputSampleRate(),
        &sourceLayout,
        m_audioCodecContext->sample_fmt,
        m_audioCodecContext->sample_rate,
        0,
        nullptr);

    if (result < 0 || !m_swrContext) {
        av_channel_layout_uninit(&sourceLayout);
        av_channel_layout_uninit(&targetLayout);
        reportError(QStringLiteral("Failed to create the audio resampler."));
        return false;
    }

    result = swr_init(m_swrContext);
    av_channel_layout_uninit(&sourceLayout);
    av_channel_layout_uninit(&targetLayout);
    if (result < 0) {
        reportError(QStringLiteral("Failed to initialize the audio resampler."));
        return false;
    }

    return true;
}

bool PlayerWorker::prepareVideoScaler()
{
    // 统一转换成紧凑的 YUV420P，方便 OpenGL 侧直接按三张纹理上传。
    m_swsContext = sws_getContext(
        m_videoCodecContext->width,
        m_videoCodecContext->height,
        m_videoCodecContext->pix_fmt,
        m_videoCodecContext->width,
        m_videoCodecContext->height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (!m_swsContext) {
        reportError(QStringLiteral("Failed to initialize the video scaler."));
        return false;
    }

    return true;
}

bool PlayerWorker::decodePacket(AVPacket *packet)
{
    AVCodecContext *codecContext = nullptr;
    bool isAudioPacket = false;

    if (packet->stream_index == m_audioStreamIndex) {
        codecContext = m_audioCodecContext;
        isAudioPacket = true;
    } else if (packet->stream_index == m_videoStreamIndex) {
        codecContext = m_videoCodecContext;
    } else {
        return true;
    }

    if (!codecContext) {
        return true;
    }

    // 每个 packet 送入解码器后，尽可能把当前可取出的 frame 全部收完。
    int result = avcodec_send_packet(codecContext, packet);
    if (result < 0) {
        reportError(QStringLiteral("Failed to send packet to decoder: %1").arg(ffmpegErrorString(result)));
        return false;
    }

    while (result >= 0 && !m_stopRequested.load()) {
        result = avcodec_receive_frame(codecContext, m_decodedFrame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return true;
        }

        if (result < 0) {
            reportError(QStringLiteral("Failed to receive decoded frame: %1").arg(ffmpegErrorString(result)));
            return false;
        }

        if (isAudioPacket) {
            if (!handleDecodedAudioFrame(m_decodedFrame)) {
                return false;
            }
        } else {
            if (!handleDecodedVideoFrame(m_decodedFrame)) {
                return false;
            }
        }

        av_frame_unref(m_decodedFrame);
    }

    return true;
}

bool PlayerWorker::drainDecoder(AVCodecContext *codecContext, bool isAudioStream)
{
    int result = avcodec_send_packet(codecContext, nullptr);
    if (result < 0) {
        reportError(QStringLiteral("Failed to flush decoder: %1").arg(ffmpegErrorString(result)));
        return false;
    }

    while (!m_stopRequested.load()) {
        result = avcodec_receive_frame(codecContext, m_decodedFrame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return true;
        }

        if (result < 0) {
            reportError(QStringLiteral("Failed to drain decoder: %1").arg(ffmpegErrorString(result)));
            return false;
        }

        // 文件读完后继续把解码器内部滞留帧吐干净，避免尾部音视频丢失。
        if (isAudioStream) {
            if (!handleDecodedAudioFrame(m_decodedFrame)) {
                return false;
            }
        } else {
            if (!handleDecodedVideoFrame(m_decodedFrame)) {
                return false;
            }
        }

        av_frame_unref(m_decodedFrame);
    }

    return true;
}

bool PlayerWorker::handleDecodedAudioFrame(AVFrame *frame)
{
    if (!m_loggedFirstAudioFrame) {
        qDebug() << "[PlayerWorker] first audio frame decoded:"
                 << "nb_samples=" << frame->nb_samples
                 << "sample_rate=" << frame->sample_rate
                 << "pts=" << secondsFromPts(frame->best_effort_timestamp,
                                            m_formatContext->streams[m_audioStreamIndex]->time_base);
        m_loggedFirstAudioFrame = true;
    }

    const int destinationSamples = av_rescale_rnd(
        swr_get_delay(m_swrContext, frame->sample_rate) + frame->nb_samples,
        effectiveOutputSampleRate(),
        frame->sample_rate,
        AV_ROUND_UP);

    const int bytesPerOutputSample = bytesPerSample(m_outputSampleFormat);
    QByteArray outputBuffer(destinationSamples * m_outputChannelCount * bytesPerOutputSample, Qt::Uninitialized);
    uint8_t *outputData[] = {reinterpret_cast<uint8_t *>(outputBuffer.data())};

    const int convertedSamples = swr_convert(
        m_swrContext,
        outputData,
        destinationSamples,
        const_cast<const uint8_t **>(frame->data),
        frame->nb_samples);

    if (convertedSamples < 0) {
        reportError(QStringLiteral("Failed to convert audio samples."));
        return false;
    }

    const int convertedBytes = convertedSamples * m_outputChannelCount * bytesPerOutputSample;
    outputBuffer.resize(convertedBytes);
    if (!outputBuffer.isEmpty()) {
        // 第一帧音频会作为控制器主时钟的起点。
        if (!m_audioBasePtsEmitted) {
            const double basePts = secondsFromPts(
                frame->best_effort_timestamp,
                m_formatContext->streams[m_audioStreamIndex]->time_base);
            emit audioBasePtsChanged(basePts);
            m_audioBasePtsEmitted = true;
        }
        m_audioBuffer->append(outputBuffer);
    }

    return true;
}

bool PlayerWorker::handleDecodedVideoFrame(AVFrame *frame)
{
    if (!m_swsContext) {
        return false;
    }

    if (!m_loggedFirstVideoFrame) {
        qDebug() << "[PlayerWorker] first video frame decoded:"
                 << "width=" << frame->width
                 << "height=" << frame->height
                 << "format=" << frame->format
                 << "pts=" << secondsFromPts(frame->best_effort_timestamp,
                                            m_formatContext->streams[m_videoStreamIndex]->time_base);
        m_loggedFirstVideoFrame = true;
    }

    VideoFrameData videoFrame;
    videoFrame.width = m_videoCodecContext->width;
    videoFrame.height = m_videoCodecContext->height;
    videoFrame.ptsSeconds = secondsFromPts(
        frame->best_effort_timestamp,
        m_formatContext->streams[m_videoStreamIndex]->time_base);

    const int yPlaneSize = videoFrame.width * videoFrame.height;
    const int uvPlaneSize = (videoFrame.width / 2) * (videoFrame.height / 2);
    videoFrame.yPlane.resize(yPlaneSize);
    videoFrame.uPlane.resize(uvPlaneSize);
    videoFrame.vPlane.resize(uvPlaneSize);

    uint8_t *destinationData[4] = {
        reinterpret_cast<uint8_t *>(videoFrame.yPlane.data()),
        reinterpret_cast<uint8_t *>(videoFrame.uPlane.data()),
        reinterpret_cast<uint8_t *>(videoFrame.vPlane.data()),
        nullptr
    };
    int destinationLinesize[4] = {
        videoFrame.width,
        videoFrame.width / 2,
        videoFrame.width / 2,
        0
    };

    sws_scale(
        m_swsContext,
        frame->data,
        frame->linesize,
        0,
        videoFrame.height,
        destinationData,
        destinationLinesize);

    // 渲染层要求紧凑 YUV420P，因此每帧入队前都先做一次统一格式转换。
    if (!m_videoBasePtsEmitted) {
        emit videoBasePtsChanged(videoFrame.ptsSeconds);
        m_videoBasePtsEmitted = true;
    }

    m_videoQueue->push(std::move(videoFrame));
    return true;
}

bool PlayerWorker::processPendingSeek()
{
    qint64 seekTargetMs = 0;
    {
        QMutexLocker locker(&m_stateMutex);
        if (!m_seekRequested) {
            return false;
        }

        seekTargetMs = m_seekTargetMs;
        m_seekRequested = false;
    }

    // seek 后不仅要刷新解码器，还要清空外部缓冲并重置新的时钟基点。
    emit stateChanged(PlaybackState::Seeking);

    const int64_t seekTarget = seekTargetMs * AV_TIME_BASE / 1000;
    const int result = av_seek_frame(m_formatContext, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        reportError(QStringLiteral("Seek failed: %1").arg(ffmpegErrorString(result)));
        return false;
    }

    if (m_audioCodecContext) {
        avcodec_flush_buffers(m_audioCodecContext);
        if (m_swrContext) {
            swr_free(&m_swrContext);
        }
        if (!prepareAudioResampler()) {
            return false;
        }
    }

    if (m_videoCodecContext) {
        avcodec_flush_buffers(m_videoCodecContext);
    }

    clearQueues();
    m_audioBasePtsEmitted = false;
    m_videoBasePtsEmitted = false;
    emit buffersFlushed();
    emit audioBasePtsChanged(static_cast<double>(seekTargetMs) / 1000.0);
    emit videoBasePtsChanged(static_cast<double>(seekTargetMs) / 1000.0);
    return true;
}

int PlayerWorker::effectiveOutputSampleRate() const
{
    // 通过调整重采样后的采样率来实现变速播放，速度越快，输出采样率越低。
    const double clampedRate = std::max(0.25, m_playbackRate);
    return std::max(8'000, static_cast<int>(std::lround(static_cast<double>(m_outputSampleRate) / clampedRate)));
}

void PlayerWorker::releaseContexts()
{
    // FFmpeg 相关资源按“先子对象、后上下文”的顺序逐个释放，避免悬空引用。
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_decodedFrame) {
        av_frame_free(&m_decodedFrame);
    }
    if (m_audioCodecContext) {
        avcodec_free_context(&m_audioCodecContext);
    }
    if (m_videoCodecContext) {
        avcodec_free_context(&m_videoCodecContext);
    }
    if (m_swrContext) {
        swr_free(&m_swrContext);
    }
    if (m_swsContext) {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
    }
}

void PlayerWorker::clearQueues()
{
    m_audioBuffer->clear();
    m_videoQueue->clear();
}

void PlayerWorker::reportError(const QString &message)
{
    emit errorOccurred(message);
    emit stateChanged(PlaybackState::Error);
}

qint64 PlayerWorker::durationMilliseconds() const
{
    if (!m_formatContext || m_formatContext->duration <= 0) {
        return 0;
    }
    // FFmpeg 总时长单位是 AV_TIME_BASE，这里统一换算成毫秒供 UI 使用。
    return m_formatContext->duration / (AV_TIME_BASE / 1000);
}
