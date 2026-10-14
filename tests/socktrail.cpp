// ball: a network traffic HUD for KDE Wayland sessions.
// Tests for the socktrail NDJSON client, driven by a real QLocalServer mock on
// a local Unix socket (no root, no network access).
#include "daemon/socktrail.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QUuid>

#include <limits>

using ball::ProcessTraffic;
using ball::Socktrail;
using ball::SocktrailFrame;
using ball::SocktrailFrameStatus;
using namespace Qt::StringLiterals;

namespace {

const QString kStarted = u"2026-10-13T14:00:00Z"_s;

QByteArray toLine(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray helloLine(const QString& started = kStarted, int version = 1) {
    const QJsonObject hello{{u"version"_s, version}, {u"started"_s, started}};
    return toLine(QJsonObject{{u"kind"_s, u"hello"_s}, {u"hello"_s, hello}});
}

QJsonObject processObject(int pid, qint64 startNs, const QString& name, qint64 rx, qint64 tx) {
    return QJsonObject{{u"pid"_s, QJsonValue(qint64(pid))},
                       {u"start_ns"_s, QJsonValue(startNs)},
                       {u"name"_s, name},
                       {u"rx_bytes"_s, QJsonValue(rx)},
                       {u"tx_bytes"_s, QJsonValue(tx)}};
}

QJsonObject statsObject(const QString& at, const QJsonArray& processes) {
    const QJsonObject io{{u"rx_bytes"_s, 0}, {u"tx_bytes"_s, 0}};
    const QJsonObject probes{
        {u"pid"_s, QJsonObject{{u"kernel_lost"_s, 0}, {u"dropped"_s, 0}, {u"invalid"_s, 0}}}};
    const QJsonObject stats{{u"at"_s, at},
                            {u"process_io"_s, processes},
                            {u"io_unindexed"_s, io},
                            {u"expired_pid_io"_s, io},
                            {u"expired_pid_count"_s, 0},
                            {u"probes"_s, probes}};
    return QJsonObject{{u"kind"_s, u"stats"_s}, {u"stats"_s, stats}};
}

QByteArray statsLine(const QString& at, const QJsonArray& processes) {
    return toLine(statsObject(at, processes));
}

QJsonArray oneProcess(int pid, qint64 startNs, const QString& name, qint64 rx, qint64 tx) {
    return QJsonArray{processObject(pid, startNs, name, rx, tx)};
}

// A flow frame: legal, but never allowed to feed the totals.
QByteArray flowLine() {
    return toLine(QJsonObject{{u"kind"_s, u"flow"_s},
                              {u"flow"_s, QJsonObject{{u"pid"_s, 99},
                                                      {u"start_ns"_s, 1},
                                                      {u"rx_bytes"_s, 5'000'000},
                                                      {u"tx_bytes"_s, 6'000'000}}}});
}

} // namespace

class TestSocktrail : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void frameValidation();
    void frameNumbersAreExact();
    void timestampNeedsAnOffset();
    void helloThenFirstStats();
    void statsBeforeHelloIsRejected();
    void accumulatedStatsAcrossSplitFrames();
    void flowFramesAreIgnored();
    void sameMillisecondStatsAreNotLost();
    void newerStatsSupersedePendingStats();
    void firstConnectionFailureIsNotHistoricalGap();
    void qualityNotesMakeObservationIncomplete();
    void rejectedFrameLeavesStateUntouched();
    void disconnectAndReconnectBridgesGap();
    void reconnectAfterBackendRestartRebaselines();
    void staleAfterMissingStats();
    void staleBeforeFirstStatsHasReason();
    void invalidFrameDisconnectsWithShortError();
    void oversizedLineIsDropped();
    void stopStopsReconnecting();

private:
    QString startServer();
    QLocalSocket* acceptConnection(int timeoutMs = 3000);
    void send(QByteArray raw);

    QLocalServer m_server;
    QLocalSocket* m_client = nullptr;
    QString m_name;
};

void TestSocktrail::init() {
    // A unique name keeps parallel runs and leftover socket files apart; the
    // socket lives in the local runtime directory and needs no root.
    m_name = u"ball-socktrail-%1"_s.arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(m_server.listen(m_name));
}

void TestSocktrail::cleanup() {
    delete m_client;
    m_client = nullptr;
    m_server.close();
    QLocalServer::removeServer(m_name);
}

QString TestSocktrail::startServer() { return m_server.serverName(); }

