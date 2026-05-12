#include "ui/MainWindow.h"
#include "ui/AnimatedBackgroundWidget.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHostInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QGraphicsDropShadowEffect>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QDebug>
#include <QDesktopServices>
#include <QMimeData>
#include <QUrl>

#include "network/WatchPartyManager.h"
#include "player/PlayerController.h"
#include "render/VideoWidget.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // 主窗口初始化顺序：先建 UI，再套主题，最后连接信号和恢复播放列表状态。
    buildUi();
    applyTheme();
    connectSignals();
    loadPlaylistState();
    setAcceptDrops(true);
    resize(1280, 860);
    setMinimumSize(960, 680);
    setWindowTitle("Luma Player");
}

MainWindow::~MainWindow() = default;

void MainWindow::openMediaFile(const QString &filePath)
{
    if (!filePath.isEmpty()) {
        qDebug() << "[MainWindow] openMediaFile:" << filePath;
        // 打开本地文件后，同时把当前媒体同步给同看房间中的其他成员。
        m_currentMediaPath = filePath;
        appendToPlaylist(filePath);
        m_playerController->openFile(filePath);
        sendWatchPartyMediaSync(filePath);
        sendWatchPartyMediaFile(filePath);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    savePlaylistState();
    m_playerController->stop();
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }

    // 只接受当前播放器明确支持的本地媒体文件拖入。
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile() && isSupportedMediaFile(url.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
    }

    event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    QStringList playableFiles;

    // 拖入多个文件时全部加入播放列表，并默认播放第一个可播放文件。
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }

        const QString filePath = url.toLocalFile();
        if (!isSupportedMediaFile(filePath)) {
            continue;
        }

        playableFiles.append(filePath);
    }

    if (!playableFiles.isEmpty()) {
        addMediaFilesToPlaylist(playableFiles, true);
        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);

    if (!m_windowChromeApplied) {
        applyWindowChrome();
        m_windowChromeApplied = true;
    }
}

void MainWindow::onOpenClicked()
{
    const QStringList filePaths = QFileDialog::getOpenFileNames(
        this,
        tr("打开一个或多个媒体文件"),
        QString(),
        tr("Media Files (*.mp4 *.mkv *.avi *.mov *.mp3 *.aac);;All Files (*.*)"));

    if (!filePaths.isEmpty()) {
        addMediaFilesToPlaylist(filePaths, true);
    }
}

void MainWindow::onPreviousClicked()
{
    const int index = currentPlaylistIndex();
    if (index > 0) {
        playPlaylistItem(index - 1);
    }
}

void MainWindow::onPlayPauseClicked()
{
    const bool wasPlaying = (m_currentState == PlaybackState::Playing);
    m_playerController->togglePlayPause();
    // 房主点击播放/暂停后，要把控制指令广播给同看成员。
    if (wasPlaying) {
        sendWatchPartyPlaybackCommand(QStringLiteral("pause"));
    } else {
        sendWatchPartyPlaybackCommand(QStringLiteral("play"));
    }
}

void MainWindow::onNextClicked()
{
    const int index = currentPlaylistIndex();
    if (index >= 0 && index + 1 < m_playlistWidget->count()) {
        playPlaylistItem(index + 1);
    }
}

void MainWindow::onStopClicked()
{
    m_playerController->stop();
    if (m_watchPartyManager && m_watchPartyManager->role() == WatchPartyRole::Host && !m_currentMediaPath.isEmpty()) {
        m_watchPartyManager->updateHostMediaState(m_currentMediaPath, currentSliderPositionMs(), currentPlaybackRate(), false);
    }
    // 停止后同步更新房主媒体状态，避免新加入成员误以为仍在播放。
    sendWatchPartyPlaybackCommand(QStringLiteral("stop"));
}

void MainWindow::onPlaylistSelectionChanged()
{
    if (m_playlistSignalBlocked || !m_playlistWidget->currentItem()) {
        return;
    }

    // 选中列表项只更新“当前焦点”和展示信息，不立即开始播放。
    const QString filePath = m_playlistWidget->currentItem()->data(Qt::UserRole).toString();
    qDebug() << "[MainWindow] playlist selection changed:" << filePath;
    if (!filePath.isEmpty()) {
        m_currentMediaPath = filePath;
        savePlaylistState();
    }
}

void MainWindow::onPlaylistItemActivated(QListWidgetItem *item)
{
    if (m_playlistSignalBlocked || item == nullptr) {
        return;
    }

    // 双击或点击触发时，才真正切换到新的媒体文件。
    const QString filePath = item->data(Qt::UserRole).toString();
    qDebug() << "[MainWindow] playlist item activated:" << filePath;
    if (!filePath.isEmpty()) {
        m_currentMediaPath = filePath;
        openMediaFile(filePath);
    }
}

void MainWindow::onDeletePlaylistItem()
{
    const int index = currentPlaylistIndex();
    if (index < 0) {
        return;
    }

    const QString removedPath = m_playlistWidget->item(index)->data(Qt::UserRole).toString();
    delete m_playlistWidget->takeItem(index);

    // 删除的是当前播放项时，自动切到下一项；如果列表空了就回到空状态。
    if (removedPath == m_currentMediaPath) {
        if (m_playlistWidget->count() > 0) {
            const int nextIndex = std::min(index, m_playlistWidget->count() - 1);
            playPlaylistItem(nextIndex);
        } else {
            m_playerController->stop();
            m_currentMediaPath.clear();
            statusBar()->showMessage(tr("播放列表已清空"), 3000);
        }
    }

    savePlaylistState();
}

void MainWindow::onClearPlaylist()
{
    m_playlistSignalBlocked = true;
    m_playlistWidget->clear();
    m_playlistSignalBlocked = false;

    m_playerController->stop();
    m_currentMediaPath.clear();
    savePlaylistState();
}

void MainWindow::onPlaylistContextMenuRequested(const QPoint &position)
{
    QListWidgetItem *item = m_playlistWidget->itemAt(position);
    QMenu menu(this);

    QAction *playAction = nullptr;
    QAction *deleteAction = nullptr;
    QAction *openInExplorerAction = nullptr;
    if (item) {
        m_contextMenuFilePath = item->data(Qt::UserRole).toString();
        playAction = menu.addAction(tr("播放"));
        deleteAction = menu.addAction(tr("删除"));
        openInExplorerAction = menu.addAction(tr("在资源管理器中打开"));
        menu.addSeparator();
    } else {
        m_contextMenuFilePath.clear();
    }

    QAction *clearAction = menu.addAction(tr("清空列表"));

    QAction *selected = menu.exec(m_playlistWidget->viewport()->mapToGlobal(position));
    if (!selected) {
        return;
    }

    if (selected == playAction && item) {
        playPlaylistItem(m_playlistWidget->row(item));
    } else if (selected == deleteAction && item) {
        m_playlistSignalBlocked = true;
        m_playlistWidget->setCurrentItem(item);
        m_playlistSignalBlocked = false;
        onDeletePlaylistItem();
    } else if (selected == openInExplorerAction && item) {
        onOpenPlaylistItemInExplorer();
    } else if (selected == clearAction) {
        onClearPlaylist();
    }
}

