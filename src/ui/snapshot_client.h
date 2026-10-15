// ball: a network traffic HUD for KDE Wayland sessions.
// Asynchronous, read-only subscriber to the local snapshot socket.
#pragma once

#include "ui/snapshot.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QTimer>

namespace ball::ui {

// Reads the GUI snapshot socket defined in docs/protocol.md and keeps only the
// newest whole snapshot. The socket is read-only: this class never sends a
// control command, and closing it does not touch the backend. Every parse is
// all-or-nothing, so a bad frame leaves the previous snapshot in place.
class SnapshotClient : public QObject {
    Q_OBJECT

public:
    explicit SnapshotClient(QObject* parent = nullptr);

    // Connects to `socketPath` and reconnects about once a second until stopped.
    void start(const QString& socketPath);
    void stop();

    // Newest complete snapshot, or nullopt before the first one arrives.
    const std::optional<Snapshot>& snapshot() const { return m_snapshot; }
    // True only while a snapshot is fresh: a connected socket or a stale
    // snapshot is not enough.
    bool fresh() const { return m_snapshot.has_value() && !m_stale; }
    // True after a disconnect, a rejected frame or a timeout made the shown
    // data untrustworthy. Sticky for the rest of the monitoring session.
    bool incomplete() const { return m_incomplete; }
    // Empty while healthy, otherwise a short ASCII reason for the UI.
    QString error() const { return m_error; }

    // A snapshot older than this is stale; the UI must not show its rates as
    // live zeros.
    static constexpr int kSnapshotTimeoutMs = 5000;
    // Upper bound for one buffered frame, to bound memory on a broken peer.
    static constexpr qint64 kMaxLineBytes = 16LL * 1024 * 1024;

Q_SIGNALS:
    // Any change of snapshot, freshness, completeness or error.
    void updated();

private:
    void connectSocket();
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onError();
    void onSnapshotTimeout();
    void dropConnection(const QString& reason);
    void startReconnect();

    QLocalSocket* m_socket = nullptr;
    QTimer* m_reconnectTimer = nullptr;
    QTimer* m_staleTimer = nullptr;
    QByteArray m_buffer;
    QByteArray m_path;
    QString m_error;
    std::optional<Snapshot> m_snapshot;
    bool m_running = false;
    bool m_stale = true;
    // Sticky: once the shown history has a gap it cannot be repaired in-process.
    bool m_incomplete = true;
};

} // namespace ball::ui
