// ball: a network traffic HUD for KDE Wayland sessions.
#include "daemon/socktrail.h"

#include <QSet>
#include <QSignalBlocker>
#include <QStringList>

#include <limits>
#include <utility>

namespace ball {
namespace {

constexpr int kReconnectIntervalMs = 1000;
constexpr int kMaxErrorLength = 120;
constexpr quint64 kIntMax = quint64(std::numeric_limits<int>::max());

SocktrailFrame reject(const char* reason) {
    SocktrailFrame frame;
    frame.status = SocktrailFrameStatus::ProtocolError;
    frame.error = QLatin1String(reason);
    return frame;
}

bool readBytes(const msgpack::object& value, quint64& rx, quint64& tx) {
    const msgpack::object* const rxValue = mp::member(value, QLatin1StringView("rx_bytes"));
    const msgpack::object* const txValue = mp::member(value, QLatin1StringView("tx_bytes"));
    return rxValue != nullptr && txValue != nullptr && mp::readCount(*rxValue, rx) &&
           mp::readCount(*txValue, tx);
}

// The collector's MessagePack timestamp, converted to local wall-clock time.
bool readTimestamp(const msgpack::object* const value, QDateTime& out) {
    if (value == nullptr) {
        return false;
    }
    mp::Instant instant;
    if (!mp::readInstant(*value, instant)) {
        return false;
    }
    const QDateTime parsed = mp::timestampToLocalTime(instant);
    if (!parsed.isValid()) {
        return false;
    }
    out = parsed;
    return true;
}

SocktrailFrame parseHello(const msgpack::object& hello) {
    quint64 version = 0;
    const msgpack::object* const versionValue = mp::member(hello, QLatin1StringView("version"));
    if (versionValue == nullptr || !mp::readCount(*versionValue, version)) {
        return reject("bad version");
    }
    if (version != 1) {
        SocktrailFrame frame;
        frame.status = SocktrailFrameStatus::Unsupported;
        frame.error = QLatin1String("unsupported version");
        return frame;
    }
    SocktrailFrame frame;
    if (!readTimestamp(mp::member(hello, QLatin1StringView("started")), frame.backendStarted)) {
        return reject("bad started");
    }
    frame.status = SocktrailFrameStatus::Hello;
    return frame;
}

SocktrailFrame parseStats(const msgpack::object& stats) {
    SocktrailFrame frame;
    if (!readTimestamp(mp::member(stats, QLatin1StringView("at")), frame.at)) {
        return reject("bad at");
    }

    const msgpack::object* const ipPackets = mp::member(stats, QLatin1StringView("ip_packets"));
    const msgpack::object* const ipBytes = mp::member(stats, QLatin1StringView("ip_bytes"));
    if (ipPackets != nullptr && !mp::readCount(*ipPackets, frame.ipPackets)) {
        return reject("bad ip_packets");
    }
    if (ipBytes != nullptr && !mp::readCount(*ipBytes, frame.ipBytes)) {
        return reject("bad ip_bytes");
    }

    const msgpack::object* processes = nullptr;
    quint32 processCount = 0;
    const msgpack::object* const processValue = mp::member(stats, QLatin1StringView("process_io"));
    if (processValue == nullptr || !mp::arrayEntries(*processValue, processes, processCount)) {
        return reject("bad process_io");
    }

    QSet<QPair<int, quint64>> identities;
    for (quint32 i = 0; i < processCount; ++i) {
        const msgpack::object& process = processes[i];
        quint64 pid = 0;
        // PID -1 marks a process the backend could not attribute and PID 0 never
        // names a process; neither is a usable identity.
        const msgpack::object* const pidValue = mp::member(process, QLatin1StringView("pid"));
        if (pidValue == nullptr || !mp::readCount(*pidValue, pid) || pid == 0 || pid > kIntMax) {
            return reject("bad pid");
        }
        // start_ns is omitted when it is zero, and 0 is the value the collector
        // itself cannot distinguish, so an absent field is the same identity.
        quint64 startNs = 0;
        const msgpack::object* const startValue =
            mp::member(process, QLatin1StringView("start_ns"));
        if (startValue != nullptr && !mp::readCount(*startValue, startNs)) {
            return reject("bad start_ns");
        }
        QString name;
        const msgpack::object* const nameValue = mp::member(process, QLatin1StringView("name"));
        if (nameValue != nullptr) {
            bool ok = false;
            name = mp::ascii(*nameValue, ok);
            if (!ok) {
                return reject("bad name");
            }
        }
        quint64 rx = 0;
        quint64 tx = 0;
        const msgpack::object* const rxValue = mp::member(process, QLatin1StringView("rx_bytes"));
        const msgpack::object* const txValue = mp::member(process, QLatin1StringView("tx_bytes"));
        if (rxValue == nullptr || txValue == nullptr || !mp::readCount(*rxValue, rx) ||
            !mp::readCount(*txValue, tx)) {
            return reject("bad byte counters");
        }
        const QPair<int, quint64> identity{int(pid), startNs};
        if (identities.contains(identity)) {
            return reject("duplicate pid identity");
        }
        identities.insert(identity);

        ProcessCounter counter;
        counter.pid = identity.first;
        counter.startNs = identity.second;
        counter.name = name;
        counter.rxBytes = rx;
        counter.txBytes = tx;
        frame.counters.append(counter);
    }

    quint64 kernelLost = 0;
    quint64 dropped = 0;
    quint64 invalid = 0;
    // member() returns nullptr for a missing key and for a value that is not a
    // map, so both mistakes land on the same reject.
    const msgpack::object* const probesValue = mp::member(stats, QLatin1StringView("probes"));
    if (probesValue == nullptr) {
        return reject("no probes");
    }
    const msgpack::object* const pidProbe = mp::member(*probesValue, QLatin1StringView("pid"));
    if (pidProbe == nullptr) {
        return reject("no probes.pid");
    }
    const msgpack::object* const kernelLostValue =
        mp::member(*pidProbe, QLatin1StringView("kernel_lost"));
    const msgpack::object* const droppedValue = mp::member(*pidProbe, QLatin1StringView("dropped"));
    const msgpack::object* const invalidValue = mp::member(*pidProbe, QLatin1StringView("invalid"));
    if (kernelLostValue == nullptr || droppedValue == nullptr || invalidValue == nullptr ||
        !mp::readCount(*kernelLostValue, kernelLost) || !mp::readCount(*droppedValue, dropped) ||
        !mp::readCount(*invalidValue, invalid)) {
        return reject("bad probes.pid");
    }
    if (kernelLost != 0 || dropped != 0 || invalid != 0) {
        frame.qualityNotes.append(
            QStringLiteral("PID events: kernel_lost %1, dropped %2, invalid %3")
                .arg(kernelLost)
                .arg(dropped)
                .arg(invalid));
    }

    quint64 unindexedRx = 0;
    quint64 unindexedTx = 0;
    quint64 expiredRx = 0;
    quint64 expiredTx = 0;
    const msgpack::object* const unindexed = mp::member(stats, QLatin1StringView("io_unindexed"));
    const msgpack::object* const expired = mp::member(stats, QLatin1StringView("expired_pid_io"));
    if (unindexed == nullptr || expired == nullptr ||
        !readBytes(*unindexed, unindexedRx, unindexedTx) ||
        !readBytes(*expired, expiredRx, expiredTx)) {
        return reject("bad io totals");
    }
    quint64 expiredCount = 0;
    const msgpack::object* const expiredCountValue =
        mp::member(stats, QLatin1StringView("expired_pid_count"));
    if (expiredCountValue == nullptr || !mp::readCount(*expiredCountValue, expiredCount)) {
        return reject("bad expired_pid_count");
    }
    // Socket I/O the collector could not attach to any connection. It is absent
    // from every flow's io[] and from the process table, so it is the one gap a
    // process total cannot show by itself.
    quint64 unmatchedBytes = 0;
    const msgpack::object* const unmatchedValue =
        mp::member(stats, QLatin1StringView("io_unmatched_bytes"));
    if (unmatchedValue == nullptr || !mp::readCount(*unmatchedValue, unmatchedBytes)) {
        return reject("bad io_unmatched_bytes");
    }
    if (expiredCount != 0 || expiredRx != 0 || expiredTx != 0) {
        frame.qualityNotes.append(QStringLiteral("expired PIDs: %1 identities, RX %2, TX %3")
                                      .arg(expiredCount)
                                      .arg(expiredRx)
                                      .arg(expiredTx));
    }
    if (unindexedRx != 0 || unindexedTx != 0) {
        frame.qualityNotes.append(
            QStringLiteral("unindexed bytes: RX %1, TX %2").arg(unindexedRx).arg(unindexedTx));
    }
    if (unmatchedBytes != 0) {
        frame.qualityNotes.append(
            QStringLiteral("unmatched socket I/O: %1 bytes").arg(unmatchedBytes));
    }

    frame.status = SocktrailFrameStatus::Ok;
    return frame;
}

} // namespace

SocktrailFrame decodeSocktrailFrame(const msgpack::object& frame) {
    const msgpack::object* const kindValue = mp::member(frame, QLatin1StringView("kind"));
    bool ok = false;
    const QString kind = kindValue != nullptr ? mp::ascii(*kindValue, ok) : QString();
    if (!ok) {
        return reject("no kind");
    }
    // Flow frames carry per-connection increments; process totals may only come
    // from stats, so a flow frame is accepted and dropped without any state.
    if (kind == QLatin1String("flow")) {
        SocktrailFrame ignored;
        ignored.status = SocktrailFrameStatus::Ignored;
        return ignored;
    }
    if (kind == QLatin1String("hello")) {
        const msgpack::object* const helloValue = mp::member(frame, QLatin1StringView("hello"));
        if (helloValue == nullptr) {
            return reject("no hello");
        }
        return parseHello(*helloValue);
    }
    if (kind != QLatin1String("stats")) {
        return reject("unknown kind");
    }
    const msgpack::object* const statsValue = mp::member(frame, QLatin1StringView("stats"));
    if (statsValue == nullptr) {
        return reject("no stats");
    }
    return parseStats(*statsValue);
}

Socktrail::Socktrail(QObject* parent) : QObject(parent) {
    m_socket = new QLocalSocket(this);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(kReconnectIntervalMs);
    m_staleTimer = new QTimer(this);
    m_staleTimer->setSingleShot(true);
    m_staleTimer->setInterval(kStatsTimeoutMs);
    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(1);
    // A peer that never terminates a frame must not make readAll grow without
    // bound; the transport cap is one frame above the limit.
    m_socket->setReadBufferSize(mp::kMaxFrameBytes);

    connect(m_reconnectTimer, &QTimer::timeout, this, &Socktrail::connectSocket);
    connect(m_staleTimer, &QTimer::timeout, this, &Socktrail::onStatsTimeout);
    connect(m_flushTimer, &QTimer::timeout, this, &Socktrail::flushPendingStats);
    connect(m_socket, &QLocalSocket::connected, this, &Socktrail::onConnected);
    connect(m_socket, &QLocalSocket::readyRead, this, &Socktrail::onReadyRead);
    connect(m_socket, &QLocalSocket::disconnected, this, &Socktrail::onDisconnected);
    connect(m_socket, &QLocalSocket::errorOccurred, this, &Socktrail::onSocketError);
}

Socktrail::~Socktrail() {
    // No signal from a destructor; the timer and socket children go away too.
    m_running = false;
    m_reconnectTimer->stop();
    disconnect(m_socket, nullptr, this, nullptr);
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        m_socket->abort();
    }
}

