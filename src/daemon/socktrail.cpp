// ball: a network traffic HUD for KDE Wayland sessions.
#include "daemon/socktrail.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QSet>
#include <QStringList>

#include <cmath>
#include <limits>
#include <utility>

namespace ball {
namespace {

constexpr int kReconnectIntervalMs = 1000;
constexpr int kMaxErrorLength = 120;
constexpr int kIntMax = std::numeric_limits<int>::max();

// Reads one non-negative integer exactly. The double view only decides sign,
// integrality and range; the value itself always comes from
// QJsonValue::toInteger, which keeps the JSON literal instead of the lossy
// double. A count past 2^63-1 is refused rather than rounded.
bool readCount(const QJsonObject& object, const char* key, quint64& value) {
    const QJsonValue json = object.value(QLatin1String(key));
    if (!json.isDouble()) {
        return false;
    }
    const double asDouble = json.toDouble();
    if (!std::isfinite(asDouble) || asDouble < 0.0 || std::trunc(asDouble) != asDouble) {
        return false;
    }
    const qint64 exact = json.toInteger(-1);
    if (exact < 0) {
        return false;
    }
    value = static_cast<quint64>(exact);
    return true;
}

bool readIOBytes(const QJsonObject& object, const char* key, quint64& rx, quint64& tx) {
    const QJsonValue json = object.value(QLatin1String(key));
    if (!json.isObject()) {
        return false;
    }
    const QJsonObject pair = json.toObject();
    return readCount(pair, "rx_bytes", rx) && readCount(pair, "tx_bytes", tx);
}

SocktrailFrame reject(const char* reason) {
    SocktrailFrame frame;
    frame.status = SocktrailFrameStatus::ProtocolError;
    frame.error = QLatin1String(reason);
    return frame;
}

// Parses "2026-10-13T14:00:00.123456789Z"; a value without a timezone is
// accepted by Qt but cannot be pinned to an instant, so the explicit suffix is
// required here.
bool parseTimestamp(const QJsonValue& value, QDateTime& out) {
    if (!value.isString()) {
        return false;
    }
    const QString text = value.toString();
    // Qt parses a timestamp without a zone as local time; an instant must name
    // its offset, so a 'Z' or a signed offset after the date is required.
    const qsizetype timeStart = text.indexOf(QLatin1Char('T'));
    const bool hasZone = text.endsWith(QLatin1Char('Z')) ||
                         (timeStart >= 0 && (text.indexOf(QLatin1Char('+'), timeStart) >= 0 ||
                                             text.indexOf(QLatin1Char('-'), timeStart) >= 0));
    if (!hasZone) {
        return false;
    }
    const QDateTime parsed = QDateTime::fromString(text, Qt::ISODate);
    if (!parsed.isValid()) {
        return false;
    }
    out = parsed;
    return true;
}

SocktrailFrame parseHello(const QJsonObject& object) {
    const QJsonValue helloValue = object.value(QLatin1String("hello"));
    if (!helloValue.isObject()) {
        return reject("no hello");
    }
    const QJsonObject hello = helloValue.toObject();
    quint64 version = 0;
    if (!readCount(hello, "version", version)) {
        return reject("bad version");
    }
    if (version != 1) {
        SocktrailFrame frame;
        frame.status = SocktrailFrameStatus::Unsupported;
        frame.error = QLatin1String("unsupported version");
        return frame;
    }
    SocktrailFrame frame;
    if (!parseTimestamp(hello.value(QLatin1String("started")), frame.backendStarted)) {
        return reject("bad started");
    }
    frame.status = SocktrailFrameStatus::Hello;
    return frame;
}

SocktrailFrame parseStats(const QJsonObject& object) {
    const QJsonValue statsValue = object.value(QLatin1String("stats"));
    if (!statsValue.isObject()) {
        return reject("no stats");
    }
    const QJsonObject stats = statsValue.toObject();

    SocktrailFrame frame;
    if (!parseTimestamp(stats.value(QLatin1String("at")), frame.at)) {
        return reject("bad at");
    }

    const QJsonValue ioValue = stats.value(QLatin1String("process_io"));
    if (!ioValue.isArray()) {
        return reject("bad process_io");
    }

    QSet<QPair<int, quint64>> identities;
    for (const QJsonValue entry : ioValue.toArray()) {
        if (!entry.isObject()) {
            return reject("bad process");
        }
        const QJsonObject process = entry.toObject();
        quint64 pid = 0;
        // PID -1 marks a process the backend could not attribute and PID 0 never
        // names a process; neither is a usable identity.
        if (!readCount(process, "pid", pid) || pid == 0 || pid > quint64(kIntMax)) {
            return reject("bad pid");
        }
        quint64 startNs = 0;
        if (!readCount(process, "start_ns", startNs)) {
            return reject("bad start_ns");
        }
        const QJsonValue nameValue = process.value(QLatin1String("name"));
        if (!nameValue.isUndefined() && !nameValue.isString()) {
            return reject("bad name");
        }
        quint64 rx = 0;
        quint64 tx = 0;
        if (!readCount(process, "rx_bytes", rx) || !readCount(process, "tx_bytes", tx)) {
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
        counter.name = nameValue.toString();
        counter.rxBytes = rx;
        counter.txBytes = tx;
        frame.counters.append(counter);
    }

    const QJsonValue probesValue = stats.value(QLatin1String("probes"));
    if (!probesValue.isObject()) {
        return reject("no probes");
    }
    const QJsonValue pidProbeValue = probesValue.toObject().value(QLatin1String("pid"));
    if (!pidProbeValue.isObject()) {
        return reject("no probes.pid");
    }
    const QJsonObject pidProbe = pidProbeValue.toObject();
    quint64 kernelLost = 0;
    quint64 dropped = 0;
    quint64 invalid = 0;
    if (!readCount(pidProbe, "kernel_lost", kernelLost) ||
        !readCount(pidProbe, "dropped", dropped) || !readCount(pidProbe, "invalid", invalid)) {
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
    if (!readIOBytes(stats, "io_unindexed", unindexedRx, unindexedTx) ||
        !readIOBytes(stats, "expired_pid_io", expiredRx, expiredTx)) {
        return reject("bad io totals");
    }
    quint64 expiredCount = 0;
    if (!readCount(stats, "expired_pid_count", expiredCount)) {
        return reject("bad expired_pid_count");
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

    frame.status = SocktrailFrameStatus::Ok;
    return frame;
}

} // namespace

SocktrailFrame decodeSocktrailFrame(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reject("bad json");
    }
    const QJsonObject object = document.object();
    const QJsonValue kindValue = object.value(QLatin1String("kind"));
    if (!kindValue.isString()) {
        return reject("no kind");
    }
    const QString kind = kindValue.toString();
    // Flow frames carry per-connection increments; process totals may only come
    // from stats, so a flow frame is accepted and dropped without any state.
    if (kind == QLatin1String("flow")) {
        SocktrailFrame frame;
        frame.status = SocktrailFrameStatus::Ignored;
        return frame;
    }
    if (kind == QLatin1String("hello")) {
        return parseHello(object);
    }
    if (kind != QLatin1String("stats")) {
        return reject("unknown kind");
    }
    return parseStats(object);
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
    // A peer that never terminates a line must not make readAll grow without
    // bound; the decoder sees at most one line above the cap and rejects it.
    m_socket->setReadBufferSize(kMaxLineBytes);

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
    m_buffer.clear();
    m_pendingCounters.clear();
    m_pendingQualityNotes.clear();
    m_hasPendingStats = false;
    // Only the accumulator keeps its history; the observable state starts over
    // as unavailable and incomplete until a hello and a stats frame arrive.
    if (!m_clock.isValid()) {
        m_clock.start();
    }
    m_helloReceived = false;
    m_qualityNotes.clear();
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
    m_buffer.clear();
    m_pendingCounters.clear();
    m_pendingQualityNotes.clear();
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
    m_buffer.clear();
    m_socket->connectToServer(m_path);
}

void Socktrail::onConnected() {
    m_reconnectTimer->stop();
    m_buffer.clear();
    // A connection is not data: availability only follows a complete stats
    // frame, and the freshness clock starts now so a peer that sends nothing
    // usable still turns stale with a reason.
    m_helloReceived = false;
    m_available = false;
    m_stale = true;
    m_error.clear();
    m_staleTimer->start();
    Q_EMIT updated();
}

void Socktrail::onReadyRead() {
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
        if (!handleLine(line)) {
            return; // a rejected frame closed the connection
        }
    }

    // A peer that never sends a newline must not grow the buffer without bound.
    if (m_buffer.size() >= kMaxLineBytes) {
        dropConnection(QStringLiteral("frame too large"));
    }
}

bool Socktrail::handleLine(const QByteArray& line) {
    const SocktrailFrame frame = decodeSocktrailFrame(line);
    switch (frame.status) {
    case SocktrailFrameStatus::Ignored:
        return true;
    case SocktrailFrameStatus::ProtocolError:
        dropConnection(frame.error.isEmpty() ? QStringLiteral("bad frame") : frame.error);
        return false;
    case SocktrailFrameStatus::Unsupported:
        dropConnection(QStringLiteral("unsupported version"));
        return false;
    case SocktrailFrameStatus::Hello:
        // A different `started` means the backend restarted with fresh counters
        // and the history in between is lost for good.
        if (m_backendStarted.isValid() && m_backendStarted != frame.backendStarted) {
            m_restartPending = true;
            m_gapSeen = true;
            m_hasPendingStats = false;
            m_pendingCounters.clear();
            m_pendingQualityNotes.clear();
            m_flushTimer->stop();
        }
        m_backendStarted = frame.backendStarted;
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
        m_pendingCounters = frame.counters;
        m_pendingAt = frame.at.toLocalTime();
        m_pendingQualityNotes = frame.qualityNotes;
        m_hasPendingStats = true;
        m_flushTimer->start();
        return true;
    }
    m_hasPendingStats = false;
    m_pendingCounters.clear();
    m_pendingQualityNotes.clear();
    m_flushTimer->stop();
    return applyStats(frame.counters, frame.at.toLocalTime(), monotonicMs, frame.qualityNotes);
}

bool Socktrail::applyStats(const QList<ProcessCounter>& counters, const QDateTime& at,
                           qint64 monotonicMs, const QStringList& qualityNotes) {
    // Processes keeps its own monotonic history, so this call must never see a
    // timestamp older than a previous one.
    if (!m_processes.update(counters, at, monotonicMs, m_restartPending)) {
        dropConnection(QStringLiteral("rejected clock sample"));
        return false;
    }
    m_restartPending = false;
    m_lastAppliedMonoMs = monotonicMs;
    m_qualityNotes = qualityNotes;

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
    if (!qualityNotes.isEmpty()) {
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
    QList<ProcessCounter> counters;
    QStringList qualityNotes;
    QDateTime at;
    std::swap(counters, m_pendingCounters);
    std::swap(qualityNotes, m_pendingQualityNotes);
    std::swap(at, m_pendingAt);
    m_hasPendingStats = false;
    applyStats(counters, at, monotonicMs, qualityNotes);
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
    m_available = false;
    m_stale = true;
    m_incomplete = true;
    m_gapSeen = m_gapSeen || m_lastAppliedMonoMs >= 0;
    m_helloReceived = false;
    m_buffer.clear();
    m_pendingCounters.clear();
    m_pendingQualityNotes.clear();
    m_hasPendingStats = false;
    if (m_error.isEmpty()) {
        m_error = QByteArrayLiteral("disconnected");
    }
    if (m_running) {
        startReconnect();
    }
    Q_EMIT updated();
}

void Socktrail::onSocketError() { dropConnection(m_socket->errorString()); }

void Socktrail::dropConnection(const QString& reason) {
    setError(reason);
    m_buffer.clear();
    m_pendingCounters.clear();
    m_pendingQualityNotes.clear();
    m_hasPendingStats = false;
    m_available = false;
    m_incomplete = true;
    m_stale = true;
    m_gapSeen = m_gapSeen || m_lastAppliedMonoMs >= 0;
    m_helloReceived = false;
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
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
