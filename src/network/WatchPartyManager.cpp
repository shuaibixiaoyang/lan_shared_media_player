#include "network/WatchPartyManager.h"

#include <QAbstractSocket>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>

namespace
{
// 统一约定 JSON 消息字段名，避免字符串散落在各个处理分支里。
constexpr const char *TypeKey = "type";
constexpr const char *SenderKey = "sender";
constexpr const char *TextKey = "text";
constexpr const char *PeersKey = "peers";
constexpr const char *PathKey = "path";
constexpr const char *FileNameKey = "fileName";
constexpr const char *SizeKey = "size";
constexpr const char *DataKey = "data";
constexpr const char *PositionKey = "positionMs";
constexpr const char *RateKey = "rate";
constexpr const char *CommandKey = "command";
constexpr qint64 FileChunkSize = 48 * 1024;

// 每条信令消息使用一行紧凑 JSON，方便接收端按换行切包。
QByteArray encodeMessage(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}
}

WatchPartyManager::WatchPartyManager(QObject *parent)
    : QObject(parent)
{
}

WatchPartyManager::~WatchPartyManager()
{
    disconnectFromRoom();
}

bool WatchPartyManager::startHost(quint16 port, const QString &displayName)
{
    disconnectFromRoom();

    // 房主端先启动监听，再更新房间状态，避免 UI 先显示成功但端口实际上没起来。
    m_displayName = displayName.trimmed().isEmpty() ? QStringLiteral("房主") : displayName.trimmed();
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &WatchPartyManager::onNewConnection);

    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        const QString message = tr("创建房间失败：%1").arg(m_server->errorString());
        emit errorOccurred(message);
        disconnectFromRoom();
        return false;
    }

    m_role = WatchPartyRole::Host;
    m_hostPort = m_server->serverPort();
    emit chatMessageReceived(tr("系统"), tr("房间已创建，别人可以用右侧地址加入。"), false);
    emit chatMessageReceived(tr("系统"), roomAddressText(), false);
    emit roomStatusChanged(roomAddressText());
    sendPeerList();
    return true;
}

void WatchPartyManager::joinHost(const QString &hostAddress, quint16 port, const QString &displayName)
{
    disconnectFromRoom();

    // 客户端模式只维护一条到房主的长连接，后续所有同步都走这条 socket。
    m_displayName = displayName.trimmed().isEmpty() ? QStringLiteral("观众") : displayName.trimmed();
    m_role = WatchPartyRole::Client;
    m_hostPort = port;
    m_hostSocket = new QTcpSocket(this);
    attachSocket(m_hostSocket, tr("房主"));
    connect(m_hostSocket, &QTcpSocket::connected, this, &WatchPartyManager::onClientConnected);

    emit roomStatusChanged(tr("正在加入 %1:%2 ...").arg(hostAddress).arg(port));
    m_hostSocket->connectToHost(hostAddress, port);
}

void WatchPartyManager::disconnectFromRoom()
{
    // 无论当前是房主还是客户端，都统一清掉 socket、缓存和房间状态。
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

    const QList<QTcpSocket *> sockets = m_clients;
    for (QTcpSocket *socket : sockets) {
        socket->disconnect(this);
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    m_clients.clear();

    if (m_hostSocket) {
        m_hostSocket->disconnect(this);
        m_hostSocket->disconnectFromHost();
        m_hostSocket->deleteLater();
        m_hostSocket = nullptr;
    }

    resetRoomState();
}

WatchPartyRole WatchPartyManager::role() const
{
    return m_role;
}

bool WatchPartyManager::isConnected() const
{
    if (m_role == WatchPartyRole::Host) {
        return m_server && m_server->isListening();
    }

    return m_hostSocket && m_hostSocket->state() == QAbstractSocket::ConnectedState;
}

QString WatchPartyManager::roomAddressText() const
{
    if (m_role == WatchPartyRole::Host && m_server) {
        const QStringList addresses = localAddresses();
        const QString address = addresses.isEmpty() ? QStringLiteral("127.0.0.1") : addresses.first();
        return tr("房间地址：%1:%2").arg(address).arg(m_server->serverPort());
    }

    if (m_role == WatchPartyRole::Client && m_hostSocket) {
        return tr("已加入：%1:%2").arg(m_hostSocket->peerAddress().toString()).arg(m_hostPort);
    }

    return tr("未连接");
}

QString WatchPartyManager::localDisplayName() const
{
    return m_displayName;
}

void WatchPartyManager::sendChatMessage(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || m_role == WatchPartyRole::Offline) {
        return;
    }

    QJsonObject object;
    object.insert(TypeKey, QStringLiteral("chat"));
    object.insert(SenderKey, m_displayName);
    object.insert(TextKey, trimmed);

    if (m_role == WatchPartyRole::Host) {
        broadcastJson(object);
    } else if (m_hostSocket) {
        sendJson(m_hostSocket, object);
    }

    emit chatMessageReceived(m_displayName, trimmed, true);
}

