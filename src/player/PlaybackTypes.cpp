#include "player/PlaybackTypes.h"

// 统一把内部状态转换成可展示字符串，便于 UI 与调试日志直接复用。
QString playbackStateToString(PlaybackState state)
{
    switch (state) {
    case PlaybackState::Idle:
        return QStringLiteral("Idle");
    case PlaybackState::Loading:
        return QStringLiteral("Loading");
    case PlaybackState::Playing:
        return QStringLiteral("Playing");
    case PlaybackState::Paused:
        return QStringLiteral("Paused");
    case PlaybackState::Stopped:
        return QStringLiteral("Stopped");
    case PlaybackState::Seeking:
        return QStringLiteral("Seeking");
    case PlaybackState::EndOfFile:
        return QStringLiteral("End Of File");
    case PlaybackState::Error:
        return QStringLiteral("Error");
    }

    return QStringLiteral("Unknown");
}
