// ball: a network traffic HUD for KDE Wayland sessions.
// End-to-end tests for the real executable: argument handling, daemon shutdown
// on signals and the promise that quitting the GUI never stops the backend.
// No display, no Wayland session and no root are needed.
#include "messagepack.h"
#include "snapshot.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <csignal>
#include <unistd.h>

using namespace Qt::StringLiterals;

#ifndef BALL_EXECUTABLE
#error "BALL_EXECUTABLE must point at the ball executable"
#endif
#ifndef BALL_FAKE_COLLECTOR_BINARY
#error "BALL_FAKE_COLLECTOR_BINARY must point at the fake collector fixture"
#endif

namespace {

QString ballBinary() { return QStringLiteral(BALL_EXECUTABLE); }
QString fakeCollectorBinary() { return QStringLiteral(BALL_FAKE_COLLECTOR_BINARY); }

// A headless environment: help/version must work with no display at all.
QProcessEnvironment headlessEnvironment() {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(u"DISPLAY"_s);
    environment.remove(u"WAYLAND_DISPLAY"_s);
    environment.remove(u"QT_QPA_PLATFORM"_s);
    return environment;
}

struct RunResult {
    int exitCode = -999;
    QByteArray standardOutput;
    QByteArray standardError;
};

RunResult runBall(const QStringList& arguments, const QProcessEnvironment& environment,
                  int timeoutMs = 10000) {
    QProcess process;
    process.setProcessEnvironment(environment);
    process.start(ballBinary(), arguments);
    RunResult result;
    if (!process.waitForStarted(5000)) {
        result.standardError = "failed to start ball";
        return result;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        result.standardError = "ball did not exit in time";
        return result;
    }
    result.exitCode = process.exitCode();
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    return result;
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

// A daemon that is running and serving: waits for the socket, then reads the
// first complete snapshot frame and decodes it.
bool firstSnapshot(const QString& socketPath, ball::Snapshot* snapshot, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs && !QFileInfo::exists(socketPath)) {
        QTest::qWait(20);
    }
    QLocalSocket client;
    client.connectToServer(socketPath);
    if (!client.waitForConnected(2000)) {
        return false;
    }
    ball::mp::Decoder decoder;
    while (clock.elapsed() < timeoutMs) {
        decoder.feed(client.readAll());
        ball::mp::Frame frame = decoder.next();
        if (frame.valid()) {
            const ball::SnapshotParseResult result = ball::decodeSnapshot(frame.value());
            if (!result.snapshot.has_value()) {
                return false;
            }
            *snapshot = *result.snapshot;
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

// Starts a daemon pointed at the fake collector and waits for its first
// snapshot; the caller terminates it through `process`.
bool startDaemon(QProcess& process, QTemporaryDir& dir, const QString& pidFile) {
    const QString socketPath = dir.filePath(u"gui.sock"_s);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(u"BALL_FAKE_PID_FILE"_s, pidFile);
    process.setProcessEnvironment(environment);
    process.start(ballBinary(), {u"--daemon"_s, u"--socket"_s, socketPath, u"--socktrail-bin"_s,
                                 fakeCollectorBinary()});
    if (!process.waitForStarted(5000)) {
        return false;
    }
    ball::Snapshot snapshot;
    if (!firstSnapshot(socketPath, &snapshot, 15000)) {
        return false;
    }
    return snapshot.version == 1;
}

void stopDaemon(QProcess& process) {
    if (process.state() == QProcess::NotRunning) {
        return;
    }
    // SIGTERM lets the daemon run its own shutdown, exactly like systemd.
    process.terminate();
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(3000);
    }
}

} // namespace

class TestCli : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void helpWorksWithoutDisplay();
    void versionWorksWithoutDisplay();
    void rejectsBadArguments();
    void rejectsMissingDaemonSocket();
    void rejectsSocktrailBinWithoutDaemon();
    void rejectsCollectorSocketMisuse();
    void daemonServesFirstSnapshot();
    void daemonUsesDefaultCollectorPath();
    void daemonExitsCleanlyOnSigterm();
    void daemonExitsCleanlyOnSigint();
    void daemonExitsCleanlyOnSighup();
    void guiExitDoesNotStopDaemon();
};

void TestCli::helpWorksWithoutDisplay() {
    const RunResult result = runBall({u"--help"_s}, headlessEnvironment());
    QCOMPARE(result.exitCode, 0);
    // Qt prints argv[0] in the usage line, so only the stable parts are checked.
    QVERIFY(result.standardOutput.contains("Usage:"));
    QVERIFY(result.standardOutput.contains("--daemon"));
    QVERIFY(result.standardError.isEmpty());

    const RunResult shortForm = runBall({u"-h"_s}, headlessEnvironment());
    QCOMPARE(shortForm.exitCode, 0);
    QVERIFY(shortForm.standardOutput.contains("Usage:"));

    const RunResult daemonHelp = runBall({u"--daemon"_s, u"--help"_s}, headlessEnvironment());
    QCOMPARE(daemonHelp.exitCode, 0);
    QVERIFY(daemonHelp.standardOutput.contains("--socket"));
}

void TestCli::versionWorksWithoutDisplay() {
    const RunResult result = runBall({u"--version"_s}, headlessEnvironment());
    QCOMPARE(result.exitCode, 0);
    // "ball <something>" without loading a GUI platform plugin.
    QVERIFY2(QRegularExpression(u"^ball \\S+"_s)
                 .match(QString::fromUtf8(result.standardOutput).trimmed())
                 .hasMatch(),
             result.standardOutput.constData());
}

void TestCli::rejectsBadArguments() {
    const QProcessEnvironment environment = headlessEnvironment();

    // Unknown option.
    RunResult result = runBall({u"--definitely-not-a-flag"_s}, environment);
    QVERIFY(result.exitCode != 0);
    QVERIFY(!result.standardError.isEmpty());

    // A relative --socket path is not accepted (both modes).
    result = runBall({u"--socket"_s, u"relative.sock"_s}, environment);
    QVERIFY(result.exitCode != 0);

    // A bare positional argument is never silently ignored.
    result = runBall({u"extra"_s}, environment);
    QVERIFY(result.exitCode != 0);

    // A missing option value.
    result = runBall({u"--socket"_s}, environment);
    QVERIFY(result.exitCode != 0);

    // A relative --socktrail-bin is rejected.
    result = runBall({u"--daemon"_s, u"--socket"_s, u"/tmp/ball-test.sock"_s, u"--socktrail-bin"_s,
                      u"collector"_s},
                     environment);
    QVERIFY(result.exitCode != 0);
}

void TestCli::rejectsMissingDaemonSocket() {
    const RunResult result = runBall({u"--daemon"_s}, headlessEnvironment());
    QVERIFY(result.exitCode != 0);
    QVERIFY(result.standardError.contains("socket"));
}

void TestCli::rejectsSocktrailBinWithoutDaemon() {
    const RunResult result =
        runBall({u"--socktrail-bin"_s, u"/absolute/collector"_s}, headlessEnvironment());
    QVERIFY(result.exitCode != 0);
    QVERIFY(result.standardError.contains("--daemon"));
}

void TestCli::rejectsCollectorSocketMisuse() {
    const QProcessEnvironment environment = headlessEnvironment();

    // Subscribe-only mode is a daemon option.
    RunResult result = runBall({u"--collector-socket"_s, u"/run/x.sock"_s}, environment);
    QVERIFY(result.exitCode != 0);
    QVERIFY(result.standardError.contains("--daemon"));

    // A relative collector socket is refused, exactly like --socket.
    result = runBall({u"--daemon"_s, u"--socket"_s, u"/tmp/ball-test.sock"_s,
                      u"--collector-socket"_s, u"relative.sock"_s},
                     environment);
    QVERIFY(result.exitCode != 0);
    QVERIFY(result.standardError.contains("absolute"));

    // Spawning a collector and subscribing to another one cannot both be meant.
    result = runBall({u"--daemon"_s, u"--socket"_s, u"/tmp/ball-test.sock"_s, u"--socktrail-bin"_s,
                      u"/absolute/collector"_s, u"--collector-socket"_s, u"/run/x.sock"_s},
                     environment);
    QVERIFY(result.exitCode != 0);
    QVERIFY(result.standardError.contains("mutually exclusive"));
}

void TestCli::daemonServesFirstSnapshot() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString socketPath = dir.filePath(u"gui.sock"_s);
    const QString pidFile = dir.filePath(u"collector.pid"_s);
    QProcess daemon;
    QVERIFY2(startDaemon(daemon, dir, pidFile), "the daemon did not serve a first snapshot");