void MainWindow::onOpenPlaylistItemInExplorer()
{
    if (m_contextMenuFilePath.isEmpty()) {
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_contextMenuFilePath).absolutePath()));
}

void MainWindow::onCreateRoomClicked()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("创建同看房间"), tr("你的昵称："),
                                               QLineEdit::Normal, defaultDisplayName(), &ok);
    if (!ok) {
        return;
    }

    const bool started = m_watchPartyManager->startHost(45454, name);
    if (started && !m_currentMediaPath.isEmpty()) {
        sendWatchPartyMediaSync(m_currentMediaPath);
        sendWatchPartyMediaFile(m_currentMediaPath);
    }
}

void MainWindow::onJoinRoomClicked()
{
    bool ok = false;
    const QString host = QInputDialog::getText(this, tr("加入同看房间"), tr("房主 IP 或地址："),
                                               QLineEdit::Normal, QStringLiteral("127.0.0.1"), &ok);
    if (!ok || host.trimmed().isEmpty()) {
        return;
    }

    const QString name = QInputDialog::getText(this, tr("加入同看房间"), tr("你的昵称："),
                                               QLineEdit::Normal, defaultDisplayName(), &ok);
    if (!ok) {
        return;
    }

    m_watchPartyManager->joinHost(host.trimmed(), 45454, name);
}

void MainWindow::onLeaveRoomClicked()
{
    m_watchPartyManager->disconnectFromRoom();
}

void MainWindow::onSendChatClicked()
{
    if (!m_watchPartyManager->isConnected()) {
        QMessageBox::information(this, tr("同看房间"), tr("请先创建或加入一个房间。"));
        return;
    }

    const QString text = m_chatInput->text();
    m_watchPartyManager->sendChatMessage(text);
    m_chatInput->clear();
}

void MainWindow::onChatReturnPressed()
{
    onSendChatClicked();
}

void MainWindow::onChatMessageReceived(const QString &sender, const QString &text, bool isLocal)
{
    const QString color = isLocal ? QStringLiteral("#2f7bdc") : QStringLiteral("#244067");
    m_chatView->append(QStringLiteral("<p style='margin:6px 0;'><b style='color:%1'>%2</b><br/><span style='color:#51627d'>%3</span></p>")
                           .arg(color, sender.toHtmlEscaped(), text.toHtmlEscaped()));
}

void MainWindow::onRoomStatusChanged(const QString &statusText)
{
    m_roomStatusLabel->setText(statusText);
    statusBar()->showMessage(statusText, 4000);
}

void MainWindow::onPeerListChanged(const QStringList &peers)
{
    m_peerListWidget->clear();
    for (const QString &peer : peers) {
        m_peerListWidget->addItem(peer);
    }
}

void MainWindow::onRemoteOpenMediaRequested(const QString &filePath, qint64 positionMs, double playbackRate)
{
    if (filePath.isEmpty()) {
        return;
    }

    if (!QFileInfo::exists(filePath)) {
        // 如果本地还没有收到房主文件，就先提示等待，不贸然打开无效路径。
        onChatMessageReceived(tr("系统"), tr("正在等待房主发送视频文件..."), false);
        return;
    }

    m_remoteSyncInProgress = true;
    setPlaybackRateCombo(playbackRate);
    m_currentMediaPath = filePath;
    appendToPlaylist(filePath);
    m_playerController->openFile(filePath);
    if (positionMs > 0) {
        QTimer::singleShot(500, this, [this, positionMs]() {
            if (!m_currentMediaPath.isEmpty()) {
                m_playerController->seekSmooth(positionMs);
            }
        });
    }
    m_remoteSyncInProgress = false;
}

void MainWindow::onRemotePlaybackCommandReceived(const QString &command, qint64 positionMs, double playbackRate)
{
    // 观众端收到房主控制指令后在这里落地。
    // 周期 sync 只修正明显漂移，并在刚接收完媒体时暂时忽略，避免画面闪烁和进度倒退。
    m_remoteSyncInProgress = true;
    setPlaybackRateCombo(playbackRate);

    // 远端播放指令统一在这里落地，避免 UI 各处分散处理网络同步分支。
    if (command == QStringLiteral("play")) {
        const bool justOpenedRemoteMedia = m_remoteMediaStabilizeTimer.isValid()
            && m_remoteMediaStabilizeTimer.elapsed() < 2500;
        if (positionMs > 0 && !justOpenedRemoteMedia) {
            m_playerController->seekSmooth(positionMs);
        }
        if (m_currentState != PlaybackState::Playing) {
            m_playerController->togglePlayPause();
        }
    } else if (command == QStringLiteral("pause")) {
        if (positionMs > 0) {
            m_playerController->seekSmooth(positionMs);
        }
        if (m_currentState == PlaybackState::Playing) {
            m_playerController->togglePlayPause();
        }
    } else if (command == QStringLiteral("stop")) {
        m_playerController->stop();
    } else if (command == QStringLiteral("seek")) {
        m_playerController->seekSmooth(positionMs);
    } else if (command == QStringLiteral("rate")) {
        if (positionMs > 0) {
            m_playerController->seekSmooth(positionMs);
        }
    } else if (command == QStringLiteral("sync")) {
        if (m_remoteMediaStabilizeTimer.isValid() && m_remoteMediaStabilizeTimer.elapsed() < 3000) {
            m_remoteSyncInProgress = false;
            return;
        }

        const qint64 localPositionMs = currentSliderPositionMs();
        const qint64 driftMs = std::llabs(localPositionMs - positionMs);
        const bool canCorrectNow = !m_lastRemoteCorrectionTimer.isValid()
            || m_lastRemoteCorrectionTimer.elapsed() > 2500;
        const bool remoteIsAhead = positionMs > localPositionMs;
        // 周期性 sync 只修正较大的、持续存在的漂移；小抖动交给本地播放自然追平。
        if (m_currentState == PlaybackState::Playing && driftMs > 1500 && remoteIsAhead && canCorrectNow) {
            m_playerController->seekSmooth(positionMs);
            m_lastRemoteCorrectionTimer.restart();
        }
        if (m_currentState != PlaybackState::Playing && positionMs > 0 && driftMs > 1500 && canCorrectNow) {
            m_playerController->seekSmooth(positionMs);
            m_lastRemoteCorrectionTimer.restart();
        }
    }

    m_remoteSyncInProgress = false;
}

void MainWindow::onMediaTransferProgress(const QString &fileName, qint64 receivedBytes, qint64 totalBytes)
{
    if (totalBytes <= 0) {
        return;
    }

    const int percent = static_cast<int>((receivedBytes * 100) / totalBytes);
    m_roomStatusLabel->setText(tr("正在接收 %1：%2%").arg(fileName).arg(percent));
}

