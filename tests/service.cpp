// ball: a network traffic HUD for KDE Wayland sessions.
// Tests for the daemon Service, driven by real Unix sockets: the small
// fake_collector fixture (a separate binary, passed in by CMake) and a
// QLocalSocket client. No root, no network access.
#include "daemon/service.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
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

using ball::Service;
using ball::ServiceOptions;
using namespace Qt::StringLiterals;

#ifndef BALL_FAKE_COLLECTOR_BINARY
#error "BALL_FAKE_COLLECTOR_BINARY must point at the fake collector fixture"
#endif

namespace {

QString fakeCollectorBinary() { return QStringLiteral(BALL_FAKE_COLLECTOR_BINARY); }

// The service lives in this same thread, so the event loop must run while
// waiting; QTest::qWait does that, waitForReadyRead only pumps the client.
bool waitForLine(QLocalSocket& socket, QByteArray& buffer, QByteArray& line, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        buffer += socket.readAll();
        const qsizetype newline = buffer.indexOf('\n');
        if (newline >= 0) {
            line = buffer.left(newline);
            buffer.remove(0, newline + 1);
            return true;
        }
        QTest::qWait(10);
    }
    return false;
}

// Counts complete lines received over `durationMs`, keeping the event loop
// running so the daemon keeps sampling.
int drainLines(QLocalSocket& socket, QByteArray& buffer, int durationMs) {
    QElapsedTimer clock;
    clock.start();
    int lines = 0;
    while (clock.elapsed() < durationMs) {
        buffer += socket.readAll();
        qsizetype newline = -1;
        while ((newline = buffer.indexOf('\n')) >= 0) {
            buffer.remove(0, newline + 1);
            ++lines;
        }
        QTest::qWait(20);
    }
    return lines;
}

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

QJsonObject parseLine(const QByteArray& line) { return QJsonDocument::fromJson(line).object(); }

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
// newline-terminated snapshots. Returns false when it stayed connected.
bool drainUntilClosed(int fd, int& lines, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    QByteArray chunk(64LL * 1024, Qt::Uninitialized);
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
        for (ssize_t i = 0; i < bytes; ++i) {
            if (chunk.at(i) == '\n') {
                ++lines;
            }
        }
    }
    return false;
}

QJsonArray interfaceArray(const QJsonObject& snapshot) {
    return snapshot.value(u"interfaces"_s).toArray();
}

} // namespace

