// ball: a network traffic HUD for KDE Wayland sessions.
// Tests for the socktrail MessagePack client, driven by a real QLocalServer mock
// on a local Unix socket (no root, no network access).
#include "daemon/socktrail.h"

#include "mp_frames.h"

#include <QDateTime>
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
using ball::test::frame;
using namespace Qt::StringLiterals;

namespace {

const QDateTime kStarted = QDateTime::fromString(u"2026-10-13T14:00:00Z"_s, Qt::ISODate);

// One process row, as socktrail encodes it. start_ns is omitted when zero, which
// is what the publisher's omitempty tag does; the decoder reads that as 0.
void writeProcess(ball::test::Writer& writer, int pid, quint64 startNs, const QString& name,
                  quint64 rx, quint64 tx) {
    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
    packer.pack_map(startNs == 0 ? 4 : 5);
    writer.key(QLatin1StringView("pid"));
    packer.pack(pid);
    if (startNs != 0) {
        writer.key(QLatin1StringView("start_ns"));
        packer.pack(startNs);
    }
    writer.key(QLatin1StringView("name"));
    writer.text(name);
    writer.key(QLatin1StringView("rx_bytes"));
    packer.pack(rx);
    writer.key(QLatin1StringView("tx_bytes"));
    packer.pack(tx);
}

struct Process {
    int pid = 0;
    quint64 startNs = 0;
    QString name;
    quint64 rx = 0;
    quint64 tx = 0;
};

// The quality section: every field a real stats frame carries besides the table.
void writeQuality(ball::test::Writer& writer, quint64 kernelLost = 0, quint64 dropped = 0,
                  quint64 invalid = 0, quint64 expiredCount = 0, quint64 unindexedRx = 0,
                  quint64 unindexedTx = 0, quint64 unmatchedBytes = 0) {
    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
    writer.key(QLatin1StringView("io_unindexed"));
    packer.pack_map(2);
    writer.key(QLatin1StringView("rx_bytes"));
    packer.pack(unindexedRx);
    writer.key(QLatin1StringView("tx_bytes"));
    packer.pack(unindexedTx);
    writer.key(QLatin1StringView("expired_pid_io"));
    packer.pack_map(2);
    writer.key(QLatin1StringView("rx_bytes"));
    packer.pack(uint64_t(0));
    writer.key(QLatin1StringView("tx_bytes"));
    packer.pack(uint64_t(0));
    writer.key(QLatin1StringView("expired_pid_count"));
    packer.pack(expiredCount);
    writer.key(QLatin1StringView("io_unmatched_bytes"));
    packer.pack(unmatchedBytes);
    writer.key(QLatin1StringView("probes"));
    packer.pack_map(1);
    writer.key(QLatin1StringView("pid"));
    packer.pack_map(3);
    writer.key(QLatin1StringView("kernel_lost"));
    packer.pack(kernelLost);
    writer.key(QLatin1StringView("dropped"));
    packer.pack(dropped);
    writer.key(QLatin1StringView("invalid"));
    packer.pack(invalid);
}

// A healthy stats frame. The real publisher writes more fields than the client
// reads (ip_packets, ip_bytes); a decoder must ignore what it does not know, so
// the fixture writes only the fields in use.
QByteArray statsLine(const QDateTime& at, const QList<Process>& processes) {
    return ball::test::envelope(u"stats"_s, QLatin1StringView("stats"),
                                [&](ball::test::Writer& writer) {
                                    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                                    packer.pack_map(7);
                                    writer.key(QLatin1StringView("at"));
                                    writer.timestamp(at);
                                    writer.key(QLatin1StringView("process_io"));
                                    packer.pack_array(uint32_t(processes.size()));
                                    for (const Process& process : processes) {
                                        writeProcess(writer, process.pid, process.startNs,
                                                     process.name, process.rx, process.tx);
                                    }
                                    writeQuality(writer);
                                });
}

// A stats frame whose payload is built by the test, for the malformed cases.
QByteArray statsFrame(const std::function<void(ball::test::Writer&)>& fill) {
    return ball::test::envelope(u"stats"_s, QLatin1StringView("stats"), fill);
}

QByteArray helloLine(const QDateTime& started = kStarted, int version = 1) {
    return ball::test::envelope(u"hello"_s, QLatin1StringView("hello"),
                                [&](ball::test::Writer& writer) {
                                    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                                    packer.pack_map(2);
                                    writer.key(QLatin1StringView("version"));
                                    packer.pack(version);
                                    writer.key(QLatin1StringView("started"));
                                    writer.timestamp(started);
                                });
}

// A flow frame: legal, but never allowed to feed the totals.
QByteArray flowLine(const QString& padding = QString()) {
    return ball::test::envelope(u"flow"_s, QLatin1StringView("flow"),
                                [&](ball::test::Writer& writer) {
                                    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                                    packer.pack_map(3);
                                    writer.key(QLatin1StringView("pid"));
                                    packer.pack(99);
                                    writer.key(QLatin1StringView("rx_bytes"));
                                    packer.pack(uint64_t(5'000'000));
                                    writer.key(QLatin1StringView("tx_bytes"));
                                    packer.pack(uint64_t(6'000'000));
                                    (void)padding;
                                });
}

// Decodes one frame in isolation, exactly as the client does without a socket.
// A fixture that does not even frame must fail loudly here rather than reach the
// decoder with a null object.
SocktrailFrame decode(const QByteArray& raw) {
    ball::mp::Decoder decoder;
    decoder.feed(raw);
    ball::mp::Frame frame = decoder.next();
    if (!frame.valid()) {
        qFatal("fixture is not a complete MessagePack frame (decoder failed: %s): %s",
               decoder.failed() ? decoder.error().constData() : "incomplete",
               raw.toHex(' ').constData());
    }
    return ball::decodeSocktrailFrame(frame.value());
}

} // namespace

