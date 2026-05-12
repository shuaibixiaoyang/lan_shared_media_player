#pragma once

#include <QIODevice>

#include <memory>

class SharedAudioBuffer;

// 把共享音频缓冲区包装成 QIODevice，供 QAudioSink 以“设备读取”的方式取 PCM 数据。
class AudioBufferDevice : public QIODevice
{
    Q_OBJECT

public:
    explicit AudioBufferDevice(std::shared_ptr<SharedAudioBuffer> buffer, QObject *parent = nullptr);

    // QAudioSink 会把该设备当作连续音频流读取，所以这里声明它是顺序设备。
    bool isSequential() const override;
    bool open(OpenMode mode) override;
    // 告诉 Qt 音频后端当前还有多少 PCM 数据可以读取。
    qint64 bytesAvailable() const override;

protected:
    // 从共享音频缓冲区读出数据；数据不足时由实现层补静音，避免播放噪声。
    qint64 readData(char *data, qint64 maxSize) override;
    // 这个设备只负责给音频后端读数据，不接受写入。
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    // 实际存放解码后音频数据的线程安全缓冲区。
    std::shared_ptr<SharedAudioBuffer> m_buffer;
};
