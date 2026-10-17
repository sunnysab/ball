// ball: a network traffic HUD for KDE Wayland sessions.
// Tests for the daemon Service, driven by real Unix sockets: the small
// fake_collector fixture (a separate binary, passed in by CMake) and a
// QLocalSocket client. No root, no network access.
#include "daemon/service.h"

#include "messagepack.h"
#include "snapshot.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using ball::InterfaceSnapshot;
using ball::ProcessSnapshot;
using ball::ProcessStatus;
using ball::Service;
using ball::ServiceOptions;
using ball::Snapshot;
using namespace Qt::StringLiterals;

#ifndef BALL_FAKE_COLLECTOR_BINARY
#error "BALL_FAKE_COLLECTOR_BINARY must point at the fake collector fixture"
#endif

namespace {

// The collector-run IP totals the fake collector fixture reports. The values are
// mirrored from tests/fake_collector.cpp rather than included from
// tests/mp_frames.h, because this suite does not link the ball_testkit target
// that provides that header. They have no relation to any socket I/O on this
// machine, and 2^53 + 1 is not exactly representable as a double, so a frame
// that rounded the byte count fails instead of coincidentally matching.
constexpr quint64 kFakeIpPackets = 2'450'000;
constexpr quint64 kFakeIpBytes = 9'007'199'254'740'993ULL;

QString fakeCollectorBinary() { return QStringLiteral(BALL_FAKE_COLLECTOR_BINARY); }

// The interface names of one snapshot, for the "sampling still works" checks.
QStringList interfaceNames(const Snapshot& snapshot) {
    QStringList names;
    for (const InterfaceSnapshot& row : snapshot.interfaces) {
        names.append(row.name);
    }
    return names;
}

// A client that reads whole MessagePack frames off the daemon socket. The
// service lives in this same thread, so the event loop must run while waiting;
// QTest::qWait does that, waitForReadyRead only pumps the client.
class FrameReader {
public:
    explicit FrameReader(QLocalSocket& socket) : m_socket(socket) {}

    // Reads until one complete frame is decoded, or `timeoutMs` elapses. The
    // decoded frame is copied out, because the next read invalidates it.
    bool waitForFrame(Snapshot& out, int timeoutMs) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < timeoutMs) {
            m_decoder.feed(m_socket.readAll());
            ball::mp::Frame frame = m_decoder.next();
            if (frame.valid()) {
                const ball::SnapshotParseResult result = ball::decodeSnapshot(frame.value());
                if (!result.snapshot.has_value()) {
                    return false;
                }
                out = *result.snapshot;
                return true;
            }
            QTest::qWait(10);
        }
        return false;
    }

    // Counts complete frames received over `durationMs`, keeping the event loop
    // running so the daemon keeps sampling.
    int drain(int durationMs) {
        QElapsedTimer clock;
        clock.start();
        int frames = 0;
        while (clock.elapsed() < durationMs) {
            m_decoder.feed(m_socket.readAll());
            while (m_decoder.next().valid()) {
                ++frames;
            }
            QTest::qWait(20);
        }
        return frames;
    }

    // Counts complete frames until the peer closes, reading from a raw
    // descriptor. Returns false when it stayed connected.
    static bool drainUntilClosed(int fd, int& frames, int timeoutMs);

private:
    QLocalSocket& m_socket;
    ball::mp::Decoder m_decoder;
};

bool processAlive(qint64 pid) { return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM; }

bool readPidFile(const QString& path, qint64& pid) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    bool ok = false;
    pid = file.readAll().trimmed().toLongLong(&ok);
    return ok && pid > 0;
}

// Creates a real but dead Unix socket file: bind, then close without unlinking.
bool createStaleSocket(const QString& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray encoded = QFile::encodeName(path);
    if (encoded.size() >= static_cast<qsizetype>(sizeof(address.sun_path))) {
        ::close(fd);
        return false;
    }
    ::strncpy(address.sun_path, encoded.constData(), sizeof(address.sun_path) - 1);
    ::unlink(encoded.constData());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return false;
    }
    ::close(fd); // the socket file survives, but nobody is listening
    return true;
}

