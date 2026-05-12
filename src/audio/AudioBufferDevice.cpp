#include "audio/AudioBufferDevice.h"

#include <cstring>

#include "player/SharedAudioBuffer.h"

// AudioBufferDevice 只负责把播放器解码线程产生的音频数据交给 Qt 音频输出模块。
AudioBufferDevice::AudioBufferDevice(std::shared_ptr<SharedAudioBuffer> buffer, QObject *parent)
    : QIODevice(parent)
    , m_buffer(std::move(buffer))
{
}

bool AudioBufferDevice::isSequential() const
{
    return true;
}

bool AudioBufferDevice::open(OpenMode mode)
{
    return QIODevice::open(mode);
}

qint64 AudioBufferDevice::bytesAvailable() const
{
    if (!m_buffer) {
        return QIODevice::bytesAvailable();
    }

    // 这里返回共享缓冲区中还能被 QAudioSink 继续消费的字节数。
    return m_buffer->bufferedBytes() + QIODevice::bytesAvailable();
}

qint64 AudioBufferDevice::readData(char *data, qint64 maxSize)
{
    if (!m_buffer) {
        return 0;
    }

    const qint64 bytesRead = m_buffer->read(data, maxSize);
    if (bytesRead < maxSize) {
        // 音频设备请求的数据不足时补 0，避免底层设备读到未初始化内存产生噪声。
        std::memset(data + bytesRead, 0, static_cast<size_t>(maxSize - bytesRead));
        return maxSize;
    }

    return bytesRead;
}

qint64 AudioBufferDevice::writeData(const char *, qint64)
{
    return -1;
}
