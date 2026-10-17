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

#include <cmath>
#include <initializer_list>
#include <limits>

using ball::ConnectionSnapshot;
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
// `ktlsStatus` is appended to the pid probe when non-empty, mirroring the
// optional upstream probes.ktls object.
void writeQuality(ball::test::Writer& writer, quint64 kernelLost = 0, quint64 dropped = 0,
                  quint64 invalid = 0, quint64 expiredCount = 0, quint64 unindexedRx = 0,
                  quint64 unindexedTx = 0, quint64 unmatchedBytes = 0,
                  const QString& ktlsStatus = QString()) {
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
    packer.pack_map(ktlsStatus.isEmpty() ? 1 : 2);
    writer.key(QLatin1StringView("pid"));
    packer.pack_map(3);
    writer.key(QLatin1StringView("kernel_lost"));
    packer.pack(kernelLost);
    writer.key(QLatin1StringView("dropped"));
    packer.pack(dropped);
    writer.key(QLatin1StringView("invalid"));
    packer.pack(invalid);
    if (!ktlsStatus.isEmpty()) {
        writer.key(QLatin1StringView("ktls"));
        packer.pack_map(1);
        writer.key(QLatin1StringView("status"));
        writer.text(ktlsStatus);
    }
}

// The collector-run IP totals a healthy fixture reports. The real publisher
// writes both fields unconditionally and they are unrelated to any process
// socket I/O, so the numbers are deliberately not derivable from the table.
constexpr quint64 kFixtureIpPackets = 2'450'000;
constexpr quint64 kFixtureIpBytes = 3'892'314'112ULL;

// A healthy stats frame, including the two optional IP totals. Both are
// optional on the wire: the decoder reads a missing key as zero, which the
// tests exercise by erasing it from this otherwise real frame.
QByteArray statsLine(const QDateTime& at, const QList<Process>& processes,
                     quint64 ipPackets = kFixtureIpPackets, quint64 ipBytes = kFixtureIpBytes) {
    return ball::test::envelope(u"stats"_s, QLatin1StringView("stats"),
                                [&](ball::test::Writer& writer) {
                                    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                                    packer.pack_map(9);
                                    writer.key(QLatin1StringView("at"));
                                    writer.timestamp(at);
                                    writer.key(QLatin1StringView("process_io"));
                                    packer.pack_array(uint32_t(processes.size()));
                                    for (const Process& process : processes) {
                                        writeProcess(writer, process.pid, process.startNs,
                                                     process.name, process.rx, process.tx);
                                    }
                                    writer.ipCounters(ipPackets, ipBytes);
                                    writeQuality(writer);
                                });
}

// The real fixture with the named IP fields erased. A nil value is a different
// case (a present but wrong type, which must be refused), so the tests need a
// way to remove a key rather than blank it.
QByteArray withoutIp(const QDateTime& at, std::initializer_list<QLatin1StringView> names) {
    return ball::test::patchFrame(
        statsLine(at, {}), [names](msgpack::object& root, msgpack::zone&) {
            msgpack::object* stats = ball::test::member(root, QLatin1StringView("stats"));
            QVERIFY(stats != nullptr);
            for (const QLatin1StringView name : names) {
                QVERIFY(ball::test::removeMember(*stats, name));
            }
        });
}

// A stats frame whose payload is built by the test, for the malformed cases.
QByteArray statsFrame(const std::function<void(ball::test::Writer&)>& fill) {
    return ball::test::envelope(u"stats"_s, QLatin1StringView("stats"), fill);
}

