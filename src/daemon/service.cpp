// ball: a network traffic HUD for KDE Wayland sessions.
#include "daemon/service.h"

#include "daemon/interfaces.h"
#include "daemon/processes.h"
#include "daemon/socktrail.h"
#include "daemon/traffic.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <csignal>
#include <sys/stat.h>
#include <utility>

namespace ball {
namespace {

// Bounds diagnostics and removes control bytes before exposing them in a
// snapshot; raw collector output is never forwarded.
QString asciiBounded(const QString& text, int maxLength) {
    QString out;
    out.reserve(qMin(text.size(), maxLength));
    for (const QChar character : text) {
        if (out.size() >= maxLength) {
            break;
        }
        out.append(character.unicode() >= 0x20 && character.unicode() < 0x7f ? character
                                                                             : QLatin1Char(' '));
    }
    return out.trimmed();
}

// QString/QFileInfo have no portable "is a Unix socket" predicate, so stat()
// is used directly. A symlink is deliberately not followed (lstat), because the
// path must never be resolved outside the socket it names.
bool isUnixSocketFile(const QString& path) {
    struct stat info{};
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0 && S_ISSOCK(info.st_mode);
}

} // namespace

Service::Service(QObject* parent)
    : QObject(parent), m_interfaces(std::make_unique<Interfaces>()),
      m_traffic(std::make_unique<Traffic>()), m_socktrail(new Socktrail(this)),
      m_tickTimer(new QTimer(this)), m_restartTimer(new QTimer(this)) {
    m_tickTimer->setInterval(kPollIntervalMs);
    m_restartTimer->setSingleShot(true);
    m_restartTimer->setInterval(kCollectorRestartMs);
    connect(m_tickTimer, &QTimer::timeout, this, &Service::onTick);
    connect(m_restartTimer, &QTimer::timeout, this, &Service::startCollector);
    connect(m_socktrail, &Socktrail::updated, this, &Service::updated);
}

Service::~Service() { shutdown(); }

bool Service::start(const ServiceOptions& options) {
    if (m_running) {
        m_error = QStringLiteral("already started");
        return false;
    }
    m_options = options;
    // Every early return after the lock is taken goes through this, so a failed
    // start never leaves the lock file held for the rest of the object's life.
    const auto fail = [this](const QString& reason) {
        m_error = reason;
        if (m_lock) {
            m_lock->unlock();
            m_lock.reset();
        }
        return false;
    };
    if (m_options.socketPath.isEmpty()) {
        return fail(QStringLiteral("no socket path configured"));
    }
    if (!m_options.collectorSocketPath.isEmpty() && !m_options.collectorBinary.isEmpty()) {
        // Spawning a second collector next to the external one would fight over
        // /tmp/socktrail.lock and the capture interfaces.
        return fail(QStringLiteral("collector binary and an external collector socket cannot "
                                   "be combined"));
    }

    // The lock is taken before the socket is touched: it is what makes a
    // crashed instance detectable and keeps two daemons from fighting over the
    // same public socket.
    m_lock = std::make_unique<QLockFile>(m_options.socketPath + QStringLiteral(".lock"));
    if (!m_lock->tryLock(0)) {
        const QLockFile::LockError error = m_lock->error();
        m_lock.reset();
        if (error == QLockFile::PermissionError) {
            m_error = QStringLiteral(
                "permission denied creating lock; check runtime directory and daemon user");
        } else if (error == QLockFile::LockFailedError) {
            m_error = QStringLiteral("another instance holds the lock");
        } else {
            m_error = QStringLiteral("cannot create lock file; check runtime directory");
        }
        return false;
    }

    const QFileInfo socketInfo(m_options.socketPath);
    if (socketInfo.isSymLink()) {
        // Never follow a symlink here: the path may point somewhere we must not
        // unlink. Refusing protects data outside the socket directory.
        return fail(QStringLiteral("socket path is a symlink"));
    }
    if (socketInfo.exists()) {
        if (socketInfo.isDir() || !isUnixSocketFile(m_options.socketPath)) {
            return fail(QStringLiteral("socket path exists and is not a socket"));
        }
        // A socket file with nobody behind it is a leftover from a crash. Only
        // a failing probe proves the file is stale; a live peer is refused.
        QLocalSocket probe;
        probe.connectToServer(m_options.socketPath);
        if (probe.waitForConnected(200)) {
            return fail(QStringLiteral("another instance is listening"));
        }
        const auto probeError = probe.error();
        probe.abort();
        if (probeError != QLocalSocket::ConnectionRefusedError &&
            probeError != QLocalSocket::ServerNotFoundError) {
            return fail(QStringLiteral("cannot confirm stale socket"));
        }
        if (!QFile::remove(m_options.socketPath)) {
            return fail(QStringLiteral("cannot remove stale socket"));
        }
    }

    m_server = new QLocalServer(this);
    // The deployment layer owns the directory; the socket itself must stay
    // readable and writable by the user's group, never by everyone.
    m_server->setSocketOptions(QLocalServer::UserAccessOption | QLocalServer::GroupAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &Service::onNewConnection);
    if (!m_server->listen(m_options.socketPath)) {
        const QString reason = asciiBounded(m_server->errorString(), kMaxErrorLength);
        m_server->deleteLater();
        m_server = nullptr;
        return fail(reason);
    }
    if (!QFile::setPermissions(m_options.socketPath,
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ReadGroup | QFileDevice::WriteGroup)) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
        return fail(QStringLiteral("cannot restrict socket permissions"));
    }