void Socktrail::start(const QString& path) {
    m_path = path;
    m_running = true;
    m_decoder.reset();
    m_pendingStats = {};
    m_hasPendingStats = false;
    // Only the accumulator keeps its history; the observable state starts over
    // as unavailable and incomplete until a hello and a stats frame arrive.
    if (!m_clock.isValid()) {
        m_clock.start();
    }
    m_helloReceived = false;
    m_qualityNotes.clear();
    m_ipPackets = 0;
    m_ipBytes = 0;
    m_error.clear();
    m_available = false;
    m_incomplete = true;
    m_stale = true;
    m_reconnectTimer->stop();
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        m_socket->abort();
    }
    if (!m_path.isEmpty()) {
        connectSocket();
    }
    Q_EMIT updated();
}

void Socktrail::stop() {
    m_running = false;
    m_reconnectTimer->stop();
    m_staleTimer->stop();
    m_flushTimer->stop();
    m_decoder.reset();
    m_pendingStats = {};
    m_hasPendingStats = false;
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        m_socket->abort();
    }
    m_available = false;
    m_stale = true;
    m_incomplete = true;
    Q_EMIT updated();
}

void Socktrail::connectSocket() {
    if (!m_running || m_path.isEmpty()) {
        return;
    }
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        return; // a connect or a live connection is already in flight
    }
    m_reconnectTimer->stop();
    m_decoder.reset();
    m_socket->connectToServer(m_path);
}