// A stats frame whose named IP field is written by the test, so every accepted
// and refused encoding is exercised without touching the other field or the
// rest of the frame. The other field keeps a legal count: writing the same
// illegal value into both would only ever exercise the first rejection.
QByteArray statsWithIp(const QDateTime& at, QLatin1StringView name,
                       const std::function<void(ball::test::Writer&)>& ipValue) {
    const bool packets = name == QLatin1StringView("ip_packets");
    return statsFrame([&](ball::test::Writer& writer) {
        msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
        packer.pack_map(9);
        writer.key(QLatin1StringView("at"));
        writer.timestamp(at);
        writer.key(QLatin1StringView("process_io"));
        packer.pack_array(0);
        writer.key(QLatin1StringView("ip_packets"));
        if (packets) {
            ipValue(writer);
        } else {
            packer.pack(uint64_t(1));
        }
        writer.key(QLatin1StringView("ip_bytes"));
        if (packets) {
            packer.pack(uint64_t(1));
        } else {
            ipValue(writer);
        }
        writeQuality(writer);
    });
}

QByteArray helloLine(const QDateTime& started = kStarted, int version = 1,
                     const QStringList& kinds = {}) {
    return ball::test::envelope(u"hello"_s, QLatin1StringView("hello"),
                                [&](ball::test::Writer& writer) {
                                    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                                    packer.pack_map(kinds.isEmpty() ? 2 : 3);
                                    writer.key(QLatin1StringView("version"));
                                    packer.pack(version);
                                    writer.key(QLatin1StringView("started"));
                                    writer.timestamp(started);
                                    if (!kinds.isEmpty()) {
                                        // The hello declares what the socket
                                        // publishes, so a subscriber need not
                                        // wait for a flow frame to learn it.
                                        writer.key(QLatin1StringView("kinds"));
                                        packer.pack_array(uint32_t(kinds.size()));
                                        for (const QString& kind : kinds) {
                                            writer.text(kind);
                                        }
                                    }
                                });
}