    if (m_started.isValid()) {
        m_traffic = std::make_unique<Traffic>();
        disconnect(m_socktrail, nullptr, this, nullptr);
        m_socktrail->deleteLater();
        m_socktrail = new Socktrail(this);
        connect(m_socktrail, &Socktrail::updated, this, &Service::updated);
    }
    m_running = true;
    m_error.clear();
    m_collectorError.clear();
    m_collectorStderr.clear();
    m_interfaceError.clear();
    m_interfacesAvailable = false;
    m_started = QDateTime::currentDateTime();
    m_clock.start();
    pollInterfaces();
    m_tickTimer->start();
    startCollector();
    Q_EMIT updated();
    return true;
}

void Service::stop() { shutdown(); }

void Service::shutdown() {
    if (!m_running && m_clients.isEmpty() && m_collector == nullptr) {
        return;
    }
    m_running = false;
    m_tickTimer->stop();
    m_restartTimer->stop();

    // Clients are dropped, not drained: a slow GUI must not delay shutdown.
    const QList<QLocalSocket*> clients = m_clients;
    m_clients.clear();
    for (QLocalSocket* socket : clients) {
        socket->abort();
        socket->deleteLater();
    }

    m_socktrail->stop();
    stopCollector();
    if (m_server != nullptr) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
        // Qt removes the socket it created; remove anything left so a restart
        // is not blocked by our own stale file.
        if (isUnixSocketFile(m_options.socketPath)) {
            QFile::remove(m_options.socketPath);
        }
    }
    if (m_lock) {
        m_lock->unlock();
        m_lock.reset();
    }
    // The private raw-socket directory and its 0600 socket vanish with it.
    m_rawDir.reset();
    m_rawSocketPath.clear();
    Q_EMIT updated();
}

void Service::onNewConnection() {
    if (m_server == nullptr) {
        return;
    }
    while (m_server->hasPendingConnections()) {
        QLocalSocket* socket = m_server->nextPendingConnection();
        if (socket == nullptr) {
            break;
        }
        if (m_clients.size() >= kMaxClients) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        // The GUI never sends anything; cap and drain the read side anyway so a
        // misbehaving client cannot grow our buffer.
        socket->setReadBufferSize(64LL * 1024);
        connect(socket, &QLocalSocket::readyRead, socket, [socket] { socket->readAll(); });
        connect(socket, &QLocalSocket::disconnected, this,
                [this, socket] { removeClient(socket); });
        connect(socket, &QLocalSocket::errorOccurred, this,
                [this, socket](QLocalSocket::LocalSocketError) { removeClient(socket); });
        m_clients.append(socket);
        // A connection is answered immediately with the current full snapshot.
        bool ok = false;
        const QByteArray frame = snapshotFrame(ok);
        if (!ok || !sendSnapshot(socket, frame)) {
            removeClient(socket);
        }
    }
    Q_EMIT updated();
}

void Service::onTick() {
    pollInterfaces();
    broadcastSnapshot();
    Q_EMIT updated();
}