void WatchPartyManager::broadcastMediaOpened(const QString &filePath, qint64 positionMs, double playbackRate)
{
    if (m_role != WatchPartyRole::Host || filePath.isEmpty()) {
        return;
    }

    // 这里只同步“应该打开哪个文件以及从哪里开始播”，不包含文件本体。
    updateHostMediaState(filePath, positionMs, playbackRate, true);

    QJsonObject object;
    object.insert(TypeKey, QStringLiteral("media"));
    object.insert(PathKey, filePath);
    object.insert(PositionKey, static_cast<double>(positionMs));
    object.insert(RateKey, playbackRate);
    broadcastJson(object);
}

void WatchPartyManager::broadcastPlaybackCommand(const QString &command, qint64 positionMs, double playbackRate)
{
    if (m_role != WatchPartyRole::Host) {
        return;
    }

    if (!m_hostMediaPath.isEmpty()) {
        bool playing = m_hostMediaPlaying;
        if (command == QStringLiteral("play") || command == QStringLiteral("sync")) {
            playing = true;
        } else if (command == QStringLiteral("pause") || command == QStringLiteral("stop")) {
            playing = false;
        }
        updateHostMediaState(m_hostMediaPath, positionMs, playbackRate, playing);
    }

    QJsonObject object;
    object.insert(TypeKey, QStringLiteral("playback"));
    object.insert(CommandKey, command);
    object.insert(PositionKey, static_cast<double>(positionMs));
    object.insert(RateKey, playbackRate);
    broadcastJson(object);
}

void WatchPartyManager::broadcastMediaFile(const QString &filePath, qint64 positionMs, double playbackRate)
{
    if (m_role != WatchPartyRole::Host || filePath.isEmpty() || !QFileInfo::exists(filePath)) {
        return;
    }

    // 发送文件前先更新房主端记录，确保后续新成员加入也能拿到同一份状态。
    updateHostMediaState(filePath, positionMs, playbackRate, m_hostMediaPlaying);

    for (QTcpSocket *socket : std::as_const(m_clients)) {
        sendFileToSocket(socket, filePath, positionMs, playbackRate);
    }
}

void WatchPartyManager::updateHostMediaState(const QString &filePath, qint64 positionMs, double playbackRate, bool playing)
{
    if (m_role != WatchPartyRole::Host || filePath.isEmpty()) {
        return;
    }

    m_hostMediaPath = filePath;
    m_hostMediaPositionMs = std::max<qint64>(0, positionMs);
    m_hostMediaPlaybackRate = playbackRate > 0.0 ? playbackRate : 1.0;
    m_hostMediaPlaying = playing;
}