// A complete flow frame with the required fields a real connection row always
// carries. The counters are cumulative since first observation, so a later
// frame with the same ID must replace them, never add up. `padding` rides in an
// unrelated field that the decoder ignores, which keeps the frame's size under
// the test's control.
QByteArray flowLine(quint64 id = 1, quint64 rxBytes = 1'000'000, quint64 txBytes = 2'000'000,
                    const QString& padding = QString()) {
    const QDateTime first = kStarted;
    const QDateTime last = kStarted.addSecs(5);
    return ball::test::envelope(u"flow"_s, QLatin1StringView("flow"),
                                [&](ball::test::Writer& writer) {
                                    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                                    packer.pack_map(padding.isEmpty() ? 9 : 10);
                                    writer.key(QLatin1StringView("id"));
                                    packer.pack(id);
                                    writer.key(QLatin1StringView("protocol"));
                                    writer.text(u"TCP"_s);
                                    writer.key(QLatin1StringView("source"));
                                    writer.text(u"10.0.0.2:40000"_s);
                                    writer.key(QLatin1StringView("target"));
                                    writer.text(u"10.0.0.5:443"_s);
                                    writer.key(QLatin1StringView("first_seen"));
                                    writer.timestamp(first);
                                    writer.key(QLatin1StringView("last_seen"));
                                    writer.timestamp(last);
                                    writer.key(QLatin1StringView("rx_bytes"));
                                    packer.pack(rxBytes);
                                    writer.key(QLatin1StringView("tx_bytes"));
                                    packer.pack(txBytes);
                                    writer.key(QLatin1StringView("packets"));
                                    packer.pack(rxBytes + txBytes);
                                    if (!padding.isEmpty()) {
                                        // An unknown key the decoder ignores,
                                        // so a large frame stays legal and
                                        // still spans several reads.
                                        writer.key(QLatin1StringView("pad"));
                                        writer.text(padding);
                                    }
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
    void ipCountersAreOptionalAndExact();
    void helloThenFirstStats();
    void statsBeforeHelloIsRejected();
    void accumulatedStatsAcrossSplitFrames();
    void connectionCacheIsReplacedAndSampled();
    void flowBeforeHelloIsRejected();
    void malformedFlowIsRejected();
    void helloResetRebuildsConnectionCache();
    void flowAloneIsNotFreshStats();
    void ipCountersFollowTheLatestStatsFrame();
    void badFrameDoesNotPartiallyUpdateIpCounters();
    void collectorRestartOverwritesIpCounters();
    void sameMillisecondStatsAreNotLost();
    void newerStatsSupersedePendingStats();
    void firstConnectionFailureIsNotHistoricalGap();
    void qualityNotesMakeObservationIncomplete();
    void diagnosticsAreNotDataLoss();
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

    // A flow frame is a legal object that carries exactly one connection row.
    const SocktrailFrame flow = decode(flowLine());
    QCOMPARE(flow.status, SocktrailFrameStatus::Flow);
    QCOMPARE(flow.connection.id, quint64(1));
    QCOMPARE(flow.connection.protocol, u"TCP"_s);
    QCOMPARE(flow.connection.source, u"10.0.0.2:40000"_s);
    QCOMPARE(flow.connection.target, u"10.0.0.5:443"_s);
    QCOMPARE(flow.connection.rxBytes, quint64(1'000'000));
    QCOMPARE(flow.connection.txBytes, quint64(2'000'000));
    QCOMPARE(flow.connection.packets, quint64(3'000'000));
    QVERIFY(flow.connection.firstSeen.isValid());
    QVERIFY(flow.connection.lastSeen.isValid());

    // A hello that declares flow makes the connection section available
    // without waiting for the first frame.
    const SocktrailFrame advertised =
        decode(helloLine(kStarted, 1, {QStringLiteral("stats"), QStringLiteral("flow")}));
    QCOMPARE(advertised.status, SocktrailFrameStatus::Hello);
    QVERIFY(advertised.connectionsAdvertised);
    QVERIFY(!decode(helloLine()).connectionsAdvertised);
    // A non-array `kinds` is malformed, not merely empty.
    QCOMPARE(decode(ball::test::envelope(u"hello"_s, QLatin1StringView("hello"),
                                         [](ball::test::Writer& writer) {
                                             msgpack::packer<msgpack::sbuffer>& packer =
                                                 writer.packer();
                                             packer.pack_map(3);
                                             writer.key(QLatin1StringView("version"));
                                             packer.pack(1);
                                             writer.key(QLatin1StringView("started"));
                                             writer.timestamp(kStarted);
                                             writer.key(QLatin1StringView("kinds"));
                                             packer.pack_map(0);
                                         }))
                 .status,
             SocktrailFrameStatus::ProtocolError);

    // The fields the connection page depends on are required on a flow frame,
    // even though the snapshot decoder treats them as optional.
    const auto missingFlowField = [](QLatin1StringView key) {
        return ball::test::patchFrame(flowLine(), [key](msgpack::object& root, msgpack::zone&) {
            msgpack::object* flow = ball::test::member(root, QLatin1StringView("flow"));
            QVERIFY(flow != nullptr);
            QVERIFY(ball::test::removeMember(*flow, key));
        });
    };
    for (const QLatin1StringView key :
         {QLatin1StringView("protocol"), QLatin1StringView("source"), QLatin1StringView("target"),
          QLatin1StringView("first_seen"), QLatin1StringView("last_seen"),
          QLatin1StringView("rx_bytes"), QLatin1StringView("tx_bytes"),
          QLatin1StringView("packets")}) {
        QCOMPARE(decode(missingFlowField(key)).status, SocktrailFrameStatus::ProtocolError);
    }
    // A zero ID is not a connection identity, and a broken counter rejects the
    // row whole rather than partially filling it.
    QCOMPARE(decode(flowLine(0)).status, SocktrailFrameStatus::ProtocolError);
    QCOMPARE(decode(ball::test::patchFrame(flowLine(),
                                           [](msgpack::object& root, msgpack::zone&) {
                                               msgpack::object* flow = ball::test::member(
                                                   root, QLatin1StringView("flow"));
                                               QVERIFY(flow != nullptr);
                                               msgpack::object* rx = ball::test::member(
                                                   *flow, QLatin1StringView("rx_bytes"));
                                               QVERIFY(rx != nullptr);
                                               rx->type = msgpack::type::NEGATIVE_INTEGER;
                                               rx->via.i64 = -1;
                                           }))
                 .status,
             SocktrailFrameStatus::ProtocolError);
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

void TestSocktrail::ipCountersAreOptionalAndExact() {
    const QDateTime at = QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate);

    // Both fields are read as written, unrelated to any process row: socket I/O
    // and the collector's IP packet count are different things and are never
    // added together.
    const SocktrailFrame present = decode(statsLine(at, {Process{7, 500, u"curl"_s, 1, 2}}));
    QCOMPARE(present.status, SocktrailFrameStatus::Ok);
    QCOMPARE(present.ipPackets, kFixtureIpPackets);
    QCOMPARE(present.ipBytes, kFixtureIpBytes);

    // 2^53 + 1 and UINT64_MAX survive the trip: both counters are integers on
    // the wire, so nothing may round them through a double.
    const quint64 maximum = std::numeric_limits<quint64>::max();
    const SocktrailFrame large = decode(statsLine(at, {}, 9'007'199'254'740'993ULL, maximum));
    QCOMPARE(large.status, SocktrailFrameStatus::Ok);
    QCOMPARE(large.ipPackets, quint64(9'007'199'254'740'993ULL));
    QCOMPARE(large.ipBytes, maximum);

    // A missing field is zero rather than a rejected frame: both fields are
    // optional on the wire, so an absent one simply means no total.
    const SocktrailFrame noPackets = decode(withoutIp(at, {QLatin1StringView("ip_packets")}));
    QCOMPARE(noPackets.status, SocktrailFrameStatus::Ok);
    QCOMPARE(noPackets.ipPackets, quint64(0));
    QCOMPARE(noPackets.ipBytes, kFixtureIpBytes);
    const SocktrailFrame noBytes = decode(withoutIp(at, {QLatin1StringView("ip_bytes")}));
    QCOMPARE(noBytes.status, SocktrailFrameStatus::Ok);
    QCOMPARE(noBytes.ipPackets, kFixtureIpPackets);
    QCOMPARE(noBytes.ipBytes, quint64(0));
    const SocktrailFrame neither =
        decode(withoutIp(at, {QLatin1StringView("ip_packets"), QLatin1StringView("ip_bytes")}));
    QCOMPARE(neither.status, SocktrailFrameStatus::Ok);
    QCOMPARE(neither.ipPackets, quint64(0));
    QCOMPARE(neither.ipBytes, quint64(0));

    // Every other encoding is refused as a whole frame, so a counter can never
    // be silently rounded, saturated or read out of the wrong type. Each field
    // is exercised on its own, with the other one left legal.
    const auto rejected = [&at](QLatin1StringView name,
                                const std::function<void(ball::test::Writer&)>& value) {
        return decode(statsWithIp(at, name, value)).status == SocktrailFrameStatus::ProtocolError;
    };
    for (const QLatin1StringView name :
         {QLatin1StringView("ip_packets"), QLatin1StringView("ip_bytes")}) {
        QVERIFY(rejected(name, [](ball::test::Writer& writer) { writer.packer().pack(-1); }));
        QVERIFY(rejected(name, [](ball::test::Writer& writer) { writer.packer().pack(1.5); }));
        QVERIFY(
            rejected(name, [](ball::test::Writer& writer) { writer.packer().pack(std::nan("")); }));
        QVERIFY(rejected(name, [](ball::test::Writer& writer) {
            writer.packer().pack(std::numeric_limits<double>::infinity());
        }));
        QVERIFY(rejected(name, [](ball::test::Writer& writer) {
            writer.packer().pack(18'446'744'073'709'551'616.0);
        }));
        QVERIFY(rejected(name, [](ball::test::Writer& writer) { writer.text(u"1234"_s); }));
        QVERIFY(rejected(name, [](ball::test::Writer& writer) { writer.packer().pack(true); }));
        QVERIFY(rejected(name, [](ball::test::Writer& writer) { writer.packer().pack_nil(); }));
    }
    // An integral float is exactly representable, so it is a legal count for
    // either field while the other one keeps its own legal value.
    const auto asFloat = [&at](QLatin1StringView name, quint64 expectedPackets,
                               quint64 expectedBytes) {
        const SocktrailFrame frame = decode(
            statsWithIp(at, name, [](ball::test::Writer& writer) { writer.packer().pack(2.0); }));
        QCOMPARE(frame.status, SocktrailFrameStatus::Ok);
        QCOMPARE(frame.ipPackets, expectedPackets);
        QCOMPARE(frame.ipBytes, expectedBytes);
    };
    asFloat(QLatin1StringView("ip_packets"), quint64(2), quint64(1));
    asFloat(QLatin1StringView("ip_bytes"), quint64(1), quint64(2));
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

void TestSocktrail::connectionCacheIsReplacedAndSampled() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    QVERIFY(!client.connectionsAvailable());
    send(helloLine(kStarted, 1, {QStringLiteral("stats"), QStringLiteral("flow")}));
    QTRY_VERIFY(client.connectionsAvailable());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());

    // The first flow frame only records a baseline once the next stats frame
    // samples it: no rate yet.
    send(flowLine(1, 1'000, 2'000));
    QTRY_COMPARE(client.connections().rows().size(), 1);
    ConnectionSnapshot row = client.connections().rows().first();
    QCOMPARE(row.id, quint64(1));
    QCOMPARE(row.rxBytes, quint64(1'000));
    QCOMPARE(row.rxBps, 0.0);

    // Flow frames never feed the process totals or add an identity there.
    QCOMPARE(client.processes().rows().size(), 1);
    QCOMPARE(client.processes().row(99, 1).present, false);

    // A later frame with the same ID and larger cumulative counters replaces
    // the row, never adds to it, and the next stats interval samples a rate.
    QTest::qWait(30);
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'100, 0}}));
    QTRY_COMPARE(client.connections().rows().first().rxBps, 0.0); // baseline
    send(flowLine(1, 3'000, 2'000));
    QTRY_COMPARE(client.connections().rows().first().rxBytes, quint64(3'000));
    QTest::qWait(30);
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'200, 0}}));
    QTRY_VERIFY(client.connections().rows().first().rxBps > 0.0);
    row = client.connections().rows().first();
    QCOMPARE(row.rxBytes, quint64(3'000));
}

void TestSocktrail::flowBeforeHelloIsRejected() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());

    // A flow frame may not identify the session before the hello does.
    send(flowLine());
    QTRY_VERIFY(!client.error().isEmpty());
    QVERIFY(client.connections().rows().isEmpty());
    QVERIFY(!client.connectionsAvailable());
}

void TestSocktrail::malformedFlowIsRejected() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine(kStarted, 1, {QStringLiteral("flow")}));
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());
    send(flowLine(1, 10, 20));
    QTRY_COMPARE(client.connections().rows().size(), 1);

    // A flow frame whose required counter is missing drops the connection
    // instead of silently dropping the row.
    send(ball::test::patchFrame(flowLine(2, 30, 40), [](msgpack::object& root, msgpack::zone&) {
        msgpack::object* flow = ball::test::member(root, QLatin1StringView("flow"));
        QVERIFY(flow != nullptr);
        QVERIFY(ball::test::removeMember(*flow, QLatin1StringView("rx_bytes")));
    }));
    QTRY_VERIFY(!client.available());
    QVERIFY(!client.error().isEmpty());
    // The real cache is cleared with the connection.
    QVERIFY(client.connections().rows().isEmpty());
}

void TestSocktrail::helloResetRebuildsConnectionCache() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine(kStarted, 1, {QStringLiteral("flow")}));
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());
    send(flowLine(1, 10, 20));
    QTRY_COMPARE(client.connections().rows().size(), 1);

    // A reconnect to the same collector restarts with a new hello. The cache is
    // rebuilt because the disconnect may have hidden an `end` or an ID reuse.
    m_client->close();
    QTRY_VERIFY(!client.connectionsAvailable());
    QVERIFY(client.connections().rows().isEmpty());
    QVERIFY(acceptConnection(4000));
    send(helloLine(kStarted, 1, {QStringLiteral("flow")}));
    QTRY_VERIFY(client.connectionsAvailable());
    QVERIFY(client.connections().rows().isEmpty());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'500, 0}}));
    QTRY_VERIFY(client.available());
    send(flowLine(1, 50, 60));
    QTRY_COMPARE(client.connections().rows().size(), 1);
    QCOMPARE(client.connections().rows().first().rxBytes, quint64(50));
}

void TestSocktrail::flowAloneIsNotFreshStats() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    // No `kinds`: a legal flow frame is the first proof the collector supports
    // connections.
    send(helloLine());
    QVERIFY(!client.connectionsAvailable());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}));
    QTRY_VERIFY(client.available());
    send(flowLine());
    QTRY_VERIFY(client.connectionsAvailable());

    // A flow frame must not restart the freshness timer, so the stats window
    // still expires.
    QTest::qWait(Socktrail::kStatsTimeoutMs + 300);
    QVERIFY(client.stale());
    QVERIFY(!client.error().isEmpty());
    // And it must not have kept the process totals fresh either.
    QVERIFY(client.available());
}