// A raw client that never reads: unlike QLocalSocket it has no event-loop pump
// draining the kernel buffer, so the daemon really sees a slow consumer.
int connectRawClient(const QString& path) {
    const QByteArray encoded = QFile::encodeName(path);
    if (encoded.size() >= static_cast<qsizetype>(sizeof(sockaddr_un::sun_path))) {
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ::strncpy(address.sun_path, encoded.constData(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Drains `fd` until the daemon closes it (read returns 0) or errors, counting
// the frames that were decoded on the way. Returns false when it stayed
// connected.
bool FrameReader::drainUntilClosed(int fd, int& frames, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    QByteArray chunk(64LL * 1024, Qt::Uninitialized);
    ball::mp::Decoder decoder;
    while (clock.elapsed() < timeoutMs) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 100);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            continue;
        }
        const ssize_t bytes = ::read(fd, chunk.data(), static_cast<size_t>(chunk.size()));
        if (bytes == 0) {
            return true; // orderly shutdown
        }
        if (bytes < 0) {
            return errno == ECONNRESET || errno == EPIPE;
        }
        decoder.feed(chunk.first(bytes));
        while (decoder.next().valid()) {
            ++frames;
        }
    }
    return false;
}

} // namespace

class TestService : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void snapshotBeforeStartIsWellFormed();
    void initialAndPeriodicSnapshots();
    void servicePublishesConnections();
    void spawnRequestsStatsAndFlow();
    void byteCountersKeepPrecision();
    void collectorFailureStillServesInterfaces();
    void collectorRestartsArePaced();
    void collectorRecoversWhenBinaryAppears();
    void restartStartsNewMonitoringSession();
    void limitsConcurrentClients();
    void cleanup();
    void slowClientIsDroppedWithoutBlockingSampling();
    void refusesRegularFileAndSymlink();
    void reportsLockPermissionFailure();
    void refusesLiveSocket();
    void replacesOnlyStaleSocket();
    void stopCleansUpSocketAndCollector();
};

void TestService::snapshotBeforeStartIsWellFormed() {
    Service service;
    const Snapshot snapshot = service.snapshot();
    QCOMPARE(snapshot.version, 1);
    QVERIFY(!snapshot.interfacesAvailable);
    QVERIFY(snapshot.interfaces.isEmpty());
    QVERIFY(snapshot.processes.isEmpty());
    QVERIFY(!snapshot.processStatus.available);
    QVERIFY(snapshot.processStatus.stale);
    // Nothing has been collected yet, so the collector-run IP totals are zero
    // rather than guessed from the interface counters.
    QCOMPARE(snapshot.ipPackets, quint64(0));
    QCOMPARE(snapshot.ipBytes, quint64(0));
    QVERIFY(service.error().isEmpty());
}

void TestService::initialAndPeriodicSnapshots() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Service service;
    ServiceOptions options;
    options.socketPath = dir.filePath(u"gui.sock"_s);
    options.collectorBinary = fakeCollectorBinary();
    QVERIFY2(service.start(options), qPrintable(service.error()));

    // The public socket is 0660: the target group may read it, others may not.
    struct stat socketStat{};
    QVERIFY(::stat(QFile::encodeName(options.socketPath).constData(), &socketStat) == 0);
    QCOMPARE(static_cast<int>(static_cast<unsigned>(socketStat.st_mode) & 0777U), 0660);

    // Wait for the collector's first stats frame to reach the accumulator.
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().processes.size() == 1, 10000);
    QVERIFY(service.snapshot().interfacesAvailable);
    QVERIFY(!interfaceNames(service.snapshot()).isEmpty());

    QLocalSocket client;
    client.connectToServer(options.socketPath);
    QVERIFY(client.waitForConnected(2000));

    FrameReader reader(client);
    Snapshot initial;
    QVERIFY2(reader.waitForFrame(initial, 2000), "no initial snapshot");
    QCOMPARE(initial.version, 1);
    QCOMPARE(initial.processes.size(), 1);
    // Both instants name a real moment, so they survive the wire.
    QVERIFY(initial.at.isValid());
    QVERIFY(initial.started.isValid());
    // The collector-run IP totals reach the GUI frame unchanged and are not
    // derived from the process table: the fixture writes values of that order
    // only for this calibre.
    QCOMPARE(initial.ipPackets, kFakeIpPackets);
    QCOMPARE(initial.ipBytes, kFakeIpBytes);

    // The next snapshot follows within about one second, without reconnecting.
    Snapshot next;
    QVERIFY2(reader.waitForFrame(next, 3000), "no periodic snapshot");
    QCOMPARE(next.version, 1);
    QVERIFY(next.at > initial.at);

    service.stop();
}

