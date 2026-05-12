#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QCloseEvent>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QShowEvent>
#include <QSlider>
#include <QElapsedTimer>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

#include "player/PlaybackTypes.h"

class PlayerController;
class AnimatedBackgroundWidget;
class VideoWidget;
class WatchPartyManager;
class QLineEdit;

// 主窗口负责组织播放器 UI、播放列表和同看房间交互。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // 对外暴露的打开媒体入口，支持 main() 启动参数和 UI 操作复用。
    void openMediaFile(const QString &filePath);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    // 处理本地打开、播放控制、播放列表、同看房间和播放器状态变化等 UI 事件。
    void onOpenClicked();
    void onPreviousClicked();
    void onPlayPauseClicked();
    void onNextClicked();
    void onStopClicked();
    void onPlaylistSelectionChanged();
    void onPlaylistItemActivated(QListWidgetItem *item);
    void onDeletePlaylistItem();
    void onClearPlaylist();
    void onPlaylistContextMenuRequested(const QPoint &position);
    void onOpenPlaylistItemInExplorer();
    void onCreateRoomClicked();
    void onJoinRoomClicked();
    void onLeaveRoomClicked();
    void onSendChatClicked();
    void onChatReturnPressed();
    void onChatMessageReceived(const QString &sender, const QString &text, bool isLocal);
    void onRoomStatusChanged(const QString &statusText);
    void onPeerListChanged(const QStringList &peers);
    void onRemoteOpenMediaRequested(const QString &filePath, qint64 positionMs, double playbackRate);
    void onRemotePlaybackCommandReceived(const QString &command, qint64 positionMs, double playbackRate);
    void onMediaTransferProgress(const QString &fileName, qint64 receivedBytes, qint64 totalBytes);
    void onMediaFileReceived(const QString &filePath, qint64 positionMs, double playbackRate);
    void onPlaybackRateChanged(int index);
    void onMuteClicked();
    void onVolumeChanged(int value);
    void onFullscreenClicked();
    void onSliderPressed();
    void onSliderReleased();
    void onPositionUpdated(qint64 positionMs, qint64 durationMs);
    void onDurationChanged(qint64 durationMs);
    void onStateChanged(PlaybackState state);
    void onErrorOccurred(const QString &message);
    void onStatusMessage(const QString &message);
    void onWatchPartySyncTimer();

private:
    // 以下方法分别负责界面样式、交互连线、播放列表持久化与同看同步逻辑。
    void applyTheme();
    void applyWindowChrome();
    // 构建主窗口所有控件、布局和初始状态。
    void buildUi();
    // 统一连接 UI、播放器控制器和同看管理器的信号槽。
    void connectSignals();
    // 播放列表中指定下标的文件，并同步给同看房间。
    void playPlaylistItem(int index);
    int currentPlaylistIndex() const;
    void updatePlayButtonText();
    void updateStatusPill(PlaybackState state);
    void updateVolumeUi(int value);
    void updateFullscreenButton();
    void appendToPlaylist(const QString &filePath);
    int appendToPlaylistAndReturnIndex(const QString &filePath, bool selectItem);
    void addMediaFilesToPlaylist(const QStringList &filePaths, bool playFirstFile);
    bool isSupportedMediaFile(const QString &filePath) const;
    void loadPlaylistState();
    void savePlaylistState() const;
    QString playlistStateFilePath() const;
    // 房主把当前媒体路径和播放上下文同步给观众。
    void sendWatchPartyMediaSync(const QString &filePath);
    // 房主发送媒体文件本体，解决观众本地没有该文件的问题。
    void sendWatchPartyMediaFile(const QString &filePath);
    // 房主广播播放控制指令，观众端收到后执行对应操作。
    void sendWatchPartyPlaybackCommand(const QString &command);
    void setPlaybackRateCombo(double playbackRate);
    qint64 currentSliderPositionMs() const;
    double currentPlaybackRate() const;
    QString defaultDisplayName() const;
    void updateTimeLabel(qint64 positionMs, qint64 durationMs);
    QString formatTime(qint64 milliseconds) const;

    PlayerController *m_playerController = nullptr;
    WatchPartyManager *m_watchPartyManager = nullptr;
    AnimatedBackgroundWidget *m_backgroundWidget = nullptr;
    VideoWidget *m_videoWidget = nullptr;
    QListWidget *m_playlistWidget = nullptr;
    QComboBox *m_speedComboBox = nullptr;
    QToolButton *m_openButton = nullptr;
    QToolButton *m_previousButton = nullptr;
    QToolButton *m_playPauseButton = nullptr;
    QToolButton *m_nextButton = nullptr;
    QToolButton *m_stopButton = nullptr;
    QToolButton *m_volumeButton = nullptr;
    QToolButton *m_fullscreenButton = nullptr;
    QToolButton *m_deletePlaylistButton = nullptr;
    QToolButton *m_clearPlaylistButton = nullptr;
    QToolButton *m_createRoomButton = nullptr;
    QToolButton *m_joinRoomButton = nullptr;
    QToolButton *m_leaveRoomButton = nullptr;
    QToolButton *m_sendChatButton = nullptr;
    QSlider *m_positionSlider = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_roomStatusLabel = nullptr;
    QListWidget *m_peerListWidget = nullptr;
    QTextEdit *m_chatView = nullptr;
    QLineEdit *m_chatInput = nullptr;

    PlaybackState m_currentState = PlaybackState::Idle;
    bool m_userIsSeeking = false;
    // 远端同步回放期间临时屏蔽反向广播，避免双方来回触发“回声同步”。
    bool m_remoteSyncInProgress = false;
    QTimer m_watchPartySyncTimer;
    QElapsedTimer m_lastRemoteCorrectionTimer;
    QElapsedTimer m_remoteMediaStabilizeTimer;
    QString m_currentMediaPath;
    int m_lastVolumeValue = 85;
    bool m_windowChromeApplied = false;
    bool m_playlistSignalBlocked = false;
    bool m_playlistActivationBlocked = false;
    QString m_contextMenuFilePath;
};