void WatchPartyManager::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        attachSocket(socket, tr("观众%1").arg(m_clients.size() + 1));
        m_clients.append(socket);
        sendHello(socket);
        emit chatMessageReceived(tr("系统"), tr("%1 加入了房间").arg(socketName(socket)), false);
        sendPeerList();
        QTimer::singleShot(500, this, [this, socket]() {
            if (m_clients.contains(socket)) {
                // 稍等片刻再同步媒体，给对端留出时间完成 hello/peer 列表初始化。
                syncCurrentMediaToSocket(socket);
            }
        });
    }
}

void WatchPartyManager::onClientConnected()
{
    sendHello(m_hostSocket);
    emit chatMessageReceived(tr("系统"), tr("已加入房间。"), false);
    emit roomStatusChanged(roomAddressText());
}

void WatchPartyManager::onClientDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) {
        return;
    }

    const QString name = socketName(socket);
    m_clients.removeAll(socket);
    m_socketBuffers.remove(socket);
    m_peerNames.remove(socket);
    m_incomingTransfers.remove(socket);

    if (socket == m_hostSocket) {
        m_hostSocket = nullptr;
        resetRoomState();
        emit errorOccurred(tr("已断开房间连接"));
    } else {
        emit chatMessageReceived(tr("系统"), tr("%1 离开了房间").arg(name), false);
        sendPeerList();
    }

    socket->deleteLater();
}

void WatchPartyManager::onClientReadyRead()
{
    // TCP 是字节流，可能一次读到半条或多条消息。
    // 这里先把数据放进每个 socket 自己的缓存，再按换行拆成完整 JSON 处理。
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) {
        return;
    }

    QByteArray &buffer = m_socketBuffers[socket];
    buffer.append(socket->readAll());

    // 协议层按换行分隔一条条 JSON 消息，因此这里持续拆包直到没有完整行。
    int newlineIndex = -1;
    while ((newlineIndex = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newlineIndex);
        buffer.remove(0, newlineIndex + 1);
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject()) {
            handleMessage(socket, document.object());
        }
    }
}

void WatchPartyManager::resetRoomState()
{
    m_socketBuffers.clear();
    m_peerNames.clear();
    m_incomingTransfers.clear();
    m_hostMediaPath.clear();
    m_hostMediaPositionMs = 0;
    m_hostMediaPlaybackRate = 1.0;
    m_hostMediaPlaying = false;
    m_role = WatchPartyRole::Offline;
    m_hostPort = 0;
    emit roomStatusChanged(tr("未连接"));
    emit peerListChanged(QStringList());
}

void WatchPartyManager::attachSocket(QTcpSocket *socket, const QString &fallbackName)
{
    m_socketBuffers.insert(socket, QByteArray());
    m_peerNames.insert(socket, fallbackName);
    connect(socket, &QTcpSocket::readyRead, this, &WatchPartyManager::onClientReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &WatchPartyManager::onClientDisconnected);
    connect(socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        QTcpSocket *failedSocket = qobject_cast<QTcpSocket *>(sender());
        if (failedSocket) {
            // 网络错误统一上抛给 UI，让界面层决定如何提示用户。
            emit errorOccurred(tr("网络错误：%1").arg(failedSocket->errorString()));
        }
    });
}

void WatchPartyManager::sendJson(QTcpSocket *socket, const QJsonObject &object)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    socket->write(encodeMessage(object));
    socket->flush();
}

void WatchPartyManager::broadcastJson(const QJsonObject &object, QTcpSocket *exceptSocket)
{
    for (QTcpSocket *socket : std::as_const(m_clients)) {
        if (socket != exceptSocket) {
            sendJson(socket, object);
        }
    }
}