void Socktrail::onConnected() {
    m_reconnectTimer->stop();
    m_decoder.reset();
    // A connection is not data: availability only follows a complete stats
    // frame, and the freshness clock starts now so a peer that sends nothing
    // usable still turns stale with a reason.
    m_helloReceived = false;
    m_available = false;
    m_stale = true;
    // A successful connect is not recovery; retain diagnostics until valid stats.
    m_staleTimer->start();
    Q_EMIT updated();
}

void Socktrail::onReadyRead() {
    m_decoder.feed(m_socket->readAll());
    if (m_decoder.failed()) {
        dropConnection(QString::fromLatin1(m_decoder.error()));
        return;
    }

    while (true) {
        mp::Frame frame = m_decoder.next();
        if (!frame.valid()) {
            break; // no complete frame left, or a transport-level failure
        }
        // A complete frame that cannot be parsed is a protocol failure: the
        // connection is dropped so the peer's bug is visible and the reconnect
        // starts from a known-good frame boundary.
        if (!handleFrame(frame.value())) {
            return;
        }
    }
    if (m_decoder.failed()) {
        dropConnection(QString::fromLatin1(m_decoder.error()));
    }
}

bool Socktrail::handleFrame(const msgpack::object& frame) {
    const SocktrailFrame decoded = decodeSocktrailFrame(frame);
    switch (decoded.status) {
    case SocktrailFrameStatus::Ignored:
        return true;
    case SocktrailFrameStatus::ProtocolError:
        dropConnection(decoded.error.isEmpty() ? QStringLiteral("bad frame") : decoded.error);
        return false;
    case SocktrailFrameStatus::Unsupported:
        dropConnection(QStringLiteral("unsupported version"));
        return false;
    case SocktrailFrameStatus::Hello:
        // A different `started` means the backend restarted with fresh counters
        // and the history in between is lost for good.
        if (m_backendStarted.isValid() && m_backendStarted != decoded.backendStarted) {
            m_restartPending = true;
            m_gapSeen = true;
            m_hasPendingStats = false;
            m_pendingStats = {};
            m_flushTimer->stop();
        }
        m_backendStarted = decoded.backendStarted;
        m_helloReceived = true;
        return true;
    case SocktrailFrameStatus::Ok:
        break;
    }

    if (!m_helloReceived) {
        dropConnection(QStringLiteral("stats before hello"));
        return false;
    }

    // One clock for the whole subscription, so every frame is on the same
    // monotonic timeline and Processes sees real intervals.
    const qint64 monotonicMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    if (monotonicMs <= m_lastAppliedMonoMs) {
        // Two stats frames inside one millisecond cannot be told apart by the
        // monotonic clock, and Processes would skip the earlier one (elapsed 0)
        // and lose its bytes. Keep the newest sample and apply it once a real
        // interval exists instead of inventing a timestamp.
        m_pendingStats = decoded;
        m_hasPendingStats = true;
        m_flushTimer->start();
        return true;
    }
    m_hasPendingStats = false;
    m_pendingStats = {};
    m_flushTimer->stop();
    return applyStats(decoded, monotonicMs);
}