class TestSocktrail : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void frameValidation();
    void frameNumbersAreExact();
    void timestampMustBeATimestamp();
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
    void oversizedFrameIsDropped();
    void stopStopsReconnecting();
    void protocolReasonSurvivesTransportCleanupAndReconnect();

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
    // MessagePack frames carry their own length, so nothing is appended: each
    // call writes exactly the bytes of one frame.
    QCOMPARE(socket->write(raw), qint64(raw.size()));
    socket->flush();
}

void TestSocktrail::frameValidation() {
    const SocktrailFrame hello = decode(helloLine());
    QCOMPARE(hello.status, SocktrailFrameStatus::Hello);
    QCOMPARE(hello.backendStarted, kStarted.toLocalTime());

    QCOMPARE(decode(helloLine(kStarted, 2)).status, SocktrailFrameStatus::Unsupported);
    // A version that is not an integer at all is malformed, not merely newer.
    QCOMPARE(decode(ball::test::envelope(u"hello"_s, QLatin1StringView("hello"),
                                         [](ball::test::Writer& writer) {
                                             msgpack::packer<msgpack::sbuffer>& packer =
                                                 writer.packer();
                                             packer.pack_map(2);
                                             writer.key(QLatin1StringView("version"));
                                             writer.text(u"one"_s);
                                             writer.key(QLatin1StringView("started"));
                                             writer.timestamp(kStarted);
                                         }))
                 .status,
             SocktrailFrameStatus::ProtocolError);
    // hello without a start instant cannot identify the backend session.
    QCOMPARE(decode(ball::test::envelope(u"hello"_s, QLatin1StringView("hello"),
                                         [](ball::test::Writer& writer) {
                                             writer.packer().pack_map(1);
                                             writer.key(QLatin1StringView("version"));
                                             writer.packer().pack(1);
                                         }))
                 .status,
             SocktrailFrameStatus::ProtocolError);

    // A flow frame is a legal object that must not yield any process data.
    QCOMPARE(decode(flowLine()).status, SocktrailFrameStatus::Ignored);

    // Malformed, unknown and non-object frames are protocol errors.
    QCOMPARE(decode(frame([](ball::test::Writer& writer) {
                 writer.packer().pack_array(2);
                 writer.packer().pack(1);
                 writer.packer().pack(2);
             })).status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(decode(frame([](ball::test::Writer& writer) {
                 writer.packer().pack_map(1);
                 writer.key(QLatin1StringView("kind"));
                 writer.text(u"nope"_s);
             })).status,
             SocktrailFrameStatus::ProtocolError);
    // A frame with no "kind" at all is not a frame.
    QCOMPARE(decode(frame([](ball::test::Writer& writer) {
                 writer.packer().pack_map(1);
                 writer.key(QLatin1StringView("hello"));
                 writer.packer().pack_map(0);
             })).status,
             SocktrailFrameStatus::ProtocolError);
    // A "stats" frame with a payload that is not a map is malformed.
    QCOMPARE(
        decode(ball::test::envelope(u"stats"_s, QLatin1StringView("stats"),
                                    [](ball::test::Writer& writer) { writer.packer().pack_nil(); }))
            .status,
        SocktrailFrameStatus::ProtocolError);

    // Good stats frame with one process.
    const SocktrailFrame stats =
        decode(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                         {Process{42, 1000, u"firefox"_s, 10, 20}}));
    QCOMPARE(stats.status, SocktrailFrameStatus::Ok);
    QCOMPARE(stats.at, QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate).toLocalTime());
    QCOMPARE(stats.counters.size(), 1);
    QCOMPARE(stats.counters.first().pid, 42);
    QCOMPARE(stats.counters.first().startNs, quint64(1000));
    QCOMPARE(stats.counters.first().name, u"firefox"_s);
    QCOMPARE(stats.counters.first().rxBytes, quint64(10));
    QCOMPARE(stats.counters.first().txBytes, quint64(20));

    // "at", the process table and the quality section are all required.
    QCOMPARE(decode(statsFrame([](ball::test::Writer& writer) {
                 msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                 packer.pack_map(5); // everything but "at"
                 writer.key(QLatin1StringView("process_io"));
                 packer.pack_array(0);
                 writeQuality(writer);
             })).status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(decode(statsFrame([](ball::test::Writer& writer) {
                 msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                 packer.pack_map(7);
                 writer.key(QLatin1StringView("at"));
                 writer.timestamp(QDateTime::currentDateTimeUtc());
                 // process_io as a map instead of a table.
                 writer.key(QLatin1StringView("process_io"));
                 packer.pack_map(0);
                 writeQuality(writer);
             })).status,
             SocktrailFrameStatus::ProtocolError);
}