void WatchPartyManager::handleMessage(QTcpSocket *socket, const QJsonObject &object)
{
    // 同看协议的所有入口都在这里：昵称握手、成员列表、聊天、远程打开媒体、
    // 文件分片传输和播放控制都会转换成信号交给 UI 层处理。
    const QString type = object.value(TypeKey).toString();

    if (type == QStringLiteral("hello")) {
        // 初次握手时用对端昵称覆盖掉默认访客名。
        m_peerNames[socket] = object.value(SenderKey).toString(socketName(socket));
        sendPeerList();
        return;
    }

    if (type == QStringLiteral("peers")) {
        QStringList names;
        const QJsonArray peers = object.value(PeersKey).toArray();
        for (const QJsonValue &peer : peers) {
            names.append(peer.toString());
        }
        emit peerListChanged(names);
        return;
    }

    if (type == QStringLiteral("chat")) {
        const QString senderName = object.value(SenderKey).toString(socketName(socket));
        const QString text = object.value(TextKey).toString();
        emit chatMessageReceived(senderName, text, false);

        if (m_role == WatchPartyRole::Host) {
            broadcastJson(object, socket);
        }
        return;
    }

    if (type == QStringLiteral("media")) {
        emit remoteOpenMediaRequested(
            object.value(PathKey).toString(),
            static_cast<qint64>(object.value(PositionKey).toDouble()),
            object.value(RateKey).toDouble(1.0));
        return;
    }

    if (type == QStringLiteral("file-start")) {
        IncomingFileTransfer transfer;
        transfer.fileName = object.value(FileNameKey).toString();
        transfer.totalBytes = static_cast<qint64>(object.value(SizeKey).toDouble());
        transfer.positionMs = static_cast<qint64>(object.value(PositionKey).toDouble());
        transfer.playbackRate = object.value(RateKey).toDouble(1.0);
        transfer.filePath = receivedMediaCachePath(transfer.fileName);

        QFile::remove(transfer.filePath);
        m_incomingTransfers.insert(socket, transfer);
        // 开始接收前先通知 UI 重置进度条。
        emit mediaTransferProgress(transfer.fileName, 0, transfer.totalBytes);
        return;
    }

    if (type == QStringLiteral("file-chunk")) {
        if (!m_incomingTransfers.contains(socket)) {
            return;
        }

        IncomingFileTransfer transfer = m_incomingTransfers.value(socket);
        QFile file(transfer.filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            emit errorOccurred(tr("接收视频失败：无法写入 %1").arg(transfer.filePath));
            m_incomingTransfers.remove(socket);
            return;
        }

        const QByteArray chunk = QByteArray::fromBase64(object.value(DataKey).toString().toUtf8());
        file.write(chunk);
        file.close();

        // 每收到一个分片都更新累计字节数，方便界面显示实时接收进度。
        transfer.receivedBytes += chunk.size();
        m_incomingTransfers.insert(socket, transfer);
        emit mediaTransferProgress(transfer.fileName, transfer.receivedBytes, transfer.totalBytes);
        return;
    }

    if (type == QStringLiteral("file-end")) {
        if (!m_incomingTransfers.contains(socket)) {
            return;
        }

        const IncomingFileTransfer transfer = m_incomingTransfers.take(socket);
        emit mediaTransferProgress(transfer.fileName, transfer.totalBytes, transfer.totalBytes);
        emit mediaFileReceived(transfer.filePath, transfer.positionMs, transfer.playbackRate);
        return;
    }

    if (type == QStringLiteral("playback")) {
        emit remotePlaybackCommandReceived(
            object.value(CommandKey).toString(),
            static_cast<qint64>(object.value(PositionKey).toDouble()),
            object.value(RateKey).toDouble(1.0));
        return;
    }
}