bool Socktrail::applyStats(const SocktrailFrame& stats, qint64 monotonicMs) {
    // Processes keeps its own monotonic history, so this call must never see a
    // timestamp older than a previous one.
    if (!m_processes.update(stats.counters, stats.at, monotonicMs, m_restartPending)) {
        dropConnection(QStringLiteral("rejected clock sample"));
        return false;
    }
    m_restartPending = false;
    m_lastAppliedMonoMs = monotonicMs;
    m_qualityNotes = stats.qualityNotes;
    m_ipPackets = stats.ipPackets;
    m_ipBytes = stats.ipBytes;

    // Stats arrives: the data is live again and the freshness timer restarts.
    // No flow frame ever touches this timer.
    m_staleTimer->start();
    m_available = true;
    m_stale = false;
    m_error.clear();
    // A stats frame with quality problems describes a real gap in the process
    // table, so it cannot claim complete observation while the notes persist.
    // The loss is already history, so the marker is sticky for the rest of the
    // subscription.
    if (!stats.qualityNotes.isEmpty()) {
        m_gapSeen = true;
    }
    m_incomplete = m_gapSeen;
    Q_EMIT updated();
    return true;
}

void Socktrail::flushPendingStats() {
    if (!m_hasPendingStats) {
        return;
    }
    const qint64 monotonicMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    if (monotonicMs <= m_lastAppliedMonoMs) {
        // Still the same millisecond; wait for the next tick rather than
        // pretending time passed.
        m_flushTimer->start();
        return;
    }
    const SocktrailFrame stats = std::exchange(m_pendingStats, {});
    m_hasPendingStats = false;
    applyStats(stats, monotonicMs);
}