    qint64 collectorPid = 0;
    QTRY_VERIFY_WITH_TIMEOUT(readPidFile(pidFile, collectorPid), 5000);
    QVERIFY(processAlive(collectorPid));

    stopDaemon(daemon);
    QVERIFY2(!QFileInfo::exists(socketPath), "the socket must be cleaned up");
    QVERIFY2(!processAlive(collectorPid), "the collector must not be left behind");
}

void TestCli::daemonUsesDefaultCollectorPath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString socketPath = dir.filePath(u"gui.sock"_s);
    QProcess daemon;
    // No --socktrail-bin: the compiled-in default applies. It may not exist on
    // this machine, but interface sampling must still serve snapshots.
    daemon.start(ballBinary(), {u"--daemon"_s, u"--socket"_s, socketPath});
    QVERIFY(daemon.waitForStarted(5000));
    ball::Snapshot snapshot;
    QVERIFY2(firstSnapshot(socketPath, &snapshot, 10000), "no snapshot from the default daemon");
    QCOMPARE(snapshot.version, 1);
    QVERIFY(snapshot.at.isValid());
    stopDaemon(daemon);
}

void TestCli::daemonExitsCleanlyOnSigterm() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString socketPath = dir.filePath(u"gui.sock"_s);
    const QString pidFile = dir.filePath(u"collector.pid"_s);
    QProcess daemon;
    QVERIFY2(startDaemon(daemon, dir, pidFile), "the daemon did not serve a first snapshot");
    qint64 collectorPid = 0;
    QTRY_VERIFY_WITH_TIMEOUT(readPidFile(pidFile, collectorPid), 5000);

    QVERIFY(::kill(static_cast<pid_t>(daemon.processId()), SIGTERM) == 0);
    QVERIFY2(daemon.waitForFinished(10000), "the daemon ignored SIGTERM");
    QCOMPARE(daemon.exitCode(), 0);
    QVERIFY2(!QFileInfo::exists(socketPath), "the socket must be cleaned up");
    QVERIFY2(!processAlive(collectorPid), "the collector must not survive the daemon");
}