void TestService::servicePublishesConnections() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Service service;
    ServiceOptions options;
    options.socketPath = dir.filePath(u"gui.sock"_s);
    options.collectorBinary = fakeCollectorBinary();
    QVERIFY2(service.start(options), qPrintable(service.error()));

    // The fake collector advertises flow in its hello and publishes two flow
    // frames per second, so the cache and its independent status become
    // available without affecting the process totals.
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().connectionStatus.available, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().connections.size() == 2, 10000);
    const Snapshot snapshot = service.snapshot();
    QCOMPARE(snapshot.connections.first().protocol, u"TCP"_s);
    QVERIFY(snapshot.processStatus.available);
    QVERIFY(!snapshot.connections.isEmpty());

    // A GUI sees the connection section in the same frame as the process data.
    QLocalSocket client;
    client.connectToServer(options.socketPath);
    QVERIFY(client.waitForConnected(2000));
    FrameReader reader(client);
    Snapshot decoded;
    QVERIFY2(reader.waitForFrame(decoded, 3000), "no snapshot with connections");
    QVERIFY(decoded.connectionStatus.available);
    QVERIFY(!decoded.connections.isEmpty());

    service.stop();
}

void TestService::spawnRequestsStatsAndFlow() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString argsFile = dir.filePath(u"collector.args"_s);
    qputenv("BALL_FAKE_ARGS_FILE", argsFile.toUtf8());

    Service service;
    ServiceOptions options;
    options.socketPath = dir.filePath(u"gui.sock"_s);
    options.collectorBinary = fakeCollectorBinary();
    QVERIFY2(service.start(options), qPrintable(service.error()));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(argsFile), 5000);

    QFile file(argsFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QStringList arguments =
        QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    // Both frame kinds are requested, while the packet-capture probes stay off.
    QVERIFY(arguments.contains(u"--socket-kinds"_s));
    QCOMPARE(arguments.at(arguments.indexOf(u"--socket-kinds"_s) + 1), u"stats,flow"_s);
    QVERIFY(arguments.contains(u"--openssl-probe=false"_s));
    QVERIFY(arguments.contains(u"--socket-sniff=false"_s));

    service.stop();
    qunsetenv("BALL_FAKE_ARGS_FILE");
}

void TestService::byteCountersKeepPrecision() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Service service;
    ServiceOptions options;
    options.socketPath = dir.filePath(u"gui.sock"_s);
    options.collectorBinary = fakeCollectorBinary();
    QVERIFY2(service.start(options), qPrintable(service.error()));

    // The second stats frame produces the first nonzero delta.
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const Snapshot snapshot = service.snapshot();
            return snapshot.processes.size() == 1 && snapshot.processes.first().today.rxBytes != 0;
        }(),
        10000);

    const ProcessSnapshot row = service.snapshot().processes.first();
    // start_ns is 2^53 + 1: only an exact integer can carry it.
    QCOMPARE(row.startNs, quint64(9007199254740993ULL));
    QCOMPARE(row.pid, 1);
    QVERIFY(std::isfinite(row.rxBps));
    QVERIFY(row.rxBps >= 0.0);

    // The collector-run IP totals keep every bit too, and are unrelated to the
    // process counters above.
    QCOMPARE(service.snapshot().ipPackets, kFakeIpPackets);
    QCOMPARE(service.snapshot().ipBytes, kFakeIpBytes);

    // The interface counters keep every bit too, and the index is a positive int.
    const InterfaceSnapshot interface = service.snapshot().interfaces.first();
    QVERIFY(interface.index > 0);
    QVERIFY(std::isfinite(interface.rxBps));

    service.stop();
}