QLocalSocket* TestSocktrail::acceptConnection(int timeoutMs) {
    // QLocalServer::waitForNewConnection does not run the client's timers, so
    // the pending connection is polled with qWait instead.
    QElapsedTimer waited;
    waited.start();
    while (!m_server.hasPendingConnections() && waited.elapsed() < timeoutMs) {
        QTest::qWait(20);
    }
    delete m_client;
    m_client = nullptr;
    if (!m_server.hasPendingConnections()) {
        return nullptr;
    }
    m_client = m_server.nextPendingConnection();
    return m_client;
}

void TestSocktrail::send(QByteArray raw) {
    QLocalSocket* socket = m_client;
    QVERIFY(socket != nullptr);
    if (socket == nullptr) {
        return; // the caller's acceptConnection() already failed the test
    }
    raw.append('\n');
    QCOMPARE(socket->write(raw), qint64(raw.size()));
    socket->flush();
}

void TestSocktrail::frameValidation() {
    // hello
    const SocktrailFrame hello = ball::decodeSocktrailFrame(helloLine());
    QCOMPARE(hello.status, SocktrailFrameStatus::Hello);
    QCOMPARE(hello.backendStarted, QDateTime::fromString(kStarted, Qt::ISODate));

    QCOMPARE(ball::decodeSocktrailFrame(helloLine(kStarted, 2)).status,
             SocktrailFrameStatus::Unsupported);
    QCOMPARE(ball::decodeSocktrailFrame(helloLine(u"not a time"_s)).status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(ball::decodeSocktrailFrame(helloLine(u"2026-10-13T14:00:00"_s)).status,
             SocktrailFrameStatus::ProtocolError);

    // A flow frame is a legal object that must not yield any process data.
    QCOMPARE(ball::decodeSocktrailFrame(flowLine()).status, SocktrailFrameStatus::Ignored);

    // Malformed, unknown and non-object frames are protocol errors.
    QCOMPARE(ball::decodeSocktrailFrame("[1,2]").status, SocktrailFrameStatus::ProtocolError);
    QCOMPARE(ball::decodeSocktrailFrame("{oops").status, SocktrailFrameStatus::ProtocolError);
    QCOMPARE(ball::decodeSocktrailFrame(toLine(QJsonObject{{u"kind"_s, u"nope"_s}})).status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(ball::decodeSocktrailFrame(toLine(QJsonObject{{u"hello"_s, QJsonObject{}}})).status,
             SocktrailFrameStatus::ProtocolError);

    // Good stats frame with one process.
    const SocktrailFrame stats = ball::decodeSocktrailFrame(
        statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(42, 1000, u"firefox"_s, 10, 20)));
    QCOMPARE(stats.status, SocktrailFrameStatus::Ok);
    QCOMPARE(stats.at, QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate));
    QCOMPARE(stats.counters.size(), 1);
    QCOMPARE(stats.counters.first().pid, 42);
    QCOMPARE(stats.counters.first().startNs, quint64(1000));
    QCOMPARE(stats.counters.first().name, u"firefox"_s);
    QCOMPARE(stats.counters.first().rxBytes, quint64(10));
    QCOMPARE(stats.counters.first().txBytes, quint64(20));

    // "at" is required and must be a valid RFC 3339 timestamp.
    QCOMPARE(ball::decodeSocktrailFrame(
                 toLine(QJsonObject{{u"kind"_s, u"stats"_s}, {u"stats"_s, QJsonObject{}}}))
                 .status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(ball::decodeSocktrailFrame(statsLine(u"tomorrow"_s, {})).status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(ball::decodeSocktrailFrame(statsLine(u"2026-10-13T14:00:01"_s, {})).status,
             SocktrailFrameStatus::ProtocolError);
}

void TestSocktrail::frameNumbersAreExact() {
    // 2^53 + 1 must survive as an exact start_ns and counter.
    const qint64 bigStart = 9'007'199'254'740'993LL;
    const QJsonArray processes{processObject(7, bigStart, u"curl"_s, 9'007'199'254'740'993LL, 0),
                               processObject(8, 1, u"other"_s, 5, 6)};
    const SocktrailFrame frame =
        ball::decodeSocktrailFrame(statsLine(u"2026-10-13T14:00:01Z"_s, processes));
    QCOMPARE(frame.status, SocktrailFrameStatus::Ok);
    QCOMPARE(frame.counters.size(), 2);
    QCOMPARE(frame.counters.first().startNs, quint64(bigStart));
    QCOMPARE(frame.counters.first().rxBytes, quint64(9'007'199'254'740'993LL));

    const qint64 maximum = std::numeric_limits<qint64>::max();
    const auto maximumFrame = ball::decodeSocktrailFrame(
        statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, maximum, u"max"_s, maximum, 0)));
    QCOMPARE(maximumFrame.status, SocktrailFrameStatus::Ok);
    QCOMPARE(maximumFrame.counters.first().rxBytes, quint64(maximum));

    // A count past 2^63-1 cannot be read exactly, so the whole frame is
    // refused instead of accepting a partially rounded process table.
    QJsonObject huge = processObject(8, 1, u"huge"_s, 1, 0);
    huge[u"rx_bytes"_s] = 18'446'744'073'709'551'616.0; // 2^64 as a double
    QCOMPARE(
        ball::decodeSocktrailFrame(statsLine(u"2026-10-13T14:00:01Z"_s, QJsonArray{huge})).status,
        SocktrailFrameStatus::ProtocolError);
    QJsonObject hugeStart = processObject(7, 1, u"x"_s, 1, 0);
    hugeStart[u"start_ns"_s] = 9'223'372'036'854'775'808.0; // 2^63 as a double
    QCOMPARE(ball::decodeSocktrailFrame(statsLine(u"2026-10-13T14:00:01Z"_s, QJsonArray{hugeStart}))
                 .status,
             SocktrailFrameStatus::ProtocolError);

    // Bad numbers never reach the accumulator.
    QCOMPARE(ball::decodeSocktrailFrame(
                 statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, -1, u"curl"_s, 1, 1)))
                 .status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(ball::decodeSocktrailFrame(statsLine(u"2026-10-13T14:00:01Z"_s,
                                                  QJsonArray{processObject(7, 1, u"x"_s, -1, 0)}))
                 .status,
             SocktrailFrameStatus::ProtocolError);

    QJsonObject fractional = processObject(7, 1, u"x"_s, 1, 0);
    fractional[u"rx_bytes"_s] = 1.5;
    QCOMPARE(
        ball::decodeSocktrailFrame(statsLine(u"2026-10-13T14:00:01Z"_s, QJsonArray{fractional}))
            .status,
        SocktrailFrameStatus::ProtocolError);

    QJsonObject missingStart = processObject(7, 1, u"x"_s, 1, 0);
    missingStart.remove(u"start_ns"_s);
    QCOMPARE(
        ball::decodeSocktrailFrame(statsLine(u"2026-10-13T14:00:01Z"_s, QJsonArray{missingStart}))
            .status,
        SocktrailFrameStatus::ProtocolError);

    // PID 0 and PID -1 do not name a process.
    QCOMPARE(ball::decodeSocktrailFrame(
                 statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(0, 1, u"x"_s, 1, 1)))
                 .status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(ball::decodeSocktrailFrame(
                 statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(-1, 1, u"many"_s, 1, 1)))
                 .status,
             SocktrailFrameStatus::ProtocolError);

    // The same identity twice in one snapshot is ambiguous.
    QCOMPARE(ball::decodeSocktrailFrame(statsLine(u"2026-10-13T14:00:01Z"_s,
                                                  QJsonArray{processObject(7, 5, u"a"_s, 1, 1),
                                                             processObject(7, 5, u"b"_s, 2, 2)}))
                 .status,
             SocktrailFrameStatus::ProtocolError);
}

void TestSocktrail::timestampNeedsAnOffset() {
    // A timestamp without a zone cannot name an instant; it is rejected on both
    // hello and stats.
    QCOMPARE(ball::decodeSocktrailFrame(helloLine(u"2026-10-13T14:00:00"_s)).status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(ball::decodeSocktrailFrame(statsLine(u"2026-10-13T14:00:01.5"_s, {})).status,
             SocktrailFrameStatus::ProtocolError);
    // An explicit offset is fine.
    const SocktrailFrame frame = ball::decodeSocktrailFrame(
        statsLine(u"2026-10-13T14:00:01+02:00"_s, oneProcess(7, 1, u"x"_s, 1, 1)));
    QCOMPARE(frame.status, SocktrailFrameStatus::Ok);
    QCOMPARE(frame.at.toLocalTime(),
             QDateTime::fromString(u"2026-10-13T14:00:01+02:00"_s, Qt::ISODate).toLocalTime());
}

void TestSocktrail::helloThenFirstStats() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    QVERIFY(!client.available()); // a connection is not data
    QVERIFY(client.incomplete());
    QVERIFY(client.stale());

    send(helloLine());
    QTest::qWait(30);
    QVERIFY(!client.available()); // still not available without a stats frame

    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 2'000)));
    QTRY_VERIFY(client.available());
    QVERIFY(!client.incomplete());
    QVERIFY(!client.stale());
    QVERIFY(client.error().isEmpty());
    QCOMPARE(client.processes().monitoringStart(),
             QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate).toLocalTime());

    // The first snapshot is a baseline: no traffic is counted yet.
    const ProcessTraffic row = client.processes().row(7, 500);
    QVERIFY(row.present);
    QCOMPARE(row.name, u"curl"_s);
    QCOMPARE(row.session.rxBytes, quint64(0));
    QCOMPARE(row.session.txBytes, quint64(0));
}

