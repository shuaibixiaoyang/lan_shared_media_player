#pragma once

#include <QByteArray>

#include <mutex>

// 线程安全的音频字节缓冲区：解码线程写入，音频输出设备线程读取。
class SharedAudioBuffer
{
public:
    // 解码线程追加重采样后的 PCM 数据。
    void append(const QByteArray &data);
    // 音频设备线程读取 PCM 数据用于播放。
    qint64 read(char *destination, qint64 maxLength);
    // 返回当前还没有被音频设备消费的字节数。
    qint64 bufferedBytes() const;
    // seek、停止或重开文件时清空缓冲。
    void clear();

private:
    // 读取偏移持续前进后，按需把未读数据收紧到数组头部，减少内存膨胀。
    void compactIfNeeded();

    mutable std::mutex m_mutex;
    QByteArray m_storage;
    qint64 m_readOffset = 0;
};