void TestService::collectorFailureStillServesInterfaces() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Service service;
    ServiceOptions options;
    options.socketPath = dir.filePath(u"gui.sock"_s);
    options.collectorBinary = dir.filePath(u"does-not-exist"_s);
    QVERIFY2(service.start(options), qPrintable(service.error()));

    // The collector is gone but interface sampling keeps running. The failure
    // is delivered through the event loop, so spin until it is visible.
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().interfacesAvailable, 3000);
    QVERIFY(!interfaceNames(service.snapshot()).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(
        service.snapshot().processStatus.notes.contains(u"collector not running"_s), 5000);
    const ProcessStatus status = service.snapshot().processStatus;
    QVERIFY(!status.available);
    QVERIFY(status.stale);
    QVERIFY(status.incomplete);
    QVERIFY(!service.error().isEmpty()); // the failure stays visible

    // A client still gets a well-formed snapshot.
    QLocalSocket client;
    client.connectToServer(options.socketPath);
    QVERIFY(client.waitForConnected(2000));
    FrameReader reader(client);
    Snapshot snapshot;
    QVERIFY(reader.waitForFrame(snapshot, 2000));
    QVERIFY(snapshot.interfacesAvailable);
    QVERIFY(snapshot.processes.isEmpty());

    service.stop();
    // No restart storm after stop.
    QTest::qWait(1500);
    QVERIFY(!service.snapshot().processStatus.available);
}

void TestService::collectorRestartsArePaced() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pidFile = dir.filePath(u"collector.pid"_s);
    qputenv("BALL_FAKE_PID_FILE", pidFile.toUtf8());
    // The fake collector exits immediately, so the service must keep restarting
    // it at a fixed rhythm rather than in a busy loop.
    qputenv("BALL_FAKE_EXIT", "1");

    Service service;
    ServiceOptions options;
    options.socketPath = dir.filePath(u"gui.sock"_s);
    options.collectorBinary = fakeCollectorBinary();
    QVERIFY2(service.start(options), qPrintable(service.error()));
    QTest::qWait(3500);

    // Each line is one start. A busy loop would produce hundreds; the fixed
    // 1 s rhythm must stay well under ten in three seconds.
    int starts = 0;
    QFile file(pidFile);
    if (file.open(QIODevice::ReadOnly)) {
        starts = static_cast<int>(file.readAll().count('\n'));
    }
    QVERIFY2(starts >= 1, "the collector was never started");
    QVERIFY2(starts <= 6, qPrintable(u"collector started %1 times in 3.5s"_s.arg(starts)));

    service.stop();
    qunsetenv("BALL_FAKE_EXIT");
    qunsetenv("BALL_FAKE_PID_FILE");
}

void TestService::cleanup() {
    qunsetenv("BALL_FAKE_EXIT");
    qunsetenv("BALL_FAKE_PID_FILE");
    qunsetenv("BALL_FAKE_PROCESS_COUNT");
    qunsetenv("BALL_FAKE_ARGS_FILE");
}

void TestService::collectorRecoversWhenBinaryAppears() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Service service;
    ServiceOptions options{dir.filePath(u"gui.sock"_s), dir.filePath(u"collector"_s)};
    QVERIFY(service.start(options));
    QTRY_VERIFY_WITH_TIMEOUT(!service.error().isEmpty(), 5000);
    QVERIFY(QFile::link(fakeCollectorBinary(), options.collectorBinary));
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().processStatus.available, 10000);
    QVERIFY2(service.error().isEmpty(), qPrintable(service.error()));
    service.stop();
}