void TestSocktrail::statsBeforeHelloIsRejected() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());

    // Stats may only follow a valid hello; otherwise the client would trust a
    // table it cannot identify.
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 2'000)));
    QTRY_VERIFY(!client.error().isEmpty());
    QVERIFY(!client.available());
    QVERIFY(client.incomplete());

    // Once hello and stats arrive, the same subscription is healthy.
    QVERIFY(acceptConnection(4000));
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:02Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 2'000)));
    QTRY_VERIFY(client.available());
}

void TestSocktrail::accumulatedStatsAcrossSplitFrames() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());

    // The hello frame arrives in two chunks and a later write carries two stats
    // frames, so both a half line and multiple frames in one read are
    // exercised.
    const QByteArray hello = helloLine();
    m_client->write(hello.left(10));
    m_client->flush();
    QTest::qWait(20);
    m_client->write(hello.mid(10) + '\n');
    m_client->flush();

    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 2'000)));
    QTRY_VERIFY(client.available());
    QTest::qWait(30);
    m_client->write(
        statsLine(u"2026-10-13T14:00:02Z"_s, oneProcess(7, 500, u"curl"_s, 1'400, 2'900)) + '\n');
    m_client->flush();
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(400));
    QCOMPARE(client.processes().row(7, 500).session.txBytes, quint64(900));

    // A later frame always adds exactly the counter difference, whatever the
    // elapsed time was.
    QTest::qWait(30);
    send(statsLine(u"2026-10-13T14:00:03Z"_s, oneProcess(7, 500, u"curl"_s, 2'100, 3'000)));
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(1'100));
    QCOMPARE(client.processes().row(7, 500).session.txBytes, quint64(1'000));
    QVERIFY(client.processes().row(7, 500).rate.rxBytesPerSecond > 0.0);
}

void TestSocktrail::flowFramesAreIgnored() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 0)));
    QTRY_VERIFY(client.available());
    QTest::qWait(30);

    // Flow frames name one connection's bytes; they must not feed the totals,
    // add any identity, or refresh the freshness clock.
    send(flowLine());
    send(statsLine(u"2026-10-13T14:00:02Z"_s, oneProcess(7, 500, u"curl"_s, 1'100, 0)));
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(100));
    QCOMPARE(client.processes().rows().size(), 1);
    QCOMPARE(client.processes().row(99, 1).present, false);
}

void TestSocktrail::sameMillisecondStatsAreNotLost() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 0)));
    QTRY_VERIFY(client.available());

    // Two stats frames in one write usually land in one readyRead within one
    // millisecond. The newest sample must not be dropped just because it shares
    // its millisecond with the previous one.
    m_client->write(
        statsLine(u"2026-10-13T14:00:02Z"_s, oneProcess(7, 500, u"curl"_s, 1'400, 0)) + '\n' +
        statsLine(u"2026-10-13T14:00:03Z"_s, oneProcess(7, 500, u"curl"_s, 1'700, 0)) + '\n');
    m_client->flush();
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(700));
    QVERIFY(!client.incomplete());
}

void TestSocktrail::newerStatsSupersedePendingStats() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"app"_s, 1'000, 0)));
    QTRY_VERIFY(client.available());

    QJsonObject padding{{u"kind"_s, u"flow"_s}, {u"padding"_s, QString(3LL * 1024 * 1024, u'x')}};
    QByteArray batch = statsLine(u"2026-10-13T14:00:02Z"_s, oneProcess(7, 500, u"app"_s, 1'400, 0));
    batch += '\n';
    batch += statsLine(u"2026-10-13T14:00:03Z"_s, oneProcess(7, 500, u"app"_s, 2'100, 0));
    batch += '\n';
    batch += toLine(padding);
    batch += '\n';
    batch += statsLine(u"2026-10-13T14:00:04Z"_s, oneProcess(7, 500, u"app"_s, 2'600, 0));
    send(batch);
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(1'600));
    QTest::qWait(20);
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(1'600));
}

void TestSocktrail::firstConnectionFailureIsNotHistoricalGap() {
    m_server.close();
    Socktrail client;
    client.start(m_name);
    QTRY_VERIFY(!client.error().isEmpty());
    QVERIFY(m_server.listen(m_name));
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"app"_s, 1'000, 0)));
    QTRY_VERIFY(client.available());
    QVERIFY(!client.incomplete());
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(0));
}

void TestSocktrail::qualityNotesMakeObservationIncomplete() {
    const QJsonObject io{{u"rx_bytes"_s, 900}, {u"tx_bytes"_s, 100}};
    const QJsonObject probes{
        {u"pid"_s, QJsonObject{{u"kernel_lost"_s, 2}, {u"dropped"_s, 3}, {u"invalid"_s, 4}}}};
    const QJsonObject stats{{u"at"_s, u"2026-10-13T14:00:01Z"_s},
                            {u"process_io"_s, oneProcess(7, 5, u"curl"_s, 10, 20)},
                            {u"io_unindexed"_s, io},
                            {u"expired_pid_io"_s, io},
                            {u"expired_pid_count"_s, 1},
                            {u"probes"_s, probes}};
    const SocktrailFrame frame = ball::decodeSocktrailFrame(
        toLine(QJsonObject{{u"kind"_s, u"stats"_s}, {u"stats"_s, stats}}));
    QCOMPARE(frame.status, SocktrailFrameStatus::Ok);
    QCOMPARE(frame.counters.size(), 1);
    QCOMPARE(frame.qualityNotes.size(), 3);

    // Lost events are never added to a process total, and the notes mark the
    // observation as incomplete instead of silently discarding the loss.
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(toLine(QJsonObject{{u"kind"_s, u"stats"_s}, {u"stats"_s, stats}}));
    QTRY_COMPARE(client.processes().rows().size(), 1);
    QCOMPARE(client.qualityNotes().size(), 3);
    QVERIFY(client.incomplete());
    QCOMPARE(client.processes().row(7, 5).session.rxBytes, quint64(0));

    // A following clean frame keeps the marker: already-lost events cannot be
    // recovered, and the client must not claim complete observation again.
    send(statsLine(u"2026-10-13T14:00:02Z"_s, oneProcess(7, 5, u"curl"_s, 30, 20)));
    QTRY_COMPARE(client.processes().row(7, 5).session.rxBytes, quint64(20));
    QVERIFY(client.incomplete());
}

void TestSocktrail::rejectedFrameLeavesStateUntouched() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 500)));
    QTRY_COMPARE(client.processes().rows().size(), 1);
    const QList<ProcessTraffic> before = client.processes().rows();

    // A stats frame whose last process is broken must not contribute any of its
    // earlier processes either.
    const QJsonArray mixed{processObject(9, 1, u"evil"_s, 5, 5),
                           processObject(8, 2, u"good"_s, -3, 0)};
    send(statsLine(u"2026-10-13T14:00:02Z"_s, mixed));
    QTRY_VERIFY(!client.available());
    QCOMPARE(client.processes().rows().size(), before.size());
    QCOMPARE(client.processes().row(9, 1).present, false);
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(0));
}

void TestSocktrail::disconnectAndReconnectBridgesGap() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 0)));
    QTRY_VERIFY(client.available());
    QTest::qWait(30);
    send(statsLine(u"2026-10-13T14:00:02Z"_s, oneProcess(7, 500, u"curl"_s, 1'500, 0)));
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(500));

    // The mock drops the subscriber; the client must report the gap instead of
    // pretending the data is still live.
    QSignalSpy updates(&client, &Socktrail::updated);
    m_client->close();
    QTRY_VERIFY(!client.available());
    QVERIFY(client.incomplete());
    QVERIFY(client.stale());
    QVERIFY(!client.error().isEmpty());
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(500));
    QVERIFY(updates.count() > 0);

    // Reconnect with the same backend session: the accumulated counter gap is
    // recovered by the counter difference, not guessed, yet the interval that
    // was never observed keeps incomplete() true.
    QVERIFY(acceptConnection(4000));
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:03Z"_s, oneProcess(7, 500, u"curl"_s, 2'000, 0)));
    QTRY_VERIFY(client.available());
    QVERIFY(client.incomplete());
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(1'000));
}

void TestSocktrail::reconnectAfterBackendRestartRebaselines() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 0)));
    QTRY_VERIFY(client.available());
    QTest::qWait(30);
    send(statsLine(u"2026-10-13T14:00:02Z"_s, oneProcess(7, 500, u"curl"_s, 1'500, 0)));
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(500));

    m_client->close();
    QTRY_VERIFY(!client.available());
    QVERIFY(acceptConnection(4000));

    // A different `started` means the backend restarted with fresh counters:
    // the old totals are kept, but the new counters only rebaseline.
    send(helloLine(u"2026-10-13T15:00:00Z"_s));
    send(statsLine(u"2026-10-13T15:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 800, 0)));
    QTRY_VERIFY(client.available());
    QVERIFY(client.incomplete());
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(500));
}

void TestSocktrail::staleAfterMissingStats() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 0)));
    QTRY_VERIFY(client.available());
    QVERIFY(!client.stale());

    // No stats for longer than the freshness window, even though the socket is
    // still up and flow frames may keep arriving.
    send(flowLine());
    QTest::qWait(Socktrail::kStatsTimeoutMs + 300);
    QVERIFY(client.stale());
    QVERIFY(client.available());
    QVERIFY(!client.error().isEmpty());
    QCOMPARE(client.processes().row(7, 500).present, true);

    // A fresh stats frame makes the data live again.
    send(statsLine(u"2026-10-13T14:00:10Z"_s, oneProcess(7, 500, u"curl"_s, 1'200, 0)));
    QTRY_VERIFY(!client.stale());
    QVERIFY(client.available());
}

void TestSocktrail::staleBeforeFirstStatsHasReason() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(flowLine());
    QVERIFY(!client.available());
    QTRY_COMPARE_WITH_TIMEOUT(client.error(), QByteArrayLiteral("no stats"), 7'000);
    QVERIFY(client.stale());
}

void TestSocktrail::invalidFrameDisconnectsWithShortError() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1, 1)));
    QTRY_VERIFY(client.available());

    // A stats frame with a broken timestamp drops the connection.
    send(toLine(QJsonObject{{u"kind"_s, u"stats"_s},
                            {u"stats"_s, QJsonObject{{u"at"_s, u"not-a-time"_s}}}}));
    QTRY_VERIFY(!client.available());
    QVERIFY(client.incomplete());
    QVERIFY(!client.error().isEmpty());
    QVERIFY(client.error().size() <= 120);
    for (const char byte : client.error()) {
        QVERIFY(byte > 0 && byte < 0x7f);
    }

    // The client keeps trying: a healthy peer restores the connection.
    QVERIFY(acceptConnection(4000));
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:02Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 0)));
    QTRY_VERIFY(client.available());
    QVERIFY(client.error().isEmpty());
}

void TestSocktrail::oversizedLineIsDropped() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());

    // A peer that never sends a newline must not grow the buffer without bound:
    // the oversized frame is rejected before the accumulator sees anything.
    QByteArray flood(Socktrail::kMaxLineBytes + 1024, 'x');
    m_client->write(flood);
    m_client->flush();
    QTRY_VERIFY(!client.error().isEmpty());
    QVERIFY(!client.available());
}

void TestSocktrail::stopStopsReconnecting() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(u"2026-10-13T14:00:01Z"_s, oneProcess(7, 500, u"curl"_s, 1'000, 0)));
    QTRY_VERIFY(client.available());

    client.stop();
    QVERIFY(!client.available());
    QVERIFY(client.stale());
    if (m_client != nullptr) {
        m_client->close();
        delete m_client;
        m_client = nullptr;
    }

    // The server stays up, so a reconnect would succeed if one were attempted.
    QTest::qWait(1500);
    QVERIFY(!m_server.hasPendingConnections());
    QVERIFY(!client.available());
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(0));
}

QTEST_GUILESS_MAIN(TestSocktrail)
#include "socktrail.moc"
