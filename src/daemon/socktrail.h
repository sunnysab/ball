// ball: a network traffic HUD for KDE Wayland sessions.
// Asynchronous subscriber for the socktrail Unix-socket MessagePack stream.
#pragma once

#include "daemon/connections.h"
#include "daemon/processes.h"
#include "messagepack.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace ball {

// Result of decoding one complete MessagePack frame. A frame is fully parsed and
// validated before any of it reaches the accumulator, so a rejected frame
// leaves no partial state behind.
enum class SocktrailFrameStatus : quint8 {
    Ok,            // kind "stats": `counters` holds the whole process_io table
    Flow,          // kind "flow": `connection` holds one validated connection row
    Hello,         // kind "hello": version/started validated, `backendStarted`
    ProtocolError, // wrong/missing kind, bad field, duplicate identity or bad number
    Unsupported,   // kind "hello" with a version this client cannot read
};

// One --output msgpack frame, kept out of the client so it can be tested alone.
struct SocktrailFrame {
    SocktrailFrameStatus status = SocktrailFrameStatus::ProtocolError;
    QDateTime at;                       // stats: "at"
    QDateTime backendStarted;           // hello: "started"
    QList<ProcessCounter> counters;     // stats: the full process_io table
    quint64 ipPackets = 0;              // stats: collector-run IP packet count
    quint64 ipBytes = 0;                // stats: collector-run IP bytes, not socket I/O
    QStringList qualityNotes;           // stats: PID events lost / unindexed bytes
    QStringList infoNotes;              // stats: explanations that are not a gap
    QString error;                      // short ASCII reason for a rejected frame
    ConnectionSnapshot connection;      // flow: one validated connection row
    bool connectionsAdvertised = false; // hello: "kinds" names flow
};

// Parses one complete frame. `frame` is the decoded object and the caller keeps
// the storage alive, because the validated fields are copied out before it goes
// away. Flow frames are validated as one connection row; the process totals may
// only come from the stats table.
SocktrailFrame decodeSocktrailFrame(const msgpack::object& frame);

// Subscribes to a socktrail `--socket` MessagePack stream and keeps a Processes
// accumulator fed from the periodic stats frames. Everything is asynchronous:
// no call blocks the event loop. Lifetime is plain parent/child ownership.
class Socktrail : public QObject {
    Q_OBJECT

public:
    explicit Socktrail(QObject* parent = nullptr);
    ~Socktrail() override;

    // Starts (or restarts) the subscription on `path`, which is the socktrail
    // Unix socket, not a filesystem path outside the socket namespace. Calling
    // it twice replaces the previous subscription. Retained process totals are
    // kept, but the new subscription starts out unavailable and incomplete.
    void start(const QString& path);
    // Stops for good: the connection is closed and no reconnect is scheduled.
    void stop();

    // Accumulated per-process totals, empty until the first stats frame.
    const Processes& processes() const { return m_processes; }
    // True only after a complete stats frame on a connected socket. A connected
    // socket alone is not enough, and every reconnect drops it again.
    bool available() const { return m_available; }
    // True while the observed history has a gap the client cannot repair: no
    // stats yet, a disconnect, a backend restart, a stale window or a stats
    // frame carrying quality problems. It stays true for the rest of the
    // subscription once a gap happened.
    bool incomplete() const { return m_incomplete; }
    // True after kStatsTimeoutMs without a stats frame, or while disconnected.
    // Flow traffic alone never marks the data fresh.
    bool stale() const { return m_stale; }
    // Empty while healthy, otherwise a short ASCII reason.
    QByteArray error() const { return m_error; }
    // Quality notes (PID events lost, unindexed bytes) for the UI. They are
    // never added to any per-process total and they mark the observation as
    // incomplete.
    QStringList qualityNotes() const { return m_qualityNotes; }
    // Explanations that do not describe a data gap (unmatched socket I/O,
    // expired PIDs, a non-active kTLS probe) for the UI.
    QStringList infoNotes() const { return m_infoNotes; }

    // Bounded cache of live and finished connections. Empty until a flow frame
    // arrives, and never fed from the stats table.
    const Connections& connections() const { return m_connections; }
    // True once a hello advertised flow, or a real flow frame was seen from a
    // collector that did not advertise. Every reconnect requires a new proof.
    bool connectionsAvailable() const { return m_connectionsAvailable; }
    quint64 ipPackets() const { return m_ipPackets; }
    quint64 ipBytes() const { return m_ipBytes; }

    // How long a stats frame may be missing before stale() turns true. The
    // window starts when the socket connects, so a peer that only sends flow
    // frames still turns stale and reports a reason.
    static constexpr int kStatsTimeoutMs = 5000;

Q_SIGNALS:
    // Any change of data, availability, staleness or error.
    void updated();

private:
    void connectSocket();
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void onStatsTimeout();
    // Returns false when the frame broke the connection and the read buffer
    // must be discarded.
    bool handleFrame(const msgpack::object& frame);
    // Applies one validated stats sample. Returns false when it was rejected.
    bool applyStats(const SocktrailFrame& stats, qint64 monotonicMs);
    // Applies a sample that was deferred because its millisecond had not moved.
    void flushPendingStats();
    void dropConnection(const QString& reason);
    void setError(const QString& reason);
    void startReconnect();

    Processes m_processes;
    Connections m_connections;
    QLocalSocket* m_socket = nullptr;
    QTimer* m_reconnectTimer = nullptr;
    QTimer* m_staleTimer = nullptr;
    QTimer* m_flushTimer = nullptr;
    // Started once and never restarted: Processes keeps its monotonic history,
    // so resetting would send it backwards in time.
    QElapsedTimer m_clock;
    QString m_path;
    mp::Decoder m_decoder;
    QByteArray m_error;
    QStringList m_qualityNotes;
    QStringList m_infoNotes;
    QDateTime m_backendStarted;
    SocktrailFrame m_pendingStats;
    quint64 m_ipPackets = 0;
    quint64 m_ipBytes = 0;
    qint64 m_lastAppliedMonoMs = -1;
    bool m_running = false;
    bool m_helloReceived = false;
    bool m_restartPending = false; // next stats frame must rebaseline
    bool m_gapSeen = false;        // a gap happened during this subscription
    bool m_hasPendingStats = false;
    bool m_available = false;
    bool m_incomplete = true;
    bool m_stale = true;
    bool m_connectionsAvailable = false;
};

} // namespace ball