void TestService::restartStartsNewMonitoringSession() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Service service;
    ServiceOptions options{dir.filePath(u"gui.sock"_s), fakeCollectorBinary()};
    QVERIFY(service.start(options));
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            const Snapshot snapshot = service.snapshot();
            return !snapshot.processes.isEmpty() && snapshot.processes.first().session.rxBytes != 0;
        }(),
        10000);
    const QDateTime started = service.snapshot().started;
    service.stop();
    QVERIFY(service.start(options));
    QVERIFY(service.snapshot().started != started);
    QVERIFY(service.snapshot().processes.isEmpty());
    for (const InterfaceSnapshot& row : service.snapshot().interfaces) {
        QCOMPARE(row.session.rxBytes, quint64(0));
    }
    // The collector-run IP totals are the new collector's own numbers, not the
    // previous run's with a new sample added: the fixture reports the same value
    // every frame, so an accumulation bug would show as a multiple.
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().processes.size() == 1, 10000);
    QCOMPARE(service.snapshot().ipPackets, kFakeIpPackets);
    QCOMPARE(service.snapshot().ipBytes, kFakeIpBytes);
    service.stop();
}

void TestService::limitsConcurrentClients() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Service service;
    ServiceOptions options{dir.filePath(u"gui.sock"_s), QString()};
    QVERIFY(service.start(options));
    QList<QLocalSocket*> clients;
    for (int i = 0; i < 17; ++i) {
        auto* client = new QLocalSocket(&service);
        clients.append(client);
        client->connectToServer(options.socketPath);
        QVERIFY(client->waitForConnected(2000));
        QTest::qWait(10);
    }
    QTRY_COMPARE(clients.last()->state(), QLocalSocket::UnconnectedState);
    for (int i = 0; i < 16; ++i) {
        QCOMPARE(clients.at(i)->state(), QLocalSocket::ConnectedState);
    }
    service.stop();
}

void TestService::slowClientIsDroppedWithoutBlockingSampling() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A large table makes each snapshot far exceed a socket buffer, so the
    // backlog bound is reached within a couple of seconds.
    qputenv("BALL_FAKE_PROCESS_COUNT", "4000");

    Service service;
    ServiceOptions options;
    options.socketPath = dir.filePath(u"gui.sock"_s);
    options.collectorBinary = fakeCollectorBinary();
    QVERIFY2(service.start(options), qPrintable(service.error()));

    const int slow = connectRawClient(options.socketPath);
    QVERIFY(slow >= 0);

    QLocalSocket active;
    active.connectToServer(options.socketPath);
    QVERIFY(active.waitForConnected(3000));
    FrameReader reader(active);
    Snapshot first;
    QVERIFY(reader.waitForFrame(first, 5000));

    // The healthy client keeps receiving snapshots while the raw one is dropped
    // once its write backlog exceeds the bound.
    const int frames = reader.drain(6000);
    QVERIFY2(frames >= 2, qPrintable(u"healthy client got %1 snapshots"_s.arg(frames)));
    QVERIFY(active.state() == QLocalSocket::ConnectedState);

    int slowFrames = 0;
    const bool closed = FrameReader::drainUntilClosed(slow, slowFrames, 8000);
    ::close(slow);
    QVERIFY2(closed,
             qPrintable(u"slow client got %1 snapshots without being dropped"_s.arg(slowFrames)));
    QVERIFY2(slowFrames < 60, qPrintable(u"slow client buffered %1 snapshots"_s.arg(slowFrames)));

    service.stop();
    qunsetenv("BALL_FAKE_PROCESS_COUNT");
}