void TestSocktrail::ipCountersFollowTheLatestStatsFrame() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}, 2'500, 300));
    QTRY_VERIFY(client.available());
    QCOMPARE(client.ipPackets(), quint64(2'500));
    QCOMPARE(client.ipBytes(), quint64(300));

    // The collector reports its own running totals, so a later frame is the new
    // value, never an increment added on top: the smaller numbers below replace
    // the larger ones instead of adding to them. A collector restart between
    // the two frames is exactly when that happens. The per-process totals are a
    // separate accumulator and keep growing.
    QTest::qWait(30);
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'400, 0}}, 10, 100));
    QTRY_COMPARE(client.ipPackets(), quint64(10));
    QCOMPARE(client.ipBytes(), quint64(100));
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(400));

    // A frame that omits both fields overwrites them with zero; an absent field
    // is not "keep the previous value".
    QTest::qWait(30);
    send(withoutIp(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                   {QLatin1StringView("ip_packets"), QLatin1StringView("ip_bytes")}));
    QTRY_COMPARE(client.ipPackets(), quint64(0));
    QCOMPARE(client.ipBytes(), quint64(0));
    QVERIFY(client.available());
}

void TestSocktrail::badFrameDoesNotPartiallyUpdateIpCounters() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}, 10, 100));
    QTRY_VERIFY(client.available());

    // The IP fields are valid but a process row is not. A rejected frame leaves
    // no partial state, so the decoded IP totals must not reach the client.
    send(statsFrame([](ball::test::Writer& writer) {
        msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
        packer.pack_map(9);
        writer.key(QLatin1StringView("at"));
        writer.timestamp(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate));
        writer.key(QLatin1StringView("process_io"));
        packer.pack_array(1);
        writeProcess(writer, 0, 1, u"nobody"_s, 1, 1); // PID 0 names no process
        writer.ipCounters(9'999'999, 9'999'999'999ULL);
        writeQuality(writer);
    }));
    QTRY_VERIFY(!client.available());
    QVERIFY(!client.error().isEmpty());
    QCOMPARE(client.ipPackets(), quint64(10));
    QCOMPARE(client.ipBytes(), quint64(100));
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(0));

    // The same holds when only one of the two IP fields is the wrong type: the
    // good field is refused with it, and the process table is not applied.
    QVERIFY(acceptConnection(4000));
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}, 20, 200));
    QTRY_COMPARE(client.ipPackets(), quint64(20));
    QTest::qWait(30);
    send(statsFrame([](ball::test::Writer& writer) {
        msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
        packer.pack_map(9);
        writer.key(QLatin1StringView("at"));
        writer.timestamp(QDateTime::fromString(u"2026-10-13T14:00:04Z"_s, Qt::ISODate));
        writer.key(QLatin1StringView("process_io"));
        packer.pack_array(0);
        writer.key(QLatin1StringView("ip_packets"));
        packer.pack(uint64_t(99));
        writer.key(QLatin1StringView("ip_bytes"));
        packer.pack(1.5); // fractional, so the frame is refused whole
        writeQuality(writer);
    }));
    QTRY_VERIFY(!client.available());
    QCOMPARE(client.ipPackets(), quint64(20));
    QCOMPARE(client.ipBytes(), quint64(200));
}