void Service::pollInterfaces() {
    if (!m_running) {
        return;
    }
    const std::optional<QList<InterfaceInfo>> dump = m_interfaces->poll(kPollTimeoutMs);
    if (!dump.has_value()) {
        // A failed dump leaves every total and baseline untouched; it must
        // never be read as an empty interface list.
        m_interfacesAvailable = false;
        m_interfaceError = QStringLiteral("interface dump failed");
        return;
    }
    const qint64 monotonicMs = m_clock.isValid() ? m_clock.elapsed() : 0;
    if (!m_traffic->update(*dump, QDateTime::currentDateTime(), monotonicMs)) {
        m_interfacesAvailable = false;
        m_interfaceError = QStringLiteral("invalid interface sample");
        return;
    }
    m_interfacesAvailable = true;
    m_interfaceError.clear();
}

void Service::broadcastSnapshot() {
    if (m_clients.isEmpty()) {
        return;
    }
    bool ok = false;
    const QByteArray frame = snapshotFrame(ok);
    if (!ok) {
        // An unencodable snapshot is a daemon bug; serving the same state as a
        // zero frame would be worse than skipping this tick.
        return;
    }
    const QList<QLocalSocket*> clients = m_clients;
    for (QLocalSocket* socket : clients) {
        if (!sendSnapshot(socket, frame)) {
            removeClient(socket);
        }
    }
}

QByteArray Service::snapshotFrame(bool& ok) const {
    Snapshot current = snapshot();
    QByteArray frame = encodeSnapshot(current, ok);
    if (ok || current.connections.isEmpty()) {
        return frame;
    }
    // The full connection table did not fit one frame. Only the connection list
    // of this local copy is reduced, so the interface and process sections
    // (which the frame limit cannot have caused) stay intact and the real cache
    // keeps every row for the next attempt. The reduction is recorded as
    // truncation and kept in the status notes, never silently dropped.
    const QString note =
        QStringLiteral("connections reduced to fit one frame; not the full history, retrying");
    if (!current.connectionStatus.notes.contains(note)) {
        current.connectionStatus.notes.append(note);
    }
    current.connectionStatus.truncated = true;
    while (!current.connections.isEmpty()) {
        current.connections.remove(current.connections.size() / 2,
                                   current.connections.size() - current.connections.size() / 2);
        frame = encodeSnapshot(current, ok);
        if (ok) {
            return frame;
        }
    }
    // The non-connection sections alone are still too large; the caller skips
    // this tick rather than sending a frame we could not describe.
    return frame;
}

bool Service::sendSnapshot(QLocalSocket* socket, const QByteArray& frame) {
    if (!m_clients.contains(socket) || socket->state() != QLocalSocket::ConnectedState) {
        return false;
    }
    // Bounded backlog: once a client is more than one snapshot behind, it is
    // disconnected instead of being allowed to grow the daemon's memory.
    const qint64 limit = qMax(kMaxBacklogBytes, 2 * static_cast<qint64>(frame.size()));
    if (socket->bytesToWrite() > limit) {
        return false;
    }
    if (socket->write(frame) != frame.size()) {
        return false;
    }
    socket->flush();
    return true;
}

void Service::removeClient(QLocalSocket* socket) {
    if (!m_clients.removeOne(socket)) {
        return; // already gone; the disconnected signal fires twice
    }
    socket->abort();
    socket->deleteLater();
}