class TestService : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void snapshotBeforeStartIsWellFormed();
    void initialAndPeriodicSnapshots();
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
    const QJsonObject snapshot = service.snapshot();
    QCOMPARE(snapshot.value(u"version"_s).toInt(), 1);
    QCOMPARE(snapshot.value(u"kind"_s).toString(), u"snapshot"_s);
    QVERIFY(snapshot.value(u"interfaces_available"_s).isBool());
    QVERIFY(snapshot.value(u"interfaces"_s).isArray());
    QVERIFY(snapshot.value(u"processes"_s).isArray());
    QVERIFY(snapshot.value(u"process_status"_s).isObject());
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
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().value(u"processes"_s).toArray().size() == 1, 10000);
    QVERIFY(service.snapshot().value(u"interfaces_available"_s).toBool());
    QVERIFY(!interfaceArray(service.snapshot()).isEmpty());

    QLocalSocket client;
    client.connectToServer(options.socketPath);
    QVERIFY(client.waitForConnected(2000));

    QByteArray buffer;
    QByteArray line;
    QVERIFY2(waitForLine(client, buffer, line, 2000), "no initial snapshot");
    const QJsonObject initial = parseLine(line);
    QCOMPARE(initial.value(u"kind"_s).toString(), u"snapshot"_s);
    QCOMPARE(initial.value(u"processes"_s).toArray().size(), 1);
    // `at` and `started` carry a timezone offset / Z.
    const QString at = initial.value(u"at"_s).toString();
    const QString started = initial.value(u"started"_s).toString();
    QVERIFY(at.contains(QLatin1Char('+')) || at.endsWith(QLatin1Char('Z')));
    QVERIFY(started.contains(QLatin1Char('+')) || started.endsWith(QLatin1Char('Z')));

    // The next snapshot follows within about one second, without reconnecting.
    QVERIFY2(waitForLine(client, buffer, line, 3000), "no periodic snapshot");
    QCOMPARE(parseLine(line).value(u"kind"_s).toString(), u"snapshot"_s);

    service.stop();
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
            const QJsonArray rows = service.snapshot().value(u"processes"_s).toArray();
            if (rows.size() != 1) {
                return false;
            }
            const QJsonObject today = rows.first().toObject().value(u"today"_s).toObject();
            return today.value(u"rx_bytes"_s).toString() != u"0"_s;
        }(),
        10000);

    const QJsonObject row = service.snapshot().value(u"processes"_s).toArray().first().toObject();
    // start_ns is 2^53 + 1: only the exact decimal string can carry it.
    QCOMPARE(row.value(u"start_ns"_s).toString(), u"9007199254740993"_s);
    QCOMPARE(row.value(u"pid"_s).toInt(), 1);
    QVERIFY(row.value(u"today"_s).toObject().value(u"rx_bytes"_s).isString());
    QVERIFY(row.value(u"session"_s).toObject().value(u"tx_bytes"_s).isString());
    const QJsonObject rate = row.value(u"rate"_s).toObject();
    QVERIFY(rate.value(u"rx_bps"_s).isDouble());
    QVERIFY(std::isfinite(rate.value(u"rx_bps"_s).toDouble()));
    QVERIFY(rate.value(u"rx_bps"_s).toDouble() >= 0.0);

    // The interface counters are decimal strings too, and the index is an int.
    const QJsonObject interface = interfaceArray(service.snapshot()).first().toObject();
    QVERIFY(interface.value(u"today"_s).toObject().value(u"rx_bytes"_s).isString());
    QVERIFY(interface.value(u"index"_s).isDouble());
    QVERIFY(interface.value(u"index"_s).toInt() > 0);

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
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot().value(u"interfaces_available"_s).toBool(), 3000);
    QVERIFY(!interfaceArray(service.snapshot()).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot()
                                 .value(u"process_status"_s)
                                 .toObject()
                                 .value(u"notes"_s)
                                 .toArray()
                                 .contains(u"collector not running"_s),
                             5000);
    const QJsonObject status = service.snapshot().value(u"process_status"_s).toObject();
    QVERIFY(!status.value(u"available"_s).toBool());
    QVERIFY(status.value(u"stale"_s).toBool());
    QVERIFY(status.value(u"incomplete"_s).toBool());
    QVERIFY(!service.error().isEmpty()); // the failure stays visible

    // A client still gets a well-formed snapshot.
    QLocalSocket client;
    client.connectToServer(options.socketPath);
    QVERIFY(client.waitForConnected(2000));
    QByteArray buffer;
    QByteArray line;
    QVERIFY(waitForLine(client, buffer, line, 2000));
    const QJsonObject snapshot = parseLine(line);
    QVERIFY(snapshot.value(u"interfaces_available"_s).toBool());
    QVERIFY(snapshot.value(u"processes"_s).toArray().isEmpty());

    service.stop();
    // No restart storm after stop.
    QTest::qWait(1500);
    QVERIFY(
        !service.snapshot().value(u"process_status"_s).toObject().value(u"available"_s).toBool());
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
}

void TestService::collectorRecoversWhenBinaryAppears() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Service service;
    ServiceOptions options{dir.filePath(u"gui.sock"_s), dir.filePath(u"collector"_s)};
    QVERIFY(service.start(options));
    QTRY_VERIFY_WITH_TIMEOUT(!service.error().isEmpty(), 5000);
    QVERIFY(QFile::link(fakeCollectorBinary(), options.collectorBinary));
    QTRY_VERIFY_WITH_TIMEOUT(
        service.snapshot().value(u"process_status"_s).toObject().value(u"available"_s).toBool(),
        10000);
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
            const auto rows = service.snapshot().value(u"processes"_s).toArray();
            return !rows.isEmpty() && rows.first()
                                              .toObject()
                                              .value(u"session"_s)
                                              .toObject()
                                              .value(u"rx_bytes"_s)
                                              .toString() != u"0"_s;
        }(),
        10000);
    const QString started = service.snapshot().value(u"started"_s).toString();
    service.stop();
    QVERIFY(service.start(options));
    QVERIFY(service.snapshot().value(u"started"_s).toString() != started);
    QVERIFY(service.snapshot().value(u"processes"_s).toArray().isEmpty());
    for (const auto value : service.snapshot().value(u"interfaces"_s).toArray()) {
        QCOMPARE(value.toObject().value(u"session"_s).toObject().value(u"rx_bytes"_s).toString(),
                 u"0"_s);
    }
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
    QByteArray buffer;
    QByteArray line;
    QVERIFY(waitForLine(active, buffer, line, 5000));

    // The healthy client keeps receiving snapshots while the raw one is dropped
    // once its write backlog exceeds the bound.
    const int lines = drainLines(active, buffer, 6000);
    QVERIFY2(lines >= 2, qPrintable(u"healthy client got %1 snapshots"_s.arg(lines)));
    QVERIFY(active.state() == QLocalSocket::ConnectedState);

    int slowLines = 0;
    const bool closed = drainUntilClosed(slow, slowLines, 8000);
    ::close(slow);
    QVERIFY2(closed,
             qPrintable(u"slow client got %1 snapshots without being dropped"_s.arg(slowLines)));
    QVERIFY2(slowLines < 60, qPrintable(u"slow client buffered %1 snapshots"_s.arg(slowLines)));

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