void TestSocktrail::collectorRestartOverwritesIpCounters() {
    Socktrail client;
    client.start(startServer());
    QVERIFY(acceptConnection());
    send(helloLine());
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'000, 0}}, 10, 100));
    QTRY_VERIFY(client.available());
    QTest::qWait(30);
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 1'500, 0}}, 60, 600));
    QTRY_COMPARE(client.ipPackets(), quint64(60));
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(500));

    m_client->close();
    QTRY_VERIFY(!client.available());
    QVERIFY(acceptConnection(4000));

    // A different `started` means the collector restarted: its counters start
    // from zero again, so the smaller number is the new total and not a delta
    // to be added. The process observation keeps what it already accumulated.
    send(helloLine(QDateTime::fromString(u"2026-10-13T15:00:00Z"_s, Qt::ISODate)));
    send(statsLine(QDateTime::fromString(u"2026-10-13T15:00:01Z"_s, Qt::ISODate),
                   {Process{7, 500, u"curl"_s, 800, 0}}, 3, 30));
    QTRY_COMPARE(client.ipPackets(), quint64(3));
    QCOMPARE(client.ipBytes(), quint64(30));
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(500));

    // A new subscription drops the collector-run totals: they were the previous
    // collector's numbers, and the next frame is an absolute value rather than
    // an increment either way.
    client.start(startServer());
    QCOMPARE(client.ipPackets(), quint64(0));
    QCOMPARE(client.ipBytes(), quint64(0));
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
    // its millisecond with the previous one, and the IP totals follow it: the
    // deferred frame carries both.
    m_client->write(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                              {Process{7, 500, u"curl"_s, 1'400, 0}}, 40, 400) +
                    statsLine(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                              {Process{7, 500, u"curl"_s, 1'700, 0}}, 70, 700));
    m_client->flush();
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(700));
    QCOMPARE(client.ipPackets(), quint64(70));
    QCOMPARE(client.ipBytes(), quint64(700));
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
    // several reads; the newest pending sample must still be the one applied,
    // together with its IP totals.
    QByteArray batch = statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                                 {Process{7, 500, u"app"_s, 1'400, 0}}, 40, 400);
    batch += statsLine(QDateTime::fromString(u"2026-10-13T14:00:03Z"_s, Qt::ISODate),
                       {Process{7, 500, u"app"_s, 2'100, 0}}, 70, 700);
    batch += flowLine(5, 1'000, 2'000, QString(3LL * 1024 * 1024, u'x'));
    batch += statsLine(QDateTime::fromString(u"2026-10-13T14:00:04Z"_s, Qt::ISODate),
                       {Process{7, 500, u"app"_s, 2'600, 0}}, 90, 900);
    send(batch);
    QTRY_COMPARE(client.processes().row(7, 500).session.rxBytes, quint64(1'600));
    QCOMPARE(client.ipPackets(), quint64(90));
    QCOMPARE(client.ipBytes(), quint64(900));
    QTest::qWait(20);
    QCOMPARE(client.processes().row(7, 500).session.rxBytes, quint64(1'600));
    QCOMPARE(client.ipPackets(), quint64(90));
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
    // Real losses (pipcked events, unindexed bytes) make the observation
    // incomplete; explanations (expired IDs, unmatched I/O) do not.
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
    // PID losses and unindexed bytes are gaps in the process totals.
    QCOMPARE(frame.qualityNotes.size(), 2);
    QVERIFY(frame.qualityNotes.first().contains(u"PID events"_s));
    // Expired identities and unmatched socket I/O explain the table without
    // claiming it undercounts.
    QCOMPARE(frame.infoNotes.size(), 2);
    QVERIFY(frame.infoNotes.first().contains(u"expired PIDs"_s));
    QVERIFY(frame.infoNotes.last().contains(u"unmatched socket I/O"_s));
    QVERIFY(frame.infoNotes.last().contains(u"7777"_s));

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
    QCOMPARE(client.qualityNotes().size(), 2);
    QVERIFY(client.incomplete());
    QCOMPARE(client.processes().row(7, 5).session.rxBytes, quint64(0));

    // A following clean frame keeps the marker: already-lost events cannot be
    // recovered, and the client must not claim complete observation again.
    send(statsLine(QDateTime::fromString(u"2026-10-13T14:00:02Z"_s, Qt::ISODate),
                   {Process{7, 5, u"curl"_s, 30, 20}}));
    QTRY_COMPARE(client.processes().row(7, 5).session.rxBytes, quint64(20));
    QVERIFY(client.incomplete());
}

void TestSocktrail::diagnosticsAreNotDataLoss() {
    // Expired PIDs and unmatched socket I/O explain where bytes went; they are
    // not a gap in the process table, so a stats-only stream stays complete.
    const SocktrailFrame frame = decode(statsFrame([](ball::test::Writer& writer) {
        msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
        packer.pack_map(7);
        writer.key(QLatin1StringView("at"));
        writer.timestamp(QDateTime::currentDateTimeUtc());
        writer.key(QLatin1StringView("process_io"));
        packer.pack_array(1);
        writeProcess(writer, 7, 5, u"curl"_s, 10, 20);
        writeQuality(writer, 0, 0, 0, 3, 0, 0, 4'096);
    }));
    QCOMPARE(frame.status, SocktrailFrameStatus::Ok);
    QVERIFY(frame.qualityNotes.isEmpty());
    QCOMPARE(frame.infoNotes.size(), 2);

    // The kTLS probe status is an explanation too, and a missing probes.ktls
    // object or an "active" status adds nothing.
    const auto withKtls = [](const QString& status) {
        return decode(statsFrame([&](ball::test::Writer& writer) {
            msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
            packer.pack_map(7);
            writer.key(QLatin1StringView("at"));
            writer.timestamp(QDateTime::currentDateTimeUtc());
            writer.key(QLatin1StringView("process_io"));
            packer.pack_array(0);
            writeQuality(writer, 0, 0, 0, 0, 0, 0, 0, status);
        }));
    };
    QVERIFY(withKtls(QStringLiteral("active")).infoNotes.isEmpty());
    QVERIFY(withKtls(QStringLiteral("missing")).infoNotes.last().contains(u"kTLS"_s));
    QVERIFY(withKtls(QStringLiteral("missing")).infoNotes.last().contains(u"missing"_s));

    // A diagnostics string may legally contain newlines; it must be normalized
    // to a single safe line rather than reject the whole stats frame.
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
        packer.pack_array(0);
        writeQuality(writer, 0, 0, 0, 0, 0, 0, 0, QStringLiteral("missing\nreason\x1b[31m"));
    }));
    QTRY_VERIFY(client.available());
    QVERIFY(!client.stale());
    QCOMPARE(client.infoNotes().size(), 1);
    for (const QChar character : client.infoNotes().first()) {
        QVERIFY(character.unicode() >= 0x20 && character.unicode() != 0x7f);
    }
    QVERIFY(client.infoNotes().first().contains(u"reason"_s));
    QVERIFY(!client.infoNotes().first().contains(u"\n"_s));
    // While a real gap still makes the observation incomplete and live.
    QVERIFY(!client.incomplete());
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