void MainWindow::onMediaFileReceived(const QString &filePath, qint64 positionMs, double playbackRate)
{
    // 观众端接收完整视频文件后开始播放。
    // 这里会启动短暂稳定期，让解码器先打开文件和显示首帧，再接受房主同步校正。
    m_remoteSyncInProgress = true;
    m_remoteMediaStabilizeTimer.restart();
    m_lastRemoteCorrectionTimer.restart();
    setPlaybackRateCombo(playbackRate);
    m_currentMediaPath = filePath;
    appendToPlaylist(filePath);
    m_playerController->openFile(filePath);
    if (positionMs > 0) {
        QTimer::singleShot(800, this, [this, positionMs]() {
            if (!m_currentMediaPath.isEmpty()) {
                m_playerController->seekSmooth(positionMs);
            }
        });
    }
    m_remoteSyncInProgress = false;

    // 收到房主推送的视频文件后，直接加入列表并开始播放，尽量降低操作门槛。
    const QFileInfo fileInfo(filePath);
    onChatMessageReceived(tr("系统"), tr("已接收并开始播放：%1").arg(fileInfo.fileName()), false);
    m_roomStatusLabel->setText(m_watchPartyManager->roomAddressText());
}

void MainWindow::onPlaybackRateChanged(int index)
{
    if (index < 0) {
        return;
    }

    const double rate = m_speedComboBox->itemData(index).toDouble();
    qDebug() << "[MainWindow] playback rate changed index=" << index << "rate=" << rate;
    // 变速既要更新本地解码器，也要把新的速度和当前位置同步到房间内其他成员。
    if (rate > 0.0) {
        m_playerController->setPlaybackRate(rate);
        if (m_watchPartyManager && m_watchPartyManager->role() == WatchPartyRole::Host && !m_currentMediaPath.isEmpty()) {
            m_watchPartyManager->updateHostMediaState(
                m_currentMediaPath,
                currentSliderPositionMs(),
                currentPlaybackRate(),
                m_currentState == PlaybackState::Playing);
        }
        sendWatchPartyPlaybackCommand(QStringLiteral("rate"));
    }
}

void MainWindow::onMuteClicked()
{
    if (m_volumeSlider->value() == 0) {
        const int restoredVolume = std::max(1, m_lastVolumeValue);
        m_volumeSlider->setValue(restoredVolume);
        m_playerController->setVolume(static_cast<float>(restoredVolume) / 100.0f);
        updateVolumeUi(restoredVolume);
        return;
    }

    m_lastVolumeValue = m_volumeSlider->value();
    m_volumeSlider->setValue(0);
    m_playerController->setVolume(0.0f);
    updateVolumeUi(0);
}

void MainWindow::onVolumeChanged(int value)
{
    if (value > 0) {
        m_lastVolumeValue = value;
    }

    m_playerController->setVolume(static_cast<float>(value) / 100.0f);
    updateVolumeUi(value);
}

void MainWindow::onFullscreenClicked()
{
    if (isFullScreen()) {
        showNormal();
    } else {
        showFullScreen();
    }

    updateFullscreenButton();
}

void MainWindow::onSliderPressed()
{
    m_userIsSeeking = true;
}

void MainWindow::onSliderReleased()
{
    m_userIsSeeking = false;
    // 拖动结束才真正执行 seek，避免用户拖动过程中持续重建解码状态。
    m_playerController->seek(m_positionSlider->value());
    if (m_watchPartyManager && m_watchPartyManager->role() == WatchPartyRole::Host && !m_currentMediaPath.isEmpty()) {
        m_watchPartyManager->updateHostMediaState(
            m_currentMediaPath,
            currentSliderPositionMs(),
            currentPlaybackRate(),
            m_currentState == PlaybackState::Playing);
    }
    sendWatchPartyPlaybackCommand(QStringLiteral("seek"));
}

void MainWindow::onPositionUpdated(qint64 positionMs, qint64 durationMs)
{
    if (!m_userIsSeeking) {
        m_positionSlider->setValue(static_cast<int>(positionMs));
    }

    updateTimeLabel(positionMs, durationMs);
}

void MainWindow::onDurationChanged(qint64 durationMs)
{
    m_positionSlider->setRange(0, static_cast<int>(durationMs));
    updateTimeLabel(0, durationMs);
}

void MainWindow::onStateChanged(PlaybackState state)
{
    m_currentState = state;
    updatePlayButtonText();
    m_statusLabel->setText(playbackStateToString(state));
    updateStatusPill(state);

    // 只有房主真正处于播放状态时才开启周期性同步心跳。
    if (state == PlaybackState::Playing && m_watchPartyManager->role() == WatchPartyRole::Host) {
        if (!m_currentMediaPath.isEmpty()) {
            m_watchPartyManager->updateHostMediaState(
                m_currentMediaPath,
                currentSliderPositionMs(),
                currentPlaybackRate(),
                true);
        }
        m_watchPartySyncTimer.start();
    } else if (state != PlaybackState::Playing) {
        if (m_watchPartyManager && m_watchPartyManager->role() == WatchPartyRole::Host && !m_currentMediaPath.isEmpty()) {
            m_watchPartyManager->updateHostMediaState(
                m_currentMediaPath,
                currentSliderPositionMs(),
                currentPlaybackRate(),
                false);
        }
        m_watchPartySyncTimer.stop();
    }
}

void MainWindow::onErrorOccurred(const QString &message)
{
    m_statusLabel->setText(message);
    QMessageBox::warning(this, tr("Playback Error"), message);
}

void MainWindow::onStatusMessage(const QString &message)
{
    statusBar()->showMessage(message, 5000);
}

void MainWindow::onWatchPartySyncTimer()
{
    // 房主播放时周期性广播当前位置，用于修正观众端长时间播放后的漂移。
    // 心跳频率不能太高，否则观众端容易频繁 seek 造成闪屏。
    if (m_remoteSyncInProgress || !m_watchPartyManager || m_watchPartyManager->role() != WatchPartyRole::Host) {
        return;
    }

    // 同看心跳用于纠正成员间的轻微时间漂移，不承担首次开播同步职责。
    if (m_currentState == PlaybackState::Playing && !m_currentMediaPath.isEmpty()) {
        m_watchPartyManager->updateHostMediaState(
            m_currentMediaPath,
            currentSliderPositionMs(),
            currentPlaybackRate(),
            true);
        m_watchPartyManager->broadcastPlaybackCommand(
            QStringLiteral("sync"),
            currentSliderPositionMs(),
            currentPlaybackRate());
    }
}