void TestService::refusesRegularFileAndSymlink() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"gui.sock"_s);

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("valuable data");
    file.close();

    Service service;
    ServiceOptions options;
    options.socketPath = path;
    QVERIFY(!service.start(options));
    QVERIFY(!service.error().isEmpty());
    // The regular file must survive untouched.
    QFile check(path);
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), QByteArrayLiteral("valuable data"));
    check.close();
    QVERIFY(QFile::remove(path));

    // A symlink is never followed, so its target cannot be removed.
    const QString target = dir.filePath(u"target"_s);
    QFile targetFile(target);
    QVERIFY(targetFile.open(QIODevice::WriteOnly));
    targetFile.write("keep me");
    targetFile.close();
    QVERIFY(QFile::link(target, path));
    Service symlinkService;
    QVERIFY(!symlinkService.start(options));
    QVERIFY(QFileInfo(path).isSymLink());
    QFile targetCheck(target);
    QVERIFY(targetCheck.open(QIODevice::ReadOnly));
    QCOMPARE(targetCheck.readAll(), QByteArrayLiteral("keep me"));
}

void TestService::reportsLockPermissionFailure() {
    if (::geteuid() == 0) {
        QSKIP("root bypasses directory write permissions");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(
        QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    Service service;
    ServiceOptions options{directory.filePath(u"gui.sock"_s), QString()};
    QVERIFY(!service.start(options));
    QVERIFY2(service.error().contains(u"permission"_s), qPrintable(service.error()));
    QVERIFY(!service.error().contains(u"another instance"_s));
    QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner |
                                                        QFileDevice::WriteOwner |
                                                        QFileDevice::ExeOwner));
}

void TestService::refusesLiveSocket() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"gui.sock"_s);

    QLocalServer live;
    QVERIFY(live.listen(path));

    Service service;
    ServiceOptions options;
    options.socketPath = path;
    QVERIFY(!service.start(options)); // never steals another instance's socket
    QVERIFY(!service.error().isEmpty());
    QVERIFY(std::filesystem::is_socket(QFile::encodeName(path).constData()));
    QVERIFY(live.isListening());

    // The live server still accepts connections, so the path was not broken.
    QLocalSocket probe;
    probe.connectToServer(path);
    QVERIFY(probe.waitForConnected(2000));
    live.close();
}

void TestService::replacesOnlyStaleSocket() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"gui.sock"_s);
    QVERIFY(createStaleSocket(path));
    QVERIFY(std::filesystem::is_socket(QFile::encodeName(path).constData()));

    Service service;
    ServiceOptions options;
    options.socketPath = path;
    QVERIFY2(service.start(options), qPrintable(service.error()));

    QLocalSocket client;
    client.connectToServer(path);
    QVERIFY2(client.waitForConnected(2000), "the stale socket was replaced by a live one");
    client.abort();
    service.stop();
    QVERIFY(!QFileInfo::exists(path));
}

void TestService::stopCleansUpSocketAndCollector() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pidFile = dir.filePath(u"collector.pid"_s);
    qputenv("BALL_FAKE_PID_FILE", pidFile.toUtf8());

    Service service;
    ServiceOptions options;
    options.socketPath = dir.filePath(u"gui.sock"_s);
    options.collectorBinary = fakeCollectorBinary();
    QVERIFY2(service.start(options), qPrintable(service.error()));

    qint64 collectorPid = 0;
    QTRY_VERIFY_WITH_TIMEOUT(readPidFile(pidFile, collectorPid), 5000);
    QVERIFY(processAlive(collectorPid));

    // An unrelated process must never be touched by stop().
    QProcess unrelated;
    unrelated.start(u"sleep"_s, {u"30"_s});
    QVERIFY(unrelated.waitForStarted(2000));

    service.stop();
    QVERIFY2(!QFileInfo::exists(options.socketPath), "the public socket must be removed");
    QVERIFY2(!processAlive(collectorPid), "the collector child must not be left behind");
    QCOMPARE(unrelated.state(), QProcess::Running);
    unrelated.kill();
    QVERIFY(unrelated.waitForFinished(2000));

    qunsetenv("BALL_FAKE_PID_FILE");
}

QTEST_GUILESS_MAIN(TestService)
#include "service.moc"
