// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/snapshot_client.h"

#include <QTimer>

namespace ball::ui {
namespace {
constexpr int kReconnectIntervalMs = 1000;
constexpr int kMaxErrorLength = 120;
} // namespace

SnapshotClient::SnapshotClient(QObject* parent) : QObject(parent) {
    m_socket = new QLocalSocket(this);
    // One frame above the cap, so a peer that never finishes a frame cannot make
    // the kernel buffer, the decoder or this process grow without bound.
    m_socket->setReadBufferSize(mp::kMaxFrameBytes);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(kReconnectIntervalMs);
    m_staleTimer = new QTimer(this);
    m_staleTimer->setSingleShot(true);
    m_staleTimer->setInterval(kSnapshotTimeoutMs);

    connect(m_reconnectTimer, &QTimer::timeout, this, &SnapshotClient::connectSocket);
    connect(m_staleTimer, &QTimer::timeout, this, &SnapshotClient::onSnapshotTimeout);
    connect(m_socket, &QLocalSocket::connected, this, &SnapshotClient::onConnected);
    connect(m_socket, &QLocalSocket::readyRead, this, &SnapshotClient::onReadyRead);
    connect(m_socket, &QLocalSocket::disconnected, this, &SnapshotClient::onDisconnected);
    connect(m_socket, &QLocalSocket::errorOccurred, this, &SnapshotClient::onError);
}

void SnapshotClient::start(const QString& socketPath) {
    m_path = socketPath.toUtf8();
    m_running = true;
    m_reconnectTimer->stop();
    m_socket->abort();
    m_decoder.reset();
    m_stale = true;
    m_incomplete = true;
    if (m_path.isEmpty()) {
        m_error = QStringLiteral("no socket path");
        Q_EMIT updated();
        return;
    }
    connectSocket();
    Q_EMIT updated();
}

void SnapshotClient::stop() {
    m_running = false;
    m_reconnectTimer->stop();
    m_staleTimer->stop();
    m_socket->abort();
    m_decoder.reset();
    m_stale = true;
    Q_EMIT updated();
}

void SnapshotClient::connectSocket() {
    if (!m_running || m_path.isEmpty()) {
        return;
    }
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        return;
    }
    m_reconnectTimer->stop();
    m_socket->connectToServer(QString::fromUtf8(m_path));
}

void SnapshotClient::onConnected() {
    m_reconnectTimer->stop();
    m_decoder.reset();
    m_staleTimer->start();
    Q_EMIT updated();
}

void SnapshotClient::onReadyRead() {
    m_decoder.feed(m_socket->readAll());
    if (m_decoder.failed()) {
        dropConnection(QString::fromLatin1(m_decoder.error()));
        return;
    }

    while (true) {
        mp::Frame frame = m_decoder.next();
        if (!frame.valid()) {
            break; // nothing complete left, or a transport-level failure
        }
        const SnapshotParseResult result = decodeSnapshot(frame.value());
        if (!result.snapshot.has_value()) {
            // Rejecting the whole frame keeps the previous snapshot intact; the
            // connection is dropped so the reason is visible and a clean
            // snapshot can take over after the reconnect.
            dropConnection(result.error.isEmpty() ? QStringLiteral("bad frame") : result.error);
            return;
        }
        m_snapshot = result.snapshot;
        m_stale = false;
        m_incomplete = false;
        m_error.clear();
        m_staleTimer->start();
        Q_EMIT updated();
    }
    if (m_decoder.failed()) {
        dropConnection(QString::fromLatin1(m_decoder.error()));
    }
}

void SnapshotClient::onDisconnected() {
    m_staleTimer->stop();
    m_stale = true;
    m_incomplete = true;
    m_decoder.reset();
    if (m_error.isEmpty()) {
        m_error = QStringLiteral("disconnected");
    }
    if (m_running) {
        startReconnect();
    }
    Q_EMIT updated();
}

void SnapshotClient::onError() {
    dropConnection(m_socket->error() == QLocalSocket::PeerClosedError
                       ? QStringLiteral("backend connection closed")
                       : m_socket->errorString());
}

void SnapshotClient::onSnapshotTimeout() {
    m_stale = true;
    m_incomplete = true;
    if (m_error.isEmpty()) {
        m_error = QStringLiteral("no snapshot");
    }
    Q_EMIT updated();
}

void SnapshotClient::dropConnection(const QString& reason) {
    m_error = reason.left(kMaxErrorLength);
    m_staleTimer->stop();
    m_socket->abort();
    m_stale = true;
    m_incomplete = true;
    if (m_running) {
        startReconnect();
    }
    Q_EMIT updated();
}

void SnapshotClient::startReconnect() {
    if (!m_running || m_reconnectTimer->isActive()) {
        return;
    }
    m_reconnectTimer->start();
}

} // namespace ball::ui
