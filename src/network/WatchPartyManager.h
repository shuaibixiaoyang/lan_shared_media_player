#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

class QTcpServer;
class QTcpSocket;

enum class WatchPartyRole
{
    Offline,
    Host,
    Client
};

// 管理局域网同看房间：负责建房、入房、聊天、播放指令同步以及媒体文件传输。
class WatchPartyManager : public QObject
{
    Q_OBJECT

public:
    explicit WatchPartyManager(QObject *parent = nullptr);
    ~WatchPartyManager() override;

    // 创建同看房间并开始监听局域网连接，当前客户端成为房主。
    bool startHost(quint16 port, const QString &displayName);
    // 连接到房主 IP 和端口，当前客户端成为观众。
    void joinHost(const QString &hostAddress, quint16 port, const QString &displayName);
    // 主动离开房间或关闭房间，并清理所有 socket 与缓存状态。
    void disconnectFromRoom();

    WatchPartyRole role() const;
    bool isConnected() const;
    QString roomAddressText() const;
    QString localDisplayName() const;

    void sendChatMessage(const QString &text);
    // 房主广播“打开哪个媒体、从哪个位置开始”，用于本地路径已经可用的场景。
    void broadcastMediaOpened(const QString &filePath, qint64 positionMs, double playbackRate);
    // 房主广播播放、暂停、停止、seek、倍速和周期同步等控制指令。
    void broadcastPlaybackCommand(const QString &command, qint64 positionMs, double playbackRate);
    // 房主把当前媒体文件切片发送给观众，解决观众本地没有同路径文件的问题。
    void broadcastMediaFile(const QString &filePath, qint64 positionMs, double playbackRate);
    // 记录房主当前媒体状态，方便新成员加入时补发当前播放上下文。
    void updateHostMediaState(const QString &filePath, qint64 positionMs, double playbackRate, bool playing);

signals:
    void roomStatusChanged(const QString &statusText);
    void peerListChanged(const QStringList &peers);
    void chatMessageReceived(const QString &sender, const QString &text, bool isLocal);
    void remoteOpenMediaRequested(const QString &filePath, qint64 positionMs, double playbackRate);
    void remotePlaybackCommandReceived(const QString &command, qint64 positionMs, double playbackRate);
    void mediaTransferProgress(const QString &fileName, qint64 receivedBytes, qint64 totalBytes);
    void mediaFileReceived(const QString &filePath, qint64 positionMs, double playbackRate);
    void errorOccurred(const QString &message);

private slots:
    void onNewConnection();
    void onClientConnected();
    void onClientDisconnected();
    void onClientReadyRead();

private:
    // 重置房间角色、成员、文件传输和房主媒体状态。
    void resetRoomState();
    // 给 socket 绑定 readyRead/disconnected/error 处理，并登记默认昵称。
    void attachSocket(QTcpSocket *socket, const QString &fallbackName);
    // 发送一条以换行结尾的 JSON 信令消息。
    void sendJson(QTcpSocket *socket, const QJsonObject &object);
    // 房主向所有观众广播信令，可选择跳过消息来源 socket。
    void broadcastJson(const QJsonObject &object, QTcpSocket *exceptSocket = nullptr);
    // 解析并执行收到的 chat/media/file/playback 等协议消息。
    void handleMessage(QTcpSocket *socket, const QJsonObject &object);
    void sendHello(QTcpSocket *socket);
    void sendPeerList();
    // 通过 JSON + Base64 分片发送媒体文件。
    void sendFileToSocket(QTcpSocket *socket, const QString &filePath, qint64 positionMs, double playbackRate);
    // 新成员加入时，把房主当前媒体文件和播放状态补发给该成员。
    void syncCurrentMediaToSocket(QTcpSocket *socket);
    QString receivedMediaCachePath(const QString &fileName) const;
    QString socketName(QTcpSocket *socket) const;
    QStringList peerNames() const;
    QStringList localAddresses() const;

    // 用于记录某个 socket 当前正在接收的视频文件状态。
    struct IncomingFileTransfer
    {
        QString fileName;
        QString filePath;
        qint64 totalBytes = 0;
        qint64 receivedBytes = 0;
        qint64 positionMs = 0;
        double playbackRate = 1.0;
    };

    // Host 模式下负责监听新连接；Client 模式下为空。
    QTcpServer *m_server = nullptr;
    // Client 模式下连接到房主的 socket；Host 模式下为空。
    QTcpSocket *m_hostSocket = nullptr;
    QList<QTcpSocket *> m_clients;
    // 按连接缓存未处理完的字节流，便于按行拆分 JSON 消息。
    QHash<QTcpSocket *, QByteArray> m_socketBuffers;
    QHash<QTcpSocket *, QString> m_peerNames;
    QHash<QTcpSocket *, IncomingFileTransfer> m_incomingTransfers;

    WatchPartyRole m_role = WatchPartyRole::Offline;
    QString m_displayName;
    // 房主端保存当前媒体状态，便于新成员加入后立即同步。
    QString m_hostMediaPath;
    qint64 m_hostMediaPositionMs = 0;
    double m_hostMediaPlaybackRate = 1.0;
    bool m_hostMediaPlaying = false;
    quint16 m_hostPort = 0;
};