Snapshot Service::snapshot() const {
    Snapshot snapshot;
    snapshot.version = 1;
    snapshot.started = m_started;
    snapshot.at = QDateTime::currentDateTime();
    snapshot.interfacesAvailable = m_interfacesAvailable;
    snapshot.interfaceError = m_interfaceError;

    const bool collectorUp = collectorRunning();
    QStringList notes = m_socktrail->qualityNotes();
    if (!collectorUp) {
        notes.prepend(QStringLiteral("collector not running"));
    }
    snapshot.processStatus.available = m_socktrail->available();
    snapshot.processStatus.stale = !collectorUp || m_socktrail->stale();
    snapshot.processStatus.incomplete = m_socktrail->incomplete();
    snapshot.processStatus.error = processError();
    snapshot.processStatus.notes = notes;
    snapshot.processStatus.info = m_socktrail->infoNotes();
    snapshot.ipPackets = m_socktrail->ipPackets();
    snapshot.ipBytes = m_socktrail->ipBytes();

    // The connection section is independent of the interface and process
    // status: a collector that publishes no flow frames leaves it unavailable
    // without affecting anything else, and says so explicitly.
    snapshot.connectionStatus.available = m_socktrail->connectionsAvailable();
    snapshot.connectionStatus.generation = m_socktrail->connectionGeneration();
    snapshot.connectionStatus.stale = !collectorUp || m_socktrail->stale();
    snapshot.connectionStatus.truncated = m_socktrail->connections().truncated();
    if (!collectorUp) {
        snapshot.connectionStatus.notes.append(QStringLiteral("collector not running"));
    } else if (!m_socktrail->connectionsAvailable()) {
        snapshot.connectionStatus.notes.append(
            QStringLiteral("collector does not publish flow frames"));
    }
    for (const ConnectionSnapshot& row : m_socktrail->connections().rows()) {
        snapshot.connections.append(row);
    }

    for (const InterfaceTraffic& row : m_traffic->rows()) {
        InterfaceSnapshot interface;
        interface.index = row.index;
        interface.name = row.name;
        interface.kind = row.kind;
        interface.up = row.up;
        interface.today = ByteCounters{row.today.rxBytes, row.today.txBytes};
        interface.session = ByteCounters{row.session.rxBytes, row.session.txBytes};
        interface.rxBps = row.rate.rxBytesPerSecond;
        interface.txBps = row.rate.txBytesPerSecond;
        snapshot.interfaces.append(interface);
    }
    for (const ProcessTraffic& row : m_socktrail->processes().rows()) {
        ProcessSnapshot process;
        process.pid = row.pid;
        process.startNs = row.startNs;
        process.name = row.name;
        process.present = row.present;
        process.today = ByteCounters{row.today.rxBytes, row.today.txBytes};
        process.session = ByteCounters{row.session.rxBytes, row.session.txBytes};
        process.rxBps = row.rate.rxBytesPerSecond;
        process.txBps = row.rate.txBytesPerSecond;
        snapshot.processes.append(process);
    }
    return snapshot;
}

QString Service::error() const {
    if (!m_error.isEmpty()) {
        return m_error; // start() never succeeded, so this is the real problem
    }
    return processError();
}

QString Service::processError() const {
    QStringList parts;
    if (!m_collectorError.isEmpty()) {
        parts.append(m_collectorError);
    }
    const QByteArray socktrailError = m_socktrail->error();
    if (!socktrailError.isEmpty()) {
        parts.append(QString::fromLatin1(socktrailError));
    }
    if (m_collectorStderr.isEmpty()) {
        return asciiBounded(parts.join(QStringLiteral("; ")), kMaxErrorLength);
    }
    parts.append(m_collectorStderr);
    return asciiBounded(parts.join(QStringLiteral("; ")), kMaxErrorLength);
}

bool Service::collectorRunning() const {
    if (usesExternalCollector()) {
        // No child to poll: the subscriber's own state is what says whether the
        // external unit is serving, and a missing socket is exactly how it
        // fails, so the reason comes from the stream rather than from here.
        return true;
    }
    // A pending restart means the collector is down right now, even if the
    // QProcess object for the next attempt already exists.
    return !m_restartTimer->isActive() && m_collector != nullptr &&
           m_collector->state() != QProcess::NotRunning;
}

