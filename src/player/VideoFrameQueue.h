#pragma once

#include <optional>
#include <deque>
#include <mutex>

#include "player/PlaybackTypes.h"

// 线程安全的视频帧队列：后台解码线程写入，主线程渲染器按时间顺序读取。
class VideoFrameQueue
{
public:
    // 解码线程把转换好的视频帧按时间顺序放入队列。
    void push(VideoFrameData frame);
    // 渲染线程只查看下一帧时间戳，不立即移除。
    std::optional<VideoFrameData> peek() const;
    // 渲染线程确认该帧应该显示时再弹出。
    std::optional<VideoFrameData> pop();
    void clear();
    int size() const;

private:
    mutable std::mutex m_mutex;
    std::deque<VideoFrameData> m_frames;
};