void MainWindow::applyTheme()
{
    // 整体样式在这里集中维护，避免控件初始化时散落大量 setStyleSheet 调用。
    setStyleSheet(R"(
        QMainWindow, QWidget#Root {
            background: transparent;
            color: #14253f;
            font-family: "Microsoft YaHei UI", "Segoe UI";
        }

        QFrame#VideoCard {
            background: rgba(255, 255, 255, 0.58);
            border: 1px solid rgba(124, 154, 214, 0.22);
            border-radius: 30px;
        }

        QFrame#ControlCard {
            background: rgba(255, 255, 255, 0.82);
            border: 1px solid rgba(124, 154, 214, 0.26);
            border-radius: 28px;
        }

        QFrame#PlaylistCard {
            background: rgba(255, 255, 255, 0.78);
            border: 1px solid rgba(124, 154, 214, 0.22);
            border-radius: 22px;
        }

        QFrame#PartyCard {
            background: rgba(255, 255, 255, 0.78);
            border: 1px solid rgba(124, 154, 214, 0.22);
            border-radius: 22px;
        }

        QTabWidget#SideTabs::pane {
            border: 1px solid rgba(124, 154, 214, 0.22);
            border-radius: 28px;
            background: rgba(255, 255, 255, 0.58);
            top: -1px;
        }

        QTabWidget#SideTabs::tab-bar {
            alignment: center;
        }

        QTabBar::tab {
            background: rgba(255, 255, 255, 0.62);
            color: #55708f;
            border: 1px solid rgba(124, 154, 214, 0.22);
            padding: 10px 20px;
            min-width: 104px;
            font-weight: 700;
        }

        QTabBar::tab:first {
            border-top-left-radius: 16px;
        }

        QTabBar::tab:last {
            border-top-right-radius: 16px;
        }

        QTabBar::tab:selected {
            background: rgba(255, 255, 255, 0.94);
            color: #173052;
            border-bottom-color: rgba(255, 255, 255, 0.94);
        }

        QLabel#BrandLabel {
            color: #3c7eef;
            font-size: 11px;
            font-weight: 700;
            letter-spacing: 2px;
            background: rgba(255, 255, 255, 0.42);
            border: 1px solid rgba(124, 154, 214, 0.18);
            border-radius: 12px;
            padding: 6px 10px;
        }

        QLabel#HeadlineLabel {
            color: #14253f;
            font-size: 34px;
            font-weight: 700;
        }

        QLabel#MetaLabel {
            color: #667896;
            font-size: 14px;
        }

        QLabel#PanelTitle {
            color: #244067;
            font-size: 13px;
            font-weight: 700;
            letter-spacing: 1px;
        }

        QToolButton {
            background: rgba(255, 255, 255, 0.94);
            color: #18314f;
            border: 1px solid #c9d8ef;
            border-radius: 16px;
            padding: 10px 16px;
            font-size: 14px;
            font-weight: 600;
        }

        QToolButton:hover {
            background: #ffffff;
            border-color: #83aaf3;
        }

        QToolButton:pressed {
            background: #e9f1ff;
        }

        QToolButton#PrimaryButton {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                        stop:0 #5ca0ff,
                                        stop:1 #63d8ff);
            color: #ffffff;
            border: none;
            font-weight: 700;
        }

        QToolButton#PrimaryButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                        stop:0 #74afff,
                                        stop:1 #80e1ff);
        }

        QToolButton#CircleButton {
            min-width: 46px;
            max-width: 46px;
            min-height: 46px;
            max-height: 46px;
            border-radius: 23px;
            padding: 0;
        }

        QComboBox {
            min-width: 96px;
            padding: 8px 12px;
            border-radius: 14px;
            border: 1px solid #c9d8ef;
            background: rgba(255, 255, 255, 0.95);
            color: #173052;
            font-weight: 600;
        }

        QComboBox::drop-down {
            width: 28px;
            border: none;
        }

        QListWidget {
            background: transparent;
            border: none;
            outline: none;
            padding: 4px;
            color: #1b3152;
            font-size: 14px;
        }

        QTextEdit, QLineEdit {
            background: rgba(255, 255, 255, 0.76);
            border: 1px solid rgba(201, 216, 239, 0.86);
            border-radius: 14px;
            color: #1b3152;
            padding: 10px;
            font-size: 13px;
        }

        QTextEdit {
            selection-background-color: #bfe8ff;
        }

        QListWidget::item {
            border-radius: 14px;
            padding: 14px 14px;
            margin: 5px 0;
            background: rgba(255, 255, 255, 0.72);
            border: 1px solid rgba(201, 216, 239, 0.75);
        }

        QListWidget::item:selected {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                        stop:0 rgba(92, 160, 255, 0.18),
                                        stop:1 rgba(99, 216, 255, 0.18));
            border: 1px solid rgba(92, 147, 255, 0.55);
            color: #14355c;
        }

        QSlider::groove:horizontal {
            background: #d7e1f2;
            border-radius: 4px;
            height: 8px;
        }

        QSlider::sub-page:horizontal {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                        stop:0 #55d2ff,
                                        stop:1 #5b92ff);
            border-radius: 4px;
        }

        QSlider::add-page:horizontal {
            background: #d7e1f2;
            border-radius: 4px;
        }

        QSlider::handle:horizontal {
            width: 18px;
            margin: -6px 0;
            border-radius: 9px;
            border: 2px solid #5c93ff;
            background: #ffffff;
        }

        QSlider#VolumeSlider::groove:horizontal {
            height: 6px;
        }

        QSlider#VolumeSlider::handle:horizontal {
            width: 14px;
            margin: -5px 0;
            border-radius: 7px;
        }

        QLabel#TimeLabel {
            color: #173052;
            font-size: 15px;
            font-weight: 600;
        }

        QLabel#StatusPill {
            border-radius: 14px;
            padding: 8px 14px;
            font-size: 12px;
            font-weight: 700;
        }

        QLabel#StatusPill[stateRole="idle"],
        QLabel#StatusPill[stateRole="stopped"] {
            color: #5f708d;
            background: #f6f9ff;
            border: 1px solid #d7e1f2;
        }

        QLabel#StatusPill[stateRole="loading"],
        QLabel#StatusPill[stateRole="seeking"] {
            color: #2f82c8;
            background: #eaf7ff;
            border: 1px solid #b9dcf6;
        }

        QLabel#StatusPill[stateRole="playing"] {
            color: #2d8c63;
            background: #e9fbf2;
            border: 1px solid #beebd0;
        }

        QLabel#StatusPill[stateRole="paused"] {
            color: #a97716;
            background: #fff7df;
            border: 1px solid #f0dfa3;
        }

        QLabel#StatusPill[stateRole="end"] {
            color: #6f5cb4;
            background: #f1edff;
            border: 1px solid #d8ccff;
        }

        QLabel#StatusPill[stateRole="error"] {
            color: #c34f67;
            background: #fff0f4;
            border: 1px solid #f2c5cf;
        }

        QStatusBar {
            background: transparent;
            color: #7a8ca7;
        }
    )");
}