void Service::startCollector() {
    if (!m_running || m_collector != nullptr) {
        return;
    }
    if (usesExternalCollector()) {
        // Another unit owns the collector process: only subscribe. Its socket
        // may not exist yet, and Socktrail retries until it appears.
        m_collectorError.clear();
        m_collectorStderr.clear();
        m_socktrail->start(m_options.collectorSocketPath);
        return;
    }
    if (m_options.collectorBinary.isEmpty()) {
        // Interfaces are still served; the snapshot says why processes are not.
        m_collectorError = QStringLiteral("no collector configured");
        return;
    }
    if (!m_rawDir) {
        auto dir =
            std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/ball-raw-XXXXXX"));
        if (dir->isValid() &&
            QFile::setPermissions(dir->path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                   QFileDevice::ExeOwner)) {
            m_rawSocketPath = dir->path() + QStringLiteral("/collector.sock");
            m_rawDir = std::move(dir);
        }
    }
    if (!m_rawDir || m_rawSocketPath.isEmpty()) {
        m_collectorError = QStringLiteral("no private socket directory");
        return;
    }
    // A collector that crashed left its socket file behind; it lives in our
    // private directory, so removing it is safe and lets the next child bind.
    QFile::remove(m_rawSocketPath);

    m_collector = new QProcess(this);
    connect(m_collector, &QProcess::readyReadStandardOutput, m_collector,
            [collector = m_collector] { collector->readAllStandardOutput(); });
    connect(m_collector, &QProcess::readyReadStandardError, this, &Service::drainCollectorStderr);
    connect(m_collector, &QProcess::started, this, [this] {
        m_collectorError.clear();
        m_collectorStderr.clear();
        Q_EMIT updated();
    });
    connect(m_collector, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_collector != nullptr) {
            m_collectorError = asciiBounded(m_collector->errorString(), kMaxErrorLength);
            onCollectorFinished();
        }
    });
    connect(m_collector, &QProcess::finished, this,
            [this](int, QProcess::ExitStatus) { onCollectorFinished(); });

    // The entry point blocks SIGINT/SIGTERM/SIGHUP before Qt starts, so the
    // child would inherit a blocked mask and ignore `terminate()`. Only
    // async-signal-safe calls run here: resetting the mask in the child.
    m_collector->setChildProcessModifier([] {
        sigset_t empty{};
        ::sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);
    });
    // No shell and no duration: argument injection stays impossible and the
    // child is only stopped by us, so a dead observer cannot end collection.
    m_collector->start(
        m_options.collectorBinary,
        {QStringLiteral("--config"), QStringLiteral("none"), QStringLiteral("--output"),
         QStringLiteral("msgpack"), QStringLiteral("--socket"), m_rawSocketPath,
         QStringLiteral("--socket-mode"), QStringLiteral("0600"), QStringLiteral("--refresh"),
         QStringLiteral("1s"), QStringLiteral("--openssl-probe=false"),
         QStringLiteral("--socket-sniff=false"),
         // The connection page needs the per-connection rows, and the process
         // totals come from the same stream; both kinds are requested.
         QStringLiteral("--socket-kinds"), QStringLiteral("stats,flow")});
    // Subscribing before the socket exists is fine: Socktrail retries on its own
    // timer and connects as soon as the collector is listening.
    m_socktrail->start(m_rawSocketPath);
}

void Service::stopCollector() {
    if (usesExternalCollector() || m_collector == nullptr) {
        // An external collector is not ours to stop; the unit owns its life.
        return;
    }
    QProcess* collector = std::exchange(m_collector, nullptr);
    disconnect(collector, nullptr, this, nullptr);
    if (collector->state() != QProcess::NotRunning) {
        collector->terminate();
        if (!collector->waitForFinished(kTerminateWaitMs)) {
            collector->kill();
            collector->waitForFinished(kTerminateWaitMs);
        }
    }
    collector->closeReadChannel(QProcess::StandardOutput);
    collector->closeReadChannel(QProcess::StandardError);
    collector->deleteLater();
}

void Service::onCollectorFinished() {
    if (m_collector == nullptr) {
        return;
    }
    if (!m_collectorError.isEmpty()) {
        // Same failure again: keep the message, do not duplicate it.
    } else if (m_collector->exitStatus() == QProcess::CrashExit || m_collector->exitCode() != 0) {
        m_collectorError = QStringLiteral("collector exited");
    }
    QProcess* collector = std::exchange(m_collector, nullptr);
    disconnect(collector, nullptr, this, nullptr);
    collector->deleteLater();
    if (m_running) {
        // A bounded, fixed rhythm instead of a busy loop; Socktrail keeps
        // retrying the socket in between.
        m_restartTimer->start();
    }
    Q_EMIT updated();
}

void Service::drainCollectorStderr() {
    if (m_collector == nullptr) {
        return;
    }
    // Only a short sanitized tail is kept: the full stderr may contain flow or
    // domain data that must not reach the GUI.
    const QString text =
        asciiBounded(QString::fromUtf8(m_collector->readAllStandardError()), kStderrTailLength);
    if (!text.isEmpty()) {
        m_collectorStderr = text;
    }
}

} // namespace ball