void TestSocktrail::frameNumbersAreExact() {
    // 2^53 + 1 is where a float64 stops counting exactly, so it must survive the
    // trip as a start_ns and as a counter.
    const quint64 bigStart = 9'007'199'254'740'993ULL;
    const SocktrailFrame frame = decode(
        statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                  {Process{7, bigStart, u"curl"_s, bigStart, 0}, Process{8, 1, u"other"_s, 5, 6}}));
    QCOMPARE(frame.status, SocktrailFrameStatus::Ok);
    QCOMPARE(frame.counters.size(), 2);
    QCOMPARE(frame.counters.first().startNs, bigStart);
    QCOMPARE(frame.counters.first().rxBytes, bigStart);

    const quint64 maximum = std::numeric_limits<quint64>::max();
    const SocktrailFrame maximumFrame =
        decode(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                         {Process{7, maximum, u"max"_s, maximum, 0}}));
    QCOMPARE(maximumFrame.status, SocktrailFrameStatus::Ok);
    QCOMPARE(maximumFrame.counters.first().rxBytes, maximum);

    // A count written as a float is accepted only when it is an exact integer,
    // so a peer cannot make a counter round silently.
    const auto withRx = [](const std::function<void(ball::test::Writer&)>& value) {
        return statsFrame([&](ball::test::Writer& writer) {
            msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
            packer.pack_map(7);
            writer.key(QLatin1StringView("at"));
            writer.timestamp(QDateTime::currentDateTimeUtc());
            writer.key(QLatin1StringView("process_io"));
            packer.pack_array(1);
            // One process whose rx_bytes is written by the test.
            packer.pack_map(4);
            writer.key(QLatin1StringView("pid"));
            packer.pack(7);
            writer.key(QLatin1StringView("name"));
            writer.text(u"curl"_s);
            writer.key(QLatin1StringView("rx_bytes"));
            value(writer);
            writer.key(QLatin1StringView("tx_bytes"));
            packer.pack(uint64_t(0));
            writeQuality(writer);
        });
    };
    QCOMPARE(decode(withRx([](ball::test::Writer& writer) { writer.packer().pack(1.5); })).status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(decode(withRx([](ball::test::Writer& writer) {
                 writer.packer().pack(18'446'744'073'709'551'616.0); // 2^64 as a double
             })).status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(decode(withRx([](ball::test::Writer& writer) { writer.packer().pack(-1); })).status,
             SocktrailFrameStatus::ProtocolError);
    // An integral float is exactly representable, so it is a legal count.
    const SocktrailFrame asFloat =
        decode(withRx([](ball::test::Writer& writer) { writer.packer().pack(2.0); }));
    QCOMPARE(asFloat.status, SocktrailFrameStatus::Ok);
    QCOMPARE(asFloat.counters.first().rxBytes, quint64(2));

    // PID 0 and PID -1 do not name a process.
    QCOMPARE(
        decode(statsLine(QDateTime::currentDateTimeUtc(), {Process{0, 1, u"x"_s, 1, 1}})).status,
        SocktrailFrameStatus::ProtocolError);
    QCOMPARE(decode(statsLine(QDateTime::currentDateTimeUtc(), {Process{-1, 1, u"many"_s, 1, 1}}))
                 .status,
             SocktrailFrameStatus::ProtocolError);

    // The same identity twice in one snapshot is ambiguous.
    QCOMPARE(decode(statsLine(QDateTime::currentDateTimeUtc(),
                              {Process{7, 5, u"a"_s, 1, 1}, Process{7, 5, u"b"_s, 2, 2}}))
                 .status,
             SocktrailFrameStatus::ProtocolError);
}

void TestSocktrail::timestampMustBeATimestamp() {
    // An instant has exactly one representation on the wire: the MessagePack
    // timestamp extension. Anything else is refused, however plausible.
    const auto withAt = [](const std::function<void(ball::test::Writer&)>& value) {
        return statsFrame([&](ball::test::Writer& writer) {
            msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
            packer.pack_map(7);
            writer.key(QLatin1StringView("at"));
            value(writer);
            writer.key(QLatin1StringView("process_io"));
            packer.pack_array(0);
            writeQuality(writer);
        });
    };
    QCOMPARE(decode(withAt([](ball::test::Writer& writer) {
                 writer.text(u"2026-10-13T14:00:01Z"_s);
             })).status,
             SocktrailFrameStatus::ProtocolError);
    QCOMPARE(
        decode(withAt([](ball::test::Writer& writer) { writer.text(u"not a time"_s); })).status,
        SocktrailFrameStatus::ProtocolError);
    QCOMPARE(decode(withAt([](ball::test::Writer& writer) {
                 writer.packer().pack(uint64_t(1'700'000'000));
             })).status,
             SocktrailFrameStatus::ProtocolError);
    // An extension of another type id is not a timestamp.
    QCOMPARE(decode(withAt([](ball::test::Writer& writer) {
                 const char body[4] = {0, 0, 0, 1};
                 writer.foreignExt(body, 4);
             })).status,
             SocktrailFrameStatus::ProtocolError);

    // An offset in the source instant does not survive as a string: the
    // timestamp names the instant, and it converts to the same local time.
    const SocktrailFrame frame =
        decode(statsLine(QDateTime::fromString(u"2026-10-13T16:00:01+02:00"_s, Qt::ISODate),
                         {Process{7, 1, u"x"_s, 1, 1}}));
    QCOMPARE(frame.status, SocktrailFrameStatus::Ok);
    QCOMPARE(frame.at,
             QDateTime::fromString(u"2026-10-13T16:00:01+02:00"_s, Qt::ISODate).toLocalTime());
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

    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 2'000}}));
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
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 2'000}}));
    QTRY_VERIFY(!client.error().isEmpty());
    QVERIFY(!client.available());
    QVERIFY(client.incomplete());

    // Once hello and stats arrive, the same subscription is healthy.
    QVERIFY(acceptConnection(4000));
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 2'000}}));
    QTRY_VERIFY(client.available());
}

void TestSocktrail::accumulatedStatsAcrossSplitFrames() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());

    // The hello frame arrives in two chunks and a later write carries two stats
    // frames, so both a half frame and several frames in one read are exercised.
    const QByteArray hello = helloLine();
    m_client->write(hello.left(10));
    m_client->flush();
    QTest::qWait(20);
    m_client->write(hello.mid(10));
    m_client->flush();

    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 2'000}}));
    QTRY_VERIFY(client.available());
    QTest::qWait(30);
    m_client->write(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                              {Process{7, 500, u"curl"_s, 1'400, 2'900}}) +
                    statsLine(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                              {Process{7, 500, u"curl"_s, 2'100, 3'000}}));
    m_client->flush();
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(1'100));
    QCOMPARE(client.processes().row(7, 500).session.txBytes, quint64(1'000));
    QVERIFY(client.processes().row(7, 500).rate.rxBytesPerSecond > 0.0);
}

void TestSocktrail::flowFramesAreIgnored() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());
    QTest::qWait(30);

    // Flow frames name one connection's bytes; they must not feed the totals,
    // add any identity, or refresh the freshness clock.
    send(flowLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'100, 0}}));
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(100));
    QCOMPARE(client.processes().rows().size(), 1);
    QCOMPARE(client.processes().row(99, 1).present, false);
}

void TestSocktrail::sameMillisecondStatsAreNotLost() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());

    // Two stats frames in one write usually land in one readyRead within one
    // millisecond. The newest sample must not be dropped just because it shares
    // its millisecond with the previous one.
    m_client->write(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                              {Process{7, 500, u"curl"_s, 1'400, 0}}) +
                    statsLine(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                              {Process{7, 500, u"curl"_s, 1'700, 0}}));
    m_client->flush();
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(700));
    QVERIFY(!client.incomplete());
}

void TestSocktrail::newerStatsSupersedePendingStats() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"app"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());

    // A large flow frame sits between the stats frames, so the batch spans
    // several reads; the newest pending sample must still be the one applied.
    QByteArray batch = statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                                 {Process{7, 500, u"app"_s, 1'400, 0}});
    batch += statsLine(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                       {Process{7, 500, u"app"_s, 2'100, 0}});
    batch += flowLine(QString(3LL * 1024 * 1024, u'x'));
    batch += statsLine(QDateTime::fromString(u"2026-10-13T14:00:04Z"_s, Qt::ISODate),
                       {Process{7, 500, u"app"_s, 2'600, 0}});
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
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"app"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());
    QVERIFY(!client.incomplete());
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(0));
}

void TestSocktrail::qualityNotesMakeObservationIncomplete() {
    // A stats frame that lost PID events and unindexed bytes: the totals it
    // carries are still applied, but the observation is not complete.
    const SocktrailFrame frame = decode(statsFrame([](ball::test::Writer& writer) {
        msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
        packer.pack_map(7);
        writer.key(QLatin1StringView("at"));
        writer.timestamp(QDateTime::currentDateTimeUtc());
        writer.key(QLatin1StringView("process_io"));
        packer.pack_array(1);
        writeProcess(writer, 7, 5, u"curl"_s, 10, 20);
        writeQuality(writer, 2, 3, 4, 1, 900, 100, 7'777);
    }));
    QCOMPARE(frame.status, SocktrailFrameStatus::Ok);
    QCOMPARE(frame.counters.size(), 1);
    QCOMPARE(frame.qualityNotes.size(), 4);
    // Socket I/O that matched no connection is named explicitly: it is missing
    // from every process row, so a reader must be told about it.
    QVERIFY(frame.qualityNotes.last().contains(u"unmatched socket I/O"_s));
    QVERIFY(frame.qualityNotes.last().contains(u"7777"_s));

    // Lost events are never added to a process total, and the notes mark the
    // observation as incomplete instead of silently discarding the loss.
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsFrame([](ball::test::Writer& writer) {
        msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
        packer.pack_map(7);
        writer.key(QLatin1StringView("at"));
        writer.timestamp(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate));
        writer.key(QLatin1StringView("process_io"));
        packer.pack_array(1);
        writeProcess(writer, 7, 5, u"curl"_s, 10, 20);
        writeQuality(writer, 2, 3, 4, 1, 900, 100);
    }));
    QTRY_COMPARE(client.processes().rows().size(), 1);
    QCOMPARE(client.qualityNotes().size(), 3);
    QVERIFY(client.incomplete());
    QCOMPARE(client.processes().row(7, 5).session.rxBytes, quint64(0));

    // A following clean frame keeps the marker: already-lost events cannot be
    // recovered, and the client must not claim complete observation again.
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 5, u"curl"_s, 30, 20}}));
    QTRY_COMPARE(client.processes().row(7, 5).session.rxBytes, quint64(20));
    QVERIFY(client.incomplete());
}

void TestSocktrail::rejectedFrameLeavesStateUntouched() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 500}}));
    QTRY_COMPARE(client.processes().rows().size(), 1);
    const QList<ProcessTraffic> before = client.processes().rows();

    // A stats frame whose last process is broken must not contribute any of its
    // earlier processes either.
    send(statsFrame([](ball::test::Writer& writer) {
        msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
        packer.pack_map(7);
        writer.key(QLatin1StringView("at"));
        writer.timestamp(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate));
        writer.key(QLatin1StringView("process_io"));
        packer.pack_array(2);
        writeProcess(writer, 9, 1, u"evil"_s, 5, 5);
        // rx_bytes as a negative integer: refused by the decoder.
        packer.pack_map(4);
        writer.key(QLatin1StringView("pid"));
        packer.pack(8);
        writer.key(QLatin1StringView("start_ns"));
        packer.pack(uint64_t(2));
        writer.key(QLatin1StringView("rx_bytes"));
        packer.pack(-3);
        writer.key(QLatin1StringView("tx_bytes"));
        packer.pack(uint64_t(0));
        writeQuality(writer);
    }));
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
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());
    QTest::qWait(30);
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'500, 0}}));
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
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 2'000, 0}}));
    QTRY_VERIFY(client.available());
    QVERIFY(client.incomplete());
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(1'000));
}

