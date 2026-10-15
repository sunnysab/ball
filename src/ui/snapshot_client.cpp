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
    m_socket->setReadBufferSize(kMaxLineBytes);
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
    m_buffer.clear();
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
    m_buffer.clear();
    m_stale = true;
    Q_EMIT updated();
}

void SnapshotClient::connectSocket() {
    if (!m_running || m_path.isEmpty()) {
        return;
    }
    m_reconnectTimer->stop();
    m_socket->connectToServer(QString::fromUtf8(m_path));
}

void SnapshotClient::onConnected() {
    m_buffer.clear();
    m_staleTimer->start();
    Q_EMIT updated();
}

void SnapshotClient::onReadyRead() {
    m_buffer += m_socket->readAll();

    qsizetype newline = -1;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        if (newline > kMaxLineBytes) {
            dropConnection(QStringLiteral("frame too large"));
            return;
        }
        QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.trimmed().isEmpty()) {
            continue; // NDJSON allows blank keep-alive lines
        }
        const SnapshotParseResult result = parseSnapshot(line);
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

    if (m_buffer.size() >= kMaxLineBytes) {
        dropConnection(QStringLiteral("frame too large"));
    }
}

void SnapshotClient::onDisconnected() {
    m_stale = true;
    m_incomplete = true;
    m_buffer.clear();
    if (m_error.isEmpty()) {
        m_error = QStringLiteral("disconnected");
    }
    if (m_running) {
        startReconnect();
    }
    Q_EMIT updated();
}

void SnapshotClient::onError() { dropConnection(m_socket->errorString()); }

void SnapshotClient::onSnapshotTimeout() {
    m_stale = true;
    m_incomplete = true;
    if (m_error.isEmpty()) {
        m_error = QStringLiteral("no fresh snapshot");
    }
    Q_EMIT updated();
}

void SnapshotClient::dropConnection(const QString& reason) {
    m_error = reason.left(kMaxErrorLength);
    m_buffer.clear();
    m_stale = true;
    m_incomplete = true;
    m_socket->abort();
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