void MainWindow::applyWindowChrome()
{
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) {
        return;
    }

    using DwmSetWindowAttributePtr = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    const HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    if (!dwmapi) {
        return;
    }

    const auto setWindowAttribute = reinterpret_cast<DwmSetWindowAttributePtr>(
        GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    if (!setWindowAttribute) {
        FreeLibrary(dwmapi);
        return;
    }

    const BOOL enableDarkMode = FALSE;
    const DWORD darkModeAttribute = 20;
    const DWORD legacyDarkModeAttribute = 19;
    setWindowAttribute(hwnd, darkModeAttribute, &enableDarkMode, sizeof(enableDarkMode));
    setWindowAttribute(hwnd, legacyDarkModeAttribute, &enableDarkMode, sizeof(enableDarkMode));
    FreeLibrary(dwmapi);
#endif
}

void MainWindow::buildUi()
{
    // 手工构建整个 Qt Widgets 界面：顶部标题区、视频区域、控制条、播放列表和同看聊天页。
    // 控件指针保存为成员变量，后续槽函数可以直接更新状态。
    auto *centralWidget = new AnimatedBackgroundWidget(this);
    centralWidget->setObjectName("Root");
    m_backgroundWidget = static_cast<AnimatedBackgroundWidget *>(centralWidget);

    auto *rootLayout = new QVBoxLayout(centralWidget);
    rootLayout->setContentsMargins(28, 24, 28, 24);
    rootLayout->setSpacing(20);

    auto *contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(18);

    auto *videoColumnLayout = new QVBoxLayout();
    videoColumnLayout->setSpacing(18);

    auto *videoCard = new QFrame(this);
    videoCard->setObjectName("VideoCard");
    auto *videoShadow = new QGraphicsDropShadowEffect(videoCard);
    videoShadow->setBlurRadius(42);
    videoShadow->setOffset(0, 16);
    videoShadow->setColor(QColor(90, 125, 200, 34));
    videoCard->setGraphicsEffect(videoShadow);
    auto *videoCardLayout = new QGridLayout(videoCard);
    videoCardLayout->setContentsMargins(1, 1, 1, 1);
    videoCardLayout->setSpacing(0);

    m_videoWidget = new VideoWidget(videoCard);
    m_videoWidget->setMinimumHeight(520);
    videoCardLayout->addWidget(m_videoWidget, 0, 0);

    auto *overlayHost = new QWidget(videoCard);
    auto *overlayHostLayout = new QVBoxLayout(overlayHost);
    overlayHostLayout->setContentsMargins(22, 22, 22, 18);
    overlayHostLayout->setSpacing(0);
    overlayHostLayout->addStretch(1);

    auto *controlCard = new QFrame(videoCard);
    controlCard->setObjectName("ControlCard");
    auto *controlShadow = new QGraphicsDropShadowEffect(controlCard);
    controlShadow->setBlurRadius(36);
    controlShadow->setOffset(0, 14);
    controlShadow->setColor(QColor(111, 135, 191, 28));
    controlCard->setGraphicsEffect(controlShadow);
    auto *controlCardLayout = new QVBoxLayout(controlCard);
    controlCardLayout->setContentsMargins(24, 22, 24, 22);
    controlCardLayout->setSpacing(18);

    auto *transportLayout = new QHBoxLayout();
    transportLayout->setSpacing(12);

    m_openButton = new QToolButton(this);
    m_openButton->setObjectName("PrimaryButton");
    m_openButton->setText(tr("Open"));
    m_openButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_openButton->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));

    m_previousButton = new QToolButton(this);
    m_previousButton->setObjectName("CircleButton");
    m_previousButton->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));

    m_playPauseButton = new QToolButton(this);
    m_playPauseButton->setObjectName("CircleButton");

    m_nextButton = new QToolButton(this);
    m_nextButton->setObjectName("CircleButton");
    m_nextButton->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));

    m_stopButton = new QToolButton(this);
    m_stopButton->setObjectName("CircleButton");
    m_stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));

    m_volumeButton = new QToolButton(this);
    m_volumeButton->setObjectName("CircleButton");

    m_fullscreenButton = new QToolButton(this);
    m_fullscreenButton->setObjectName("CircleButton");

    m_positionSlider = new QSlider(Qt::Horizontal, this);
    m_positionSlider->setRange(0, 0);

    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setObjectName("VolumeSlider");
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(m_lastVolumeValue);
    m_volumeSlider->setFixedWidth(120);

    m_timeLabel = new QLabel(tr("00:00 / 00:00"), this);
    m_timeLabel->setObjectName("TimeLabel");

    m_statusLabel = new QLabel(playbackStateToString(m_currentState), this);
    m_statusLabel->setObjectName("StatusPill");

    transportLayout->addWidget(m_openButton);
    transportLayout->addWidget(m_previousButton);
    transportLayout->addWidget(m_playPauseButton);
    transportLayout->addWidget(m_nextButton);
    transportLayout->addWidget(m_stopButton);
    transportLayout->addSpacing(8);
    transportLayout->addWidget(m_volumeButton);
    transportLayout->addWidget(m_volumeSlider);
    transportLayout->addStretch(1);
    transportLayout->addWidget(m_fullscreenButton);
    transportLayout->addWidget(m_statusLabel);

    auto *timelineLayout = new QHBoxLayout();
    timelineLayout->setSpacing(14);
    timelineLayout->addWidget(m_positionSlider, 1);
    timelineLayout->addWidget(m_timeLabel);

    controlCardLayout->addLayout(transportLayout);
    controlCardLayout->addLayout(timelineLayout);

    overlayHostLayout->addWidget(controlCard);
    videoCardLayout->addWidget(overlayHost, 0, 0);

    auto *sideTabs = new QTabWidget(this);
    sideTabs->setObjectName("SideTabs");
    sideTabs->setFixedWidth(360);
    sideTabs->setDocumentMode(true);

    auto *playlistCard = new QFrame(this);
    playlistCard->setObjectName("PlaylistCard");
    auto *playlistShadow = new QGraphicsDropShadowEffect(playlistCard);
    playlistShadow->setBlurRadius(34);
    playlistShadow->setOffset(0, 12);
    playlistShadow->setColor(QColor(111, 135, 191, 24));
    playlistCard->setGraphicsEffect(playlistShadow);

    auto *playlistLayout = new QVBoxLayout(playlistCard);
    playlistLayout->setContentsMargins(20, 20, 20, 20);
    playlistLayout->setSpacing(14);

    auto *playlistTitle = new QLabel(tr("播放列表"), playlistCard);
    playlistTitle->setObjectName("PanelTitle");
    m_speedComboBox = new QComboBox(playlistCard);
    m_speedComboBox->addItem(tr("0.75 倍速"), 0.75);
    m_speedComboBox->addItem(tr("1.0 倍速"), 1.0);
    m_speedComboBox->addItem(tr("1.25 倍速"), 1.25);
    m_speedComboBox->addItem(tr("1.5 倍速"), 1.5);
    m_speedComboBox->addItem(tr("2.0 倍速"), 2.0);
    m_speedComboBox->setCurrentIndex(1);

    auto *playlistHeader = new QHBoxLayout();
    playlistHeader->setSpacing(10);
    playlistHeader->addWidget(playlistTitle);
    playlistHeader->addStretch(1);
    playlistHeader->addWidget(m_speedComboBox);

    auto *playlistActionLayout = new QHBoxLayout();
    playlistActionLayout->setSpacing(8);
    playlistActionLayout->addStretch(1);

    m_deletePlaylistButton = new QToolButton(playlistCard);
    m_deletePlaylistButton->setObjectName("CircleButton");
    m_deletePlaylistButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    m_deletePlaylistButton->setToolTip(tr("删除当前项"));

    m_clearPlaylistButton = new QToolButton(playlistCard);
    m_clearPlaylistButton->setObjectName("CircleButton");
    m_clearPlaylistButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    m_clearPlaylistButton->setToolTip(tr("清空列表"));

    playlistActionLayout->addWidget(m_deletePlaylistButton);
    playlistActionLayout->addWidget(m_clearPlaylistButton);

    m_playlistWidget = new QListWidget(playlistCard);
    m_playlistWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_playlistWidget->setContextMenuPolicy(Qt::CustomContextMenu);

    playlistLayout->addLayout(playlistHeader);
    playlistLayout->addLayout(playlistActionLayout);
    playlistLayout->addWidget(m_playlistWidget, 1);

    auto *partyCard = new QFrame(this);
    partyCard->setObjectName("PartyCard");
    auto *partyShadow = new QGraphicsDropShadowEffect(partyCard);
    partyShadow->setBlurRadius(34);
    partyShadow->setOffset(0, 12);
    partyShadow->setColor(QColor(111, 135, 191, 24));
    partyCard->setGraphicsEffect(partyShadow);

    auto *partyLayout = new QVBoxLayout(partyCard);
    partyLayout->setContentsMargins(20, 20, 20, 20);
    partyLayout->setSpacing(12);

    auto *partyTitle = new QLabel(tr("同看房间"), partyCard);
    partyTitle->setObjectName("PanelTitle");

    m_roomStatusLabel = new QLabel(tr("未连接"), partyCard);
    m_roomStatusLabel->setObjectName("MetaLabel");
    m_roomStatusLabel->setWordWrap(true);

    auto *roomActionLayout = new QHBoxLayout();
    roomActionLayout->setSpacing(8);

    m_createRoomButton = new QToolButton(partyCard);
    m_createRoomButton->setText(tr("创建"));
    m_createRoomButton->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    m_createRoomButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_joinRoomButton = new QToolButton(partyCard);
    m_joinRoomButton->setText(tr("加入"));
    m_joinRoomButton->setIcon(style()->standardIcon(QStyle::SP_DirLinkIcon));
    m_joinRoomButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_leaveRoomButton = new QToolButton(partyCard);
    m_leaveRoomButton->setObjectName("CircleButton");
    m_leaveRoomButton->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    m_leaveRoomButton->setToolTip(tr("离开房间"));

    roomActionLayout->addWidget(m_createRoomButton);
    roomActionLayout->addWidget(m_joinRoomButton);
    roomActionLayout->addWidget(m_leaveRoomButton);

    auto *peerTitle = new QLabel(tr("在线成员"), partyCard);
    peerTitle->setObjectName("PanelTitle");

    m_peerListWidget = new QListWidget(partyCard);
    m_peerListWidget->setMaximumHeight(90);
    m_peerListWidget->setSelectionMode(QAbstractItemView::NoSelection);

    m_chatView = new QTextEdit(partyCard);
    m_chatView->setReadOnly(true);
    m_chatView->setMinimumHeight(150);

    auto *chatInputLayout = new QHBoxLayout();
    chatInputLayout->setSpacing(8);

    m_chatInput = new QLineEdit(partyCard);
    m_chatInput->setPlaceholderText(tr("输入聊天内容"));

    m_sendChatButton = new QToolButton(partyCard);
    m_sendChatButton->setObjectName("PrimaryButton");
    m_sendChatButton->setText(tr("发送"));

    chatInputLayout->addWidget(m_chatInput, 1);
    chatInputLayout->addWidget(m_sendChatButton);

    partyLayout->addWidget(partyTitle);
    partyLayout->addWidget(m_roomStatusLabel);
    partyLayout->addLayout(roomActionLayout);
    partyLayout->addWidget(peerTitle);
    partyLayout->addWidget(m_peerListWidget);
    partyLayout->addWidget(m_chatView, 1);
    partyLayout->addLayout(chatInputLayout);

    sideTabs->addTab(playlistCard, tr("播放列表"));
    sideTabs->addTab(partyCard, tr("同看聊天"));

    videoColumnLayout->addWidget(videoCard, 1);
    contentLayout->addLayout(videoColumnLayout, 1);
    contentLayout->addWidget(sideTabs);

    rootLayout->addLayout(contentLayout, 1);

    setCentralWidget(centralWidget);
    statusBar()->showMessage(tr("Ready"));

    m_playerController = new PlayerController(m_videoWidget, this);
    m_playerController->setVolume(static_cast<float>(m_lastVolumeValue) / 100.0f);
    m_watchPartyManager = new WatchPartyManager(this);
    m_watchPartySyncTimer.setInterval(1000);

    updateStatusPill(m_currentState);
    updatePlayButtonText();
    updateVolumeUi(m_lastVolumeValue);
    updateFullscreenButton();
}