void TestSocktrail::reconnectAfterBackendRestartRebaselines() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());
    QTest::qWait(30);
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'500, 0}}));
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(500));

    m_client->close();
    QTRY_VERIFY(!client.available());
    QVERIFY(acceptConnection(4000));

    // A different `started` means the backend restarted with fresh counters:
    // the old totals are kept, but the new counters only rebaseline.
    send(helloLine(QDateTime::fromString(u"2026-10-13T15:00:00Z"_s, Qt::ISODate)));
    send(statsLine(QDateTime::fromString(u"2026-10-13T15:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 800, 0}}));
    QTRY_VERIFY(client.available());
    QVERIFY(client.incomplete());
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(500));
}

void TestSocktrail::staleAfterMissingStats() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
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
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:10Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'200, 0}}));
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
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1, 1}}));
    QTRY_VERIFY(client.available());

    // A stats frame with a broken process row drops the connection.
    send(statsFrame([](ball::test::Writer& writer) {
        msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
        packer.pack_map(7);
        writer.key(QLatin1StringView("at"));
        writer.timestamp(QDateTime::currentDateTimeUtc());
        writer.key(QLatin1StringView("process_io"));
        packer.pack_array(1);
        writeProcess(writer, 0, 1, u"nobody"_s, 1, 1); // PID 0 names no process
        writeQuality(writer);
    }));
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
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());
    QVERIFY(client.error().isEmpty());
}

