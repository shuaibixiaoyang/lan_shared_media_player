#include "player/SharedAudioBuffer.h"

#include <algorithm>
#include <cstring>

// 该缓冲区的目标不是复杂调度，而是提供一个简单稳定的 PCM 生产者/消费者桥梁。
void SharedAudioBuffer::append(const QByteArray &data)
{
    if (data.isEmpty()) {
        return;
    }

    std::scoped_lock lock(m_mutex);
    compactIfNeeded();
    m_storage.append(data);
}

qint64 SharedAudioBuffer::read(char *destination, qint64 maxLength)
{
    std::scoped_lock lock(m_mutex);

    const qint64 available = m_storage.size() - m_readOffset;
    const qint64 bytesToRead = std::min(available, maxLength);

    if (bytesToRead <= 0) {
        return 0;
    }

    std::memcpy(destination, m_storage.constData() + m_readOffset, static_cast<size_t>(bytesToRead));
    m_readOffset += bytesToRead;
    compactIfNeeded();
    return bytesToRead;
}

qint64 SharedAudioBuffer::bufferedBytes() const
{
    std::scoped_lock lock(m_mutex);
    return m_storage.size() - m_readOffset;
}

void SharedAudioBuffer::clear()
{
    std::scoped_lock lock(m_mutex);
    m_storage.clear();
    m_readOffset = 0;
}

void SharedAudioBuffer::compactIfNeeded()
{
    if (m_readOffset <= 0) {
        return;
    }

    // 不必每次 read 都搬运数据，只在偏移积累到一定程度后再压缩一次。
    if (m_readOffset >= m_storage.size() || m_readOffset > 65536) {
        m_storage.remove(0, static_cast<qsizetype>(m_readOffset));
        m_readOffset = 0;
    }
}