void MainWindow::connectSignals()
{
    // 所有 UI 交互和播放器/网络事件都集中在这里接线，便于后续排查行为来源。
    connect(m_openButton, &QToolButton::clicked, this, &MainWindow::onOpenClicked);
    connect(m_previousButton, &QToolButton::clicked, this, &MainWindow::onPreviousClicked);
    connect(m_playPauseButton, &QToolButton::clicked, this, &MainWindow::onPlayPauseClicked);
    connect(m_nextButton, &QToolButton::clicked, this, &MainWindow::onNextClicked);
    connect(m_stopButton, &QToolButton::clicked, this, &MainWindow::onStopClicked);
    connect(m_playlistWidget, &QListWidget::itemSelectionChanged, this, &MainWindow::onPlaylistSelectionChanged);
    connect(m_playlistWidget, &QListWidget::itemClicked, this, &MainWindow::onPlaylistItemActivated);
    connect(m_playlistWidget, &QListWidget::itemDoubleClicked, this, &MainWindow::onPlaylistItemActivated);
    connect(m_playlistWidget, &QWidget::customContextMenuRequested, this, &MainWindow::onPlaylistContextMenuRequested);
    connect(m_deletePlaylistButton, &QToolButton::clicked, this, &MainWindow::onDeletePlaylistItem);
    connect(m_clearPlaylistButton, &QToolButton::clicked, this, &MainWindow::onClearPlaylist);
    connect(m_createRoomButton, &QToolButton::clicked, this, &MainWindow::onCreateRoomClicked);
    connect(m_joinRoomButton, &QToolButton::clicked, this, &MainWindow::onJoinRoomClicked);
    connect(m_leaveRoomButton, &QToolButton::clicked, this, &MainWindow::onLeaveRoomClicked);
    connect(m_sendChatButton, &QToolButton::clicked, this, &MainWindow::onSendChatClicked);
    connect(m_chatInput, &QLineEdit::returnPressed, this, &MainWindow::onChatReturnPressed);
    connect(m_speedComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onPlaybackRateChanged);
    connect(m_volumeButton, &QToolButton::clicked, this, &MainWindow::onMuteClicked);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &MainWindow::onVolumeChanged);
    connect(m_fullscreenButton, &QToolButton::clicked, this, &MainWindow::onFullscreenClicked);
    connect(m_positionSlider, &QSlider::sliderPressed, this, &MainWindow::onSliderPressed);
    connect(m_positionSlider, &QSlider::sliderReleased, this, &MainWindow::onSliderReleased);
    connect(&m_watchPartySyncTimer, &QTimer::timeout, this, &MainWindow::onWatchPartySyncTimer);

    connect(m_playerController, &PlayerController::positionUpdated, this, &MainWindow::onPositionUpdated);
    connect(m_playerController, &PlayerController::durationChanged, this, &MainWindow::onDurationChanged);
    connect(m_playerController, &PlayerController::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_playerController, &PlayerController::errorOccurred, this, &MainWindow::onErrorOccurred);
    connect(m_playerController, &PlayerController::statusMessage, this, &MainWindow::onStatusMessage);

    connect(m_watchPartyManager, &WatchPartyManager::chatMessageReceived, this, &MainWindow::onChatMessageReceived);
    connect(m_watchPartyManager, &WatchPartyManager::roomStatusChanged, this, &MainWindow::onRoomStatusChanged);
    connect(m_watchPartyManager, &WatchPartyManager::peerListChanged, this, &MainWindow::onPeerListChanged);
    connect(m_watchPartyManager, &WatchPartyManager::remoteOpenMediaRequested, this, &MainWindow::onRemoteOpenMediaRequested);
    connect(m_watchPartyManager, &WatchPartyManager::remotePlaybackCommandReceived, this, &MainWindow::onRemotePlaybackCommandReceived);
    connect(m_watchPartyManager, &WatchPartyManager::mediaTransferProgress, this, &MainWindow::onMediaTransferProgress);
    connect(m_watchPartyManager, &WatchPartyManager::mediaFileReceived, this, &MainWindow::onMediaFileReceived);
    connect(m_watchPartyManager, &WatchPartyManager::errorOccurred, this, &MainWindow::onErrorOccurred);
}