void Socktrail::onStatsTimeout() {
    m_stale = true;
    if (m_lastAppliedMonoMs >= 0) {
        m_gapSeen = true;
        m_incomplete = true;
    }
    if (m_error.isEmpty()) {
        // Explicit reason so "connected but no stats" is distinguishable from a
        // healthy subscription.
        m_error = QByteArrayLiteral("no stats");
    }
    Q_EMIT updated();
}

void Socktrail::onDisconnected() {
    m_staleTimer->stop();
    m_available = false;
    m_stale = true;
    m_incomplete = true;
    m_gapSeen = m_gapSeen || m_lastAppliedMonoMs >= 0;
    m_helloReceived = false;
    m_decoder.reset();
    m_pendingStats = {};
    m_hasPendingStats = false;
    if (m_error.isEmpty()) {
        m_error = QByteArrayLiteral("disconnected");
    }
    if (m_running) {
        startReconnect();
    }
    Q_EMIT updated();
}

void Socktrail::onSocketError() {
    dropConnection(m_socket->error() == QLocalSocket::PeerClosedError
                       ? QStringLiteral("collector connection closed")
                       : m_socket->errorString());
}

void Socktrail::dropConnection(const QString& reason) {
    setError(reason);
    m_staleTimer->stop();
    m_flushTimer->stop();
    m_decoder.reset();
    m_pendingStats = {};
    m_hasPendingStats = false;
    m_available = false;
    m_incomplete = true;
    m_stale = true;
    m_gapSeen = m_gapSeen || m_lastAppliedMonoMs >= 0;
    m_helloReceived = false;
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        const QSignalBlocker blocker(m_socket);
        m_socket->abort();
    }
    if (m_running) {
        startReconnect();
    }
    Q_EMIT updated();
}

void Socktrail::startReconnect() {
    if (!m_running || m_reconnectTimer->isActive()) {
        return;
    }
    m_reconnectTimer->start();
}

void Socktrail::setError(const QString& reason) {
    QByteArray bytes = reason.toUtf8().left(kMaxErrorLength);
    for (const char character : bytes) {
        // The error is meant for logs and the CLI, so keep it short ASCII.
        if (character < 0x20 || character >= 0x7f) {
            bytes = QByteArrayLiteral("socket error");
            break;
        }
    }
    m_error = std::move(bytes);
}

} // namespace ball