void TestSocktrail::oversizedFrameIsDropped() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());

    // A frame header that claims far more than one frame may hold is refused
    // before the decoder allocates for it, so a peer cannot exhaust memory.
    const QByteArray huge = QByteArray::fromHex("db7fffffff"); // str with 2^31-1 bytes
    m_client->write(huge);
    m_client->flush();
    QTRY_VERIFY(!client.error().isEmpty());
    QVERIFY(!client.available());
}

void TestSocktrail::stopStopsReconnecting() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
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

void TestSocktrail::protocolReasonSurvivesTransportCleanupAndReconnect() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection() != nullptr);
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"app"_s, 100, 200}}));
    QTRY_VERIFY(client.available());

    // Garbage bytes cannot be resynchronized: MessagePack has no marker to scan
    // for, so the connection is dropped with the parser's reason.
    send(QByteArray::fromHex("c1c1c1"));
    QTRY_VERIFY(!client.available());
    QVERIFY(!client.error().isEmpty());
    const QByteArray reason = client.error();
    QVERIFY(acceptConnection(4000) != nullptr);
    QCOMPARE(client.error(), reason);
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 500, u"app"_s, 200, 300}}));
    QTRY_VERIFY(client.available());
    QVERIFY(client.error().isEmpty());
    m_client->close();
    QTRY_VERIFY(!client.available());
    QCOMPARE(client.error(), QByteArrayLiteral("collector connection closed"));
}

QTEST_GUILESS_MAIN(TestSocktrail)
#include "socktrail.moc"