void WatchPartyManager::sendFileToSocket(QTcpSocket *socket, const QString &filePath, qint64 positionMs, double playbackRate)
{
    // 文件传输分为 file-start / file-chunk / file-end 三类消息。
    // 这样可以复用现有 JSON 信令通道，同时让接收端实时显示进度。
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit errorOccurred(tr("发送视频失败：无法读取 %1").arg(filePath));
        return;
    }

    const QFileInfo fileInfo(filePath);

    QJsonObject start;
    start.insert(TypeKey, QStringLiteral("file-start"));
    start.insert(FileNameKey, fileInfo.fileName());
    start.insert(SizeKey, static_cast<double>(file.size()));
    start.insert(PositionKey, static_cast<double>(positionMs));
    start.insert(RateKey, playbackRate);
    sendJson(socket, start);

    while (!file.atEnd()) {
        const QByteArray chunk = file.read(FileChunkSize);
        QJsonObject part;
        part.insert(TypeKey, QStringLiteral("file-chunk"));
        // 这里为了复用 JSON 信令通道，直接把二进制数据做 Base64 分片传输。
        part.insert(DataKey, QString::fromLatin1(chunk.toBase64()));
        sendJson(socket, part);
    }

    QJsonObject end;
    end.insert(TypeKey, QStringLiteral("file-end"));
    sendJson(socket, end);
    emit chatMessageReceived(tr("系统"), tr("已发送视频：%1").arg(fileInfo.fileName()), true);
}

void WatchPartyManager::syncCurrentMediaToSocket(QTcpSocket *socket)
{
    // 新成员刚加入时先拿到完整媒体文件，再延迟收到播放状态。
    // 延迟发送播放状态可以避免接收端还没完成写盘/打开文件就被立即 seek。
    if (m_role != WatchPartyRole::Host || m_hostMediaPath.isEmpty()) {
        return;
    }

    // 新成员入房时先补发媒体文件，再发送播放状态，尽量减少黑屏等待。
    sendFileToSocket(socket, m_hostMediaPath, m_hostMediaPositionMs, m_hostMediaPlaybackRate);

    QTimer::singleShot(1200, this, [this, socket]() {
        if (!m_clients.contains(socket) || m_hostMediaPath.isEmpty()) {
            return;
        }

        QJsonObject playback;
        playback.insert(TypeKey, QStringLiteral("playback"));
        playback.insert(CommandKey, m_hostMediaPlaying ? QStringLiteral("play") : QStringLiteral("pause"));
        playback.insert(PositionKey, static_cast<double>(m_hostMediaPositionMs));
        playback.insert(RateKey, m_hostMediaPlaybackRate);
        sendJson(socket, playback);
    });
}

QString WatchPartyManager::receivedMediaCachePath(const QString &fileName) const
{
    // 接收到的视频统一落到应用缓存目录，避免污染用户原始媒体目录。
    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/received_media");
    QDir().mkpath(baseDir);

    QString safeName = QFileInfo(fileName).fileName();
    if (safeName.isEmpty()) {
        safeName = QStringLiteral("watch_party_video.mp4");
    }

    return QDir(baseDir).absoluteFilePath(safeName);
}

void WatchPartyManager::sendHello(QTcpSocket *socket)
{
    QJsonObject object;
    object.insert(TypeKey, QStringLiteral("hello"));
    object.insert(SenderKey, m_displayName);
    sendJson(socket, object);
}

void WatchPartyManager::sendPeerList()
{
    QStringList names;
    names.append(m_displayName.isEmpty() ? tr("我") : m_displayName);
    names.append(peerNames());

    emit peerListChanged(names);

    if (m_role != WatchPartyRole::Host) {
        return;
    }

    QJsonArray peers;
    for (const QString &name : names) {
        peers.append(name);
    }

    QJsonObject object;
    object.insert(TypeKey, QStringLiteral("peers"));
    object.insert(PeersKey, peers);
    broadcastJson(object);
}

QString WatchPartyManager::socketName(QTcpSocket *socket) const
{
    return m_peerNames.value(socket, tr("访客"));
}

QStringList WatchPartyManager::peerNames() const
{
    QStringList names;
    for (QTcpSocket *socket : m_clients) {
        names.append(socketName(socket));
    }
    return names;
}

QStringList WatchPartyManager::localAddresses() const
{
    QStringList addresses;
    const QList<QHostAddress> entries = QNetworkInterface::allAddresses();
    for (const QHostAddress &address : entries) {
        // 这里只取本机可用的 IPv4 地址，方便直接展示给局域网内其他成员。
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) {
            addresses.append(address.toString());
        }
    }
    return addresses;
}