void MainWindow::updatePlayButtonText()
{
    if (m_currentState == PlaybackState::Playing) {
        m_playPauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
        m_playPauseButton->setToolTip(tr("Pause"));
    } else {
        m_playPauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        m_playPauseButton->setToolTip(tr("Play"));
    }
}

void MainWindow::updateStatusPill(PlaybackState state)
{
    QString role = QStringLiteral("idle");
    switch (state) {
    case PlaybackState::Idle:
    case PlaybackState::Stopped:
        role = QStringLiteral("stopped");
        break;
    case PlaybackState::Loading:
        role = QStringLiteral("loading");
        break;
    case PlaybackState::Playing:
        role = QStringLiteral("playing");
        break;
    case PlaybackState::Paused:
        role = QStringLiteral("paused");
        break;
    case PlaybackState::Seeking:
        role = QStringLiteral("seeking");
        break;
    case PlaybackState::EndOfFile:
        role = QStringLiteral("end");
        break;
    case PlaybackState::Error:
        role = QStringLiteral("error");
        break;
    }

    m_statusLabel->setProperty("stateRole", role);
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);

    // 到达文件结尾时，若播放列表还有下一项，则自动续播。
    if (state == PlaybackState::EndOfFile) {
        const int index = currentPlaylistIndex();
        if (index >= 0 && index + 1 < m_playlistWidget->count()) {
            playPlaylistItem(index + 1);
        }
    }
}

void MainWindow::playPlaylistItem(int index)
{
    if (index < 0 || index >= m_playlistWidget->count()) {
        return;
    }

    // 切歌时临时屏蔽列表信号，避免 setCurrentRow 触发额外的选择联动。
    m_playlistActivationBlocked = true;
    m_playlistSignalBlocked = true;
    m_playlistWidget->setCurrentRow(index);
    m_playlistSignalBlocked = false;
    m_playlistActivationBlocked = false;

    QListWidgetItem *item = m_playlistWidget->item(index);
    if (!item) {
        return;
    }

    const QString filePath = item->data(Qt::UserRole).toString();
    if (!filePath.isEmpty()) {
        m_currentMediaPath = filePath;
        savePlaylistState();
        m_playerController->openFile(filePath);
        sendWatchPartyMediaSync(filePath);
        sendWatchPartyMediaFile(filePath);
    }
}

int MainWindow::currentPlaylistIndex() const
{
    return m_playlistWidget->currentRow();
}

void MainWindow::appendToPlaylist(const QString &filePath)
{
    appendToPlaylistAndReturnIndex(filePath, true);
}

int MainWindow::appendToPlaylistAndReturnIndex(const QString &filePath, bool selectItem)
{
    for (int i = 0; i < m_playlistWidget->count(); ++i) {
        QListWidgetItem *item = m_playlistWidget->item(i);
        if (item->data(Qt::UserRole).toString() == filePath) {
            if (selectItem) {
                m_playlistSignalBlocked = true;
                m_playlistWidget->setCurrentItem(item);
                m_playlistSignalBlocked = false;
            }
            savePlaylistState();
            return i;
        }
    }

    const QFileInfo fileInfo(filePath);
    auto *item = new QListWidgetItem(fileInfo.fileName(), m_playlistWidget);
    item->setToolTip(filePath);
    item->setData(Qt::UserRole, filePath);

    const int newIndex = m_playlistWidget->count();
    m_playlistSignalBlocked = true;
    m_playlistWidget->addItem(item);
    if (selectItem) {
        m_playlistWidget->setCurrentItem(item);
    }
    m_playlistSignalBlocked = false;

    savePlaylistState();
    return newIndex;
}

