#include "player/VideoFrameQueue.h"

// 视频帧数量通常不大，这里用 deque 能较自然地处理头部弹出与尾部追加。
void VideoFrameQueue::push(VideoFrameData frame)
{
    std::scoped_lock lock(m_mutex);
    m_frames.push_back(std::move(frame));
}

std::optional<VideoFrameData> VideoFrameQueue::peek() const
{
    std::scoped_lock lock(m_mutex);
    if (m_frames.empty()) {
        return std::nullopt;
    }
    return m_frames.front();
}

std::optional<VideoFrameData> VideoFrameQueue::pop()
{
    std::scoped_lock lock(m_mutex);
    if (m_frames.empty()) {
        return std::nullopt;
    }

    VideoFrameData frame = std::move(m_frames.front());
    m_frames.pop_front();
    return frame;
}

void VideoFrameQueue::clear()
{
    std::scoped_lock lock(m_mutex);
    m_frames.clear();
}

int VideoFrameQueue::size() const
{
    std::scoped_lock lock(m_mutex);
    return static_cast<int>(m_frames.size());
}