void TestCli::daemonExitsCleanlyOnSigint() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pidFile = dir.filePath(u"collector.pid"_s);
    QProcess daemon;
    QVERIFY2(startDaemon(daemon, dir, pidFile), "the daemon did not serve a first snapshot");

    QVERIFY(::kill(static_cast<pid_t>(daemon.processId()), SIGINT) == 0);
    QVERIFY2(daemon.waitForFinished(10000), "the daemon ignored SIGINT");
    QCOMPARE(daemon.exitCode(), 0);
    QVERIFY(!QFileInfo::exists(dir.filePath(u"gui.sock"_s)));
}

void TestCli::daemonExitsCleanlyOnSighup() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QProcess daemon;
    QVERIFY(startDaemon(daemon, dir, dir.filePath(u"collector.pid"_s)));
    QVERIFY(::kill(static_cast<pid_t>(daemon.processId()), SIGHUP) == 0);
    QVERIFY(daemon.waitForFinished(10000));
    QCOMPARE(daemon.exitCode(), 0);
    QVERIFY(!QFileInfo::exists(dir.filePath(u"gui.sock"_s)));
}

void TestCli::guiExitDoesNotStopDaemon() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString socketPath = dir.filePath(u"gui.sock"_s);
    const QString pidFile = dir.filePath(u"collector.pid"_s);
    QProcess daemon;
    QVERIFY2(startDaemon(daemon, dir, pidFile), "the daemon did not serve a first snapshot");
    qint64 collectorPid = 0;
    QTRY_VERIFY_WITH_TIMEOUT(readPidFile(pidFile, collectorPid), 5000);

    // The GUI cannot run headless (no Wayland), so it exits right away. That
    // failed exit must leave the daemon and its collector untouched.
    QProcessEnvironment guiEnvironment = headlessEnvironment();
    guiEnvironment.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
    const RunResult gui = runBall({u"--socket"_s, socketPath}, guiEnvironment);
    QVERIFY(gui.exitCode != 0);

    QVERIFY(daemon.state() == QProcess::Running);
    QVERIFY(processAlive(collectorPid));
    ball::Snapshot snapshot;
    QVERIFY2(firstSnapshot(socketPath, &snapshot, 5000),
             "the daemon stopped serving after the GUI exited");
    QVERIFY(QFileInfo::exists(socketPath));

    stopDaemon(daemon);
}

QTEST_GUILESS_MAIN(TestCli)
#include "cli.moc"