void MainWindow::addMediaFilesToPlaylist(const QStringList &filePaths, bool playFirstFile)
{
    QString firstPlayableFile;
    int firstPlayableIndex = -1;
    int addedCount = 0;

    // 批量导入时只记录首个可播放文件，最后统一决定是否自动开播。
    for (const QString &filePath : filePaths) {
        if (filePath.isEmpty() || !isSupportedMediaFile(filePath)) {
            continue;
        }

        if (firstPlayableFile.isEmpty()) {
            firstPlayableFile = filePath;
        }

        const int index = appendToPlaylistAndReturnIndex(filePath, false);
        if (firstPlayableIndex < 0) {
            firstPlayableIndex = index;
        }
        ++addedCount;
    }

    if (addedCount <= 0) {
        statusBar()->showMessage(tr("没有找到可播放的媒体文件"), 3000);
        return;
    }

    statusBar()->showMessage(tr("已添加 %1 个媒体文件").arg(addedCount), 3000);

    if (playFirstFile && firstPlayableIndex >= 0) {
        playPlaylistItem(firstPlayableIndex);
    }
}

bool MainWindow::isSupportedMediaFile(const QString &filePath) const
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    static const QStringList supported = {
        QStringLiteral("mp4"),
        QStringLiteral("mkv"),
        QStringLiteral("avi"),
        QStringLiteral("mov"),
        QStringLiteral("mp3"),
        QStringLiteral("aac")
    };
    return supported.contains(suffix);
}

void MainWindow::loadPlaylistState()
{
    // 从应用数据目录读取上次保存的播放列表和倍速。
    // 只恢复仍然存在的本地文件，避免列表里出现失效路径。
    const QString statePath = playlistStateFilePath();
    QFile file(statePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!document.isObject()) {
        return;
    }

    // 启动时恢复上次的播放列表、当前文件和倍速设置，但不自动开始播放。
    const QJsonObject root = document.object();
    const QJsonArray playlist = root.value(QStringLiteral("playlist")).toArray();
    const QString currentFile = root.value(QStringLiteral("currentFile")).toString();
    const double rate = root.value(QStringLiteral("playbackRate")).toDouble(1.0);
    qDebug() << "[MainWindow] loadPlaylistState currentFile=" << currentFile << "rate=" << rate << "count=" << playlist.size();

    m_playlistSignalBlocked = true;
    for (const QJsonValue &entry : playlist) {
        const QString filePath = entry.toString();
        if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
            continue;
        }

        const QFileInfo fileInfo(filePath);
        auto *item = new QListWidgetItem(fileInfo.fileName(), m_playlistWidget);
        item->setToolTip(filePath);
        item->setData(Qt::UserRole, filePath);
        m_playlistWidget->addItem(item);

        if (filePath == currentFile) {
            qDebug() << "[MainWindow] restored current playlist item:" << filePath;
            m_playlistWidget->setCurrentItem(item);
            m_currentMediaPath = filePath;
        }
    }
    m_playlistSignalBlocked = false;

    const int speedIndex = std::max(0, m_speedComboBox->findData(rate));
    m_speedComboBox->setCurrentIndex(speedIndex);
}

void MainWindow::savePlaylistState() const
{
    // 关闭窗口、切换列表或添加文件时保存播放列表状态，方便下次打开继续使用。
    // 播放列表状态持久化到应用数据目录，便于下次打开继续使用。
    QJsonArray playlist;
    for (int i = 0; i < m_playlistWidget->count(); ++i) {
        playlist.append(m_playlistWidget->item(i)->data(Qt::UserRole).toString());
    }

    QJsonObject root;
    root.insert(QStringLiteral("playlist"), playlist);
    root.insert(QStringLiteral("currentFile"), m_currentMediaPath);
    root.insert(QStringLiteral("playbackRate"), m_speedComboBox->currentData().toDouble());

    const QString statePath = playlistStateFilePath();
    QDir().mkpath(QFileInfo(statePath).absolutePath());

    QFile file(statePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
}

QString MainWindow::playlistStateFilePath() const
{
    const QString appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return appDataDir + QStringLiteral("/playlist_state.json");
}

void MainWindow::updateVolumeUi(int value)
{
    if (value <= 0) {
        m_volumeButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolumeMuted));
        m_volumeButton->setToolTip(tr("Unmute"));
        return;
    }

    m_volumeButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    m_volumeButton->setToolTip(tr("Mute"));
}

void MainWindow::sendWatchPartyMediaSync(const QString &filePath)
{
    if (m_remoteSyncInProgress || !m_watchPartyManager) {
        return;
    }

    // 先发“媒体状态”，让对端知道应该打开什么，再由文件传输补足实际内容。
    m_watchPartyManager->broadcastMediaOpened(
        filePath,
        currentSliderPositionMs(),
        currentPlaybackRate());
    m_watchPartyManager->updateHostMediaState(
        filePath,
        currentSliderPositionMs(),
        currentPlaybackRate(),
        m_currentState == PlaybackState::Playing || m_currentState == PlaybackState::Loading);
}

void MainWindow::sendWatchPartyMediaFile(const QString &filePath)
{
    if (m_remoteSyncInProgress || !m_watchPartyManager) {
        return;
    }

    m_watchPartyManager->broadcastMediaFile(
        filePath,
        currentSliderPositionMs(),
        currentPlaybackRate());
}

void MainWindow::sendWatchPartyPlaybackCommand(const QString &command)
{
    if (m_remoteSyncInProgress || !m_watchPartyManager) {
        return;
    }

    m_watchPartyManager->broadcastPlaybackCommand(
        command,
        currentSliderPositionMs(),
        currentPlaybackRate());
}

void MainWindow::setPlaybackRateCombo(double playbackRate)
{
    if (!m_speedComboBox) {
        return;
    }

    // 从远端同步倍速时用 QSignalBlocker 避免再次触发本地 onPlaybackRateChanged。
    const int index = m_speedComboBox->findData(playbackRate);
    if (index >= 0 && index != m_speedComboBox->currentIndex()) {
        QSignalBlocker blocker(m_speedComboBox);
        m_speedComboBox->setCurrentIndex(index);
        m_playerController->setPlaybackRate(playbackRate);
    }
}

qint64 MainWindow::currentSliderPositionMs() const
{
    return m_positionSlider ? static_cast<qint64>(m_positionSlider->value()) : 0;
}

double MainWindow::currentPlaybackRate() const
{
    return m_speedComboBox ? m_speedComboBox->currentData().toDouble() : 1.0;
}

QString MainWindow::defaultDisplayName() const
{
    const QString hostName = QHostInfo::localHostName();
    return hostName.isEmpty() ? tr("我") : hostName;
}

void MainWindow::updateFullscreenButton()
{
    if (isFullScreen()) {
        m_fullscreenButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton));
        m_fullscreenButton->setToolTip(tr("Exit Fullscreen"));
    } else {
        m_fullscreenButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
        m_fullscreenButton->setToolTip(tr("Enter Fullscreen"));
    }
}

void MainWindow::updateTimeLabel(qint64 positionMs, qint64 durationMs)
{
    m_timeLabel->setText(QString("%1 / %2").arg(formatTime(positionMs), formatTime(durationMs)));
}

QString MainWindow::formatTime(qint64 milliseconds) const
{
    // 当前界面使用 mm:ss 即可，时长较长的媒体后续如有需要可再扩展到 hh:mm:ss。
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}
