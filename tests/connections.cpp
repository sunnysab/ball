// ball: a network traffic HUD for KDE Wayland sessions.
// Protocol tests for the connection snapshot section: the encoder and the
// decoder must agree on every field, refuse a frame that breaks the schema and
// leave a frame without the section readable. No sockets and no display.
#include "snapshot.h"

#include "messagepack.h"
#include "mp_frames.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTest>

#include <cmath>
#include <functional>
#include <limits>

using ball::ConnectionProcess;
using ball::ConnectionSnapshot;
using ball::ConnectionStatus;
using ball::ProcessStatus;
using ball::Snapshot;
using ball::test::member;
using ball::test::patchFrame;
using ball::test::removeMember;
using namespace Qt::StringLiterals;

namespace {

const QDateTime kStarted = QDateTime::fromString(u"2026-10-15T09:40:00+08:00"_s, Qt::ISODate);
const QDateTime kAt = QDateTime::fromString(u"2026-10-15T09:41:07+08:00"_s, Qt::ISODate);

// A baseline snapshot with the pre-existing sections valid, so every rejection
// below is caused by the connection edit and not by the fixture.
Snapshot makeSnapshot() {
    Snapshot snapshot;
    snapshot.version = 1;
    snapshot.started = kStarted;
    snapshot.at = kAt;
    snapshot.interfacesAvailable = true;
    snapshot.processStatus = ProcessStatus{true, false, false, QString(), {u"note"_s}, {}};
    return snapshot;
}

// One rich connection row: every field at a value a real collector can produce.
ConnectionSnapshot makeConnection(quint64 id) {
    ConnectionSnapshot row;
    row.id = id;
    row.protocol = u"tcp"_s;
    row.app = u"firefox"_s;
    row.state = u"established"_s;
    row.direction = u"outbound"_s;
    row.source = u"10.0.0.5:51234"_s;
    row.target = u"93.184.216.34:443"_s;
    row.name = u"example.com"_s;
    row.detail = u"SNI example.com; Host example.com"_s;
    row.end.clear();
    row.interfaces = {u"eno1"_s, u"wg0"_s};
    row.firstSeen = kStarted;
    row.lastSeen = kAt;
    row.rxBytes = 4096;
    row.txBytes = 2048;
    row.packets = 42;
    row.retransmits = 3;
    row.rxBps = 1024.5;
    row.txBps = 256.25;
    row.rttUs = 12500;
    row.rttSource = u"tcp_info"_s;
    row.client = ConnectionProcess{1234, 9'007'199'254'740'993ULL, u"firefox"_s, 4096, 2048};
    // The server side is the peer process when the collector could name it.
    row.server = ConnectionProcess{-1, 1, QString(), 0, 0};
    row.io = {ConnectionProcess{1234, 9'007'199'254'740'993ULL, u"firefox"_s, 4096, 2048},
              ConnectionProcess{987, 7, u"code"_s, 100, 50}};
    return row;
}

QByteArray encode(const Snapshot& snapshot) {
    bool ok = false;
    const QByteArray bytes = ball::encodeSnapshot(snapshot, ok);
    if (!ok) {
        QTest::qFail("the fixture snapshot must be encodable", __FILE__, __LINE__);
        return {};
    }
    return bytes;
}

// Decodes one frame in isolation, exactly as a client does.
ball::SnapshotParseResult decodeFrame(const QByteArray& bytes) {
    ball::mp::Decoder decoder;
    decoder.feed(bytes);
    ball::mp::Frame decoded = decoder.next();
    if (!decoded.valid()) {
        return ball::SnapshotParseResult{std::nullopt, QStringLiteral("not a frame")};
    }
    return ball::decodeSnapshot(decoded.value());
}

Snapshot requireSnapshot(const QByteArray& bytes) {
    const ball::SnapshotParseResult result = decodeFrame(bytes);
    if (!result.snapshot.has_value()) {
        const QString reason = u"unexpected rejection: %1"_s.arg(result.error);
        QTest::qFail(qPrintable(reason), __FILE__, __LINE__);
        return {};
    }
    return *result.snapshot;
}

// A one-row connection frame, for tests that patch or remove a single field.
QByteArray oneConnectionFrame(ConnectionSnapshot row = makeConnection(1)) {
    Snapshot snapshot = makeSnapshot();
    snapshot.connectionStatus = ConnectionStatus{true, false, false, {u"ok"_s}};
    snapshot.connections = {row};
    return encode(snapshot);
}

bool rejects(const QByteArray& bytes) { return !decodeFrame(bytes).snapshot.has_value(); }

} // namespace

class TestConnections : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void roundTripsEveryConnectionField();
    void keepsCountersAndIdentityExact();
    void missingSectionIsUnavailable();
    void absentOptionalRowFieldsDefault();
    void rejectsMalformedConnections();
    void rejectsOversizedCollections();
    void acceptsLongTextAndManyParticipants();
    void rejectsMalformedStatusAndInfo();
    void rejectsOversizedFrame();
};

void TestConnections::roundTripsEveryConnectionField() {
    Snapshot original = makeSnapshot();
    original.connectionStatus = ConnectionStatus{true, false, true, {u"capped at 8192"_s}};
    original.connections = {makeConnection(1), makeConnection(2)};
    original.connections[1].state = u"closed"_s;
    original.connections[1].end = u"timeout"_s;

    const Snapshot decoded = requireSnapshot(encode(original));
    QCOMPARE(decoded.connections.size(), 2);
    QCOMPARE(decoded.connectionStatus.available, true);
    QCOMPARE(decoded.connectionStatus.stale, false);
    QCOMPARE(decoded.connectionStatus.truncated, true);
    QCOMPARE(decoded.connectionStatus.notes, QStringList{u"capped at 8192"_s});

    const ConnectionSnapshot& row = decoded.connections.at(0);
    const ConnectionSnapshot& reference = original.connections.at(0);
    QCOMPARE(row.id, reference.id);
    QCOMPARE(row.protocol, reference.protocol);
    QCOMPARE(row.app, reference.app);
    QCOMPARE(row.state, reference.state);
    QCOMPARE(row.direction, reference.direction);
    QCOMPARE(row.source, reference.source);
    QCOMPARE(row.target, reference.target);
    QCOMPARE(row.name, reference.name);
    QCOMPARE(row.detail, reference.detail);
    QCOMPARE(row.end, reference.end);
    QCOMPARE(row.interfaces, reference.interfaces);
    QCOMPARE(row.firstSeen, reference.firstSeen);
    QCOMPARE(row.lastSeen, reference.lastSeen);
    QCOMPARE(row.rxBytes, reference.rxBytes);
    QCOMPARE(row.txBytes, reference.txBytes);
    QCOMPARE(row.packets, reference.packets);
    QCOMPARE(row.retransmits, reference.retransmits);
    QCOMPARE(row.rxBps, reference.rxBps);
    QCOMPARE(row.txBps, reference.txBps);
    QCOMPARE(row.rttUs, reference.rttUs);
    QCOMPARE(row.rttSource, reference.rttSource);
    QCOMPARE(row.client.pid, reference.client.pid);
    QCOMPARE(row.client.startNs, reference.client.startNs);
    QCOMPARE(row.client.name, reference.client.name);
    QCOMPARE(row.client.rxBytes, reference.client.rxBytes);
    QCOMPARE(row.client.txBytes, reference.client.txBytes);
    QCOMPARE(row.server.pid, -1); // the ambiguous-process sentinel survives
    QCOMPARE(row.server.startNs, quint64(1));
    QCOMPARE(row.io.size(), 2);
    QCOMPARE(row.io.at(0).pid, 1234);
    QCOMPARE(row.io.at(0).startNs, quint64(9'007'199'254'740'993ULL));
    QCOMPARE(row.io.at(1).name, u"code"_s);
    QCOMPARE(row.io.at(1).rxBytes, quint64(100));

    // A finished row keeps its end reason and empty fields stay empty.
    QCOMPARE(decoded.connections.at(1).state, u"closed"_s);
    QCOMPARE(decoded.connections.at(1).end, u"timeout"_s);
    QCOMPARE(decoded.connections.at(0).end, QString());

    // The pre-existing sections are untouched by the new one.
    QCOMPARE(decoded.processStatus.available, true);
    QCOMPARE(decoded.processStatus.notes, QStringList{u"note"_s});
    QCOMPARE(decoded.started, kStarted);
    QCOMPARE(decoded.at, kAt);
}

void TestConnections::keepsCountersAndIdentityExact() {
    ConnectionSnapshot row = makeConnection(9'007'199'254'740'993ULL);
    row.rxBytes = std::numeric_limits<quint64>::max();
    row.txBytes = 9'007'199'254'740'993ULL; // 2^53 + 1, unrepresentable as a double
    row.packets = 9'007'199'254'740'993ULL;
    row.retransmits = std::numeric_limits<quint64>::max();
    row.client.pid = 1;
    row.client.startNs = std::numeric_limits<quint64>::max();
    row.client.rxBytes = 9'007'199'254'740'993ULL;
    row.io = {ConnectionProcess{1, 9'007'199'254'740'993ULL, u"init"_s, 9'007'199'254'740'993ULL,
                                std::numeric_limits<quint64>::max()}};

    const Snapshot decoded = requireSnapshot(oneConnectionFrame(row));
    const ConnectionSnapshot& back = decoded.connections.first();
    QCOMPARE(back.id, quint64(9'007'199'254'740'993ULL));
    QCOMPARE(back.rxBytes, std::numeric_limits<quint64>::max());
    QCOMPARE(back.txBytes, quint64(9'007'199'254'740'993ULL));
    QCOMPARE(back.packets, quint64(9'007'199'254'740'993ULL));
    QCOMPARE(back.retransmits, std::numeric_limits<quint64>::max());
    QCOMPARE(back.client.startNs, std::numeric_limits<quint64>::max());
    QCOMPARE(back.client.rxBytes, quint64(9'007'199'254'740'993ULL));
    QCOMPARE(back.io.first().startNs, quint64(9'007'199'254'740'993ULL));
    QCOMPARE(back.io.first().txBytes, std::numeric_limits<quint64>::max());

    // pid -1 and pid 0 are sentinels, not wrap-around counts: they must keep
    // their sign and meaning on the wire.
    ConnectionSnapshot sentinels = makeConnection(5);
    sentinels.client = ConnectionProcess{-1, 0, u"ambiguous"_s, 0, 0};
    sentinels.server = ConnectionProcess{0, 0, QString(), 0, 0};
    const Snapshot decodedSentinels = requireSnapshot(oneConnectionFrame(sentinels));
    QCOMPARE(decodedSentinels.connections.first().client.pid, -1);
    QCOMPARE(decodedSentinels.connections.first().server.pid, 0);
}

void TestConnections::missingSectionIsUnavailable() {
    // The two new top-level keys are absent on an older daemon: the frame is
    // still a valid snapshot, and the section reads as "not supported" rather
    // than as an available empty table.
    const QByteArray bytes =
        patchFrame(oneConnectionFrame(), [](msgpack::object& root, msgpack::zone&) {
            QVERIFY(removeMember(root, QLatin1StringView("connections")));
            QVERIFY(removeMember(root, QLatin1StringView("connection_status")));
        });
    const Snapshot decoded = requireSnapshot(bytes);
    QVERIFY(decoded.connections.isEmpty());
    QCOMPARE(decoded.connectionStatus.available, false);
    QCOMPARE(decoded.connectionStatus.stale, true);
    QCOMPARE(decoded.connectionStatus.truncated, false);
    QVERIFY(decoded.connectionStatus.notes.isEmpty());

    // `info` is likewise newer than the rest of process_status: a frame without
    // it keeps an empty list instead of being refused.
    const QByteArray noInfo =
        patchFrame(oneConnectionFrame(), [](msgpack::object& root, msgpack::zone&) {
            msgpack::object* status = member(root, QLatin1StringView("process_status"));
            QVERIFY(status != nullptr);
            QVERIFY(removeMember(*status, QLatin1StringView("info")));
        });
    const Snapshot decodedNoInfo = requireSnapshot(noInfo);
    QVERIFY(decodedNoInfo.processStatus.info.isEmpty());

    // A present `info` round-trips, and it never turns the process side
    // incomplete on its own.
    Snapshot withInfo = makeSnapshot();
    withInfo.processStatus.info = {u"unmatched socket I/O: 42 bytes"_s, u"kTLS off"_s};
    const Snapshot decodedInfo = requireSnapshot(encode(withInfo));
    QCOMPARE(decodedInfo.processStatus.info.size(), 2);
    QCOMPARE(decodedInfo.processStatus.info.first(), u"unmatched socket I/O: 42 bytes"_s);
    QCOMPARE(decodedInfo.processStatus.incomplete, false);
}

void TestConnections::absentOptionalRowFieldsDefault() {
    // A row may carry only its identity; every other field is optional and
    // defaults to empty/zero rather than making the frame invalid.
    Snapshot snapshot = makeSnapshot();
    snapshot.connectionStatus = ConnectionStatus{true, false, false, {}};
    ConnectionSnapshot bare;
    bare.id = 7;
    snapshot.connections = {bare};

    const Snapshot decoded = requireSnapshot(encode(snapshot));
    const ConnectionSnapshot& row = decoded.connections.first();
    QCOMPARE(row.id, quint64(7));
    QVERIFY(row.protocol.isEmpty());
    QVERIFY(row.name.isEmpty());
    QVERIFY(row.end.isEmpty());
    QVERIFY(row.interfaces.isEmpty());
    QVERIFY(!row.firstSeen.isValid());
    QVERIFY(!row.lastSeen.isValid());
    QCOMPARE(row.rxBytes, quint64(0));
    QCOMPARE(row.rttUs, qint64(0));
    QCOMPARE(row.client.pid, 0);
    QVERIFY(row.io.isEmpty());

    // Removing single optional fields from an otherwise complete row keeps the
    // same defaults.
    const QByteArray pruned =
        patchFrame(oneConnectionFrame(), [](msgpack::object& root, msgpack::zone&) {
            msgpack::object* rows = member(root, QLatin1StringView("connections"));
            QVERIFY(rows != nullptr && rows->via.array.size == 1);
            msgpack::object& row = rows->via.array.ptr[0];
            for (const QLatin1StringView name :
                 {QLatin1StringView("app"), QLatin1StringView("packets"),
                  QLatin1StringView("rtt_us"), QLatin1StringView("client"),
                  QLatin1StringView("first_seen"), QLatin1StringView("interfaces")}) {
                QVERIFY(removeMember(row, name));
            }
        });
    const Snapshot decodedPruned = requireSnapshot(pruned);
    const ConnectionSnapshot& pruned1 = decodedPruned.connections.first();
    QVERIFY(pruned1.app.isEmpty());
    QCOMPARE(pruned1.packets, quint64(0));
    QCOMPARE(pruned1.client.pid, 0);
    QVERIFY(!pruned1.firstSeen.isValid());
    QVERIFY(pruned1.interfaces.isEmpty());
    // The fields that were kept are still there.
    QCOMPARE(pruned1.name, u"example.com"_s);
    QCOMPARE(pruned1.rxBytes, quint64(4096));
}

void TestConnections::rejectsMalformedConnections() {
    // The unpatched frame is accepted, so each rejection below comes from the
    // edit and not from the fixture.
    QVERIFY(decodeFrame(oneConnectionFrame()).snapshot.has_value());

    const auto patchRow = [](const QByteArray& bytes,
                             const std::function<void(msgpack::object&, msgpack::zone&)>& edit) {
        return patchFrame(bytes, [&](msgpack::object& root, msgpack::zone& zone) {
            msgpack::object* rows = member(root, QLatin1StringView("connections"));
            QVERIFY(rows != nullptr && rows->via.array.size >= 1);
            edit(rows->via.array.ptr[0], zone);
        });
    };
    const auto set =
        [&patchRow](QLatin1StringView name,
                    const std::function<void(msgpack::object&, msgpack::zone&)>& value) {
            return patchRow(oneConnectionFrame(), [&](msgpack::object& row, msgpack::zone& zone) {
                msgpack::object* field = member(row, name);
                QVERIFY(field != nullptr);
                value(*field, zone);
            });
        };

    // The ID is the identity: zero is not a connection and a non-integer is not
    // an ID.
    QVERIFY(rejects(patchRow(oneConnectionFrame(), [](msgpack::object& row, msgpack::zone& zone) {
        msgpack::object* id = member(row, QLatin1StringView("id"));
        QVERIFY(id != nullptr);
        *id = msgpack::object(uint64_t(0), zone);
    })));
    QVERIFY(rejects(set(QLatin1StringView("id"), [](msgpack::object& field, msgpack::zone&) {
        field = msgpack::object(); // present, but nil is not a number
    })));
    QVERIFY(rejects(set(QLatin1StringView("id"), [](msgpack::object& field, msgpack::zone& zone) {
        field = msgpack::object(std::string("1"), zone);
    })));

    for (const auto key : {QLatin1StringView("client"), QLatin1StringView("server")}) {
        QVERIFY(rejects(set(
            key, [](msgpack::object& field, msgpack::zone&) { field = msgpack::object(true); })));
    }
    QVERIFY(rejects(set(QLatin1StringView("rtt_us"), [](msgpack::object& field, msgpack::zone&) {
        field = msgpack::object(int64_t(-1));
    })));

    // A duplicate ID is ambiguous: the cache is keyed by it.
    Snapshot duplicate = makeSnapshot();
    duplicate.connectionStatus = ConnectionStatus{true, false, false, {}};
    duplicate.connections = {makeConnection(4), makeConnection(4)};
    QVERIFY(rejects(encode(duplicate)));

    // Counters must be exact and non-negative.
    QVERIFY(rejects(set(QLatin1StringView("rx_bytes"), [](msgpack::object& field, msgpack::zone&) {
        field = msgpack::object(int64_t(-1));
    })));
    QVERIFY(
        rejects(set(QLatin1StringView("tx_bytes"), [](msgpack::object& field, msgpack::zone& zone) {
            field = msgpack::object(1.5, zone);
        })));
    QVERIFY(rejects(set(QLatin1StringView("packets"), [](msgpack::object& field, msgpack::zone&) {
        field = msgpack::object(true);
    })));

    // Rates must be finite and non-negative.
    QVERIFY(rejects(patchRow(oneConnectionFrame(), [](msgpack::object& row, msgpack::zone& zone) {
        msgpack::object* rate = member(row, QLatin1StringView("rate"));
        QVERIFY(rate != nullptr);
        *member(*rate, QLatin1StringView("rx_bps")) = msgpack::object(std::nan(""), zone);
    })));
    QVERIFY(rejects(patchRow(oneConnectionFrame(), [](msgpack::object& row, msgpack::zone& zone) {
        msgpack::object* rate = member(row, QLatin1StringView("rate"));
        QVERIFY(rate != nullptr);
        *member(*rate, QLatin1StringView("tx_bps")) =
            msgpack::object(std::numeric_limits<double>::infinity(), zone);
    })));
    QVERIFY(rejects(patchRow(oneConnectionFrame(), [](msgpack::object& row, msgpack::zone& zone) {
        msgpack::object* rate = member(row, QLatin1StringView("rate"));
        QVERIFY(rate != nullptr);
        *member(*rate, QLatin1StringView("rx_bps")) = msgpack::object(-1.0, zone);
    })));

    // A time must be a MessagePack timestamp, not a plausible-looking string.
    QVERIFY(rejects(
        set(QLatin1StringView("first_seen"), [](msgpack::object& field, msgpack::zone& zone) {
            field = msgpack::object(std::string("2026-10-15T09:40:00"), zone);
        })));
    QVERIFY(rejects(set(QLatin1StringView("last_seen"), [](msgpack::object& field, msgpack::zone&) {
        field = msgpack::object(uint64_t(1'700'000'000));
    })));

    // A measured RTT cannot be fractional.
    QVERIFY(
        rejects(set(QLatin1StringView("rtt_us"), [](msgpack::object& field, msgpack::zone& zone) {
            field = msgpack::object(12.5, zone);
        })));

    // io[] only accepts a positive PID; the 0/-1 sentinels belong to
    // client/server.
    QVERIFY(rejects(patchRow(oneConnectionFrame(), [](msgpack::object& row, msgpack::zone& zone) {
        msgpack::object* io = member(row, QLatin1StringView("io"));
        QVERIFY(io != nullptr && io->via.array.size >= 1);
        *member(io->via.array.ptr[0], QLatin1StringView("pid")) =
            msgpack::object(uint64_t(0), zone);
    })));
    QVERIFY(rejects(patchRow(oneConnectionFrame(), [](msgpack::object& row, msgpack::zone& zone) {
        msgpack::object* io = member(row, QLatin1StringView("io"));
        QVERIFY(io != nullptr);
        msgpack::object* process = member(io->via.array.ptr[0], QLatin1StringView("pid"));
        QVERIFY(process != nullptr);
        *process = msgpack::object(int64_t(-1), zone);
    })));

    // client/server accept 0 and -1 but nothing else.
    QVERIFY(rejects(patchRow(oneConnectionFrame(), [](msgpack::object& row, msgpack::zone& zone) {
        msgpack::object* client = member(row, QLatin1StringView("client"));
        QVERIFY(client != nullptr);
        *member(*client, QLatin1StringView("pid")) = msgpack::object(int64_t(-2), zone);
    })));
    QVERIFY(rejects(patchRow(oneConnectionFrame(), [](msgpack::object& row, msgpack::zone& zone) {
        msgpack::object* server = member(row, QLatin1StringView("server"));
        QVERIFY(server != nullptr);
        *member(*server, QLatin1StringView("pid")) =
            msgpack::object(uint64_t(std::numeric_limits<int>::max()) + 1, zone);
    })));

    // A wrong container type and a control character in a shown string are both
    // refused, because a reader would otherwise have to guess.
    QVERIFY(rejects(
        set(QLatin1StringView("interfaces"), [](msgpack::object& field, msgpack::zone& zone) {
            field = msgpack::object(std::string("eno1"), zone);
        })));
    QVERIFY(rejects(set(QLatin1StringView("name"), [](msgpack::object& field, msgpack::zone& zone) {
        field = msgpack::object(std::string("bad\x1b[31m"), zone);
    })));

    // A row that is not a map at all.
    QVERIFY(
        rejects(patchFrame(oneConnectionFrame(), [](msgpack::object& root, msgpack::zone& zone) {
            msgpack::object* rows = member(root, QLatin1StringView("connections"));
            QVERIFY(rows != nullptr);
            rows->via.array.ptr[0] = msgpack::object(uint64_t(1), zone);
        })));
}

void TestConnections::rejectsOversizedCollections() {
    // More rows than the bounded cache can hold: the decoder refuses the frame
    // rather than silently keeping a prefix.
    Snapshot oversized = makeSnapshot();
    oversized.connectionStatus = ConnectionStatus{true, false, false, {}};
    for (int i = 0; i < ball::connection::kMaxRows + 1; ++i) {
        oversized.connections.append(ConnectionSnapshot{quint64(i) + 1});
    }
    const QByteArray bytes = encode(oversized);
    QVERIFY(!bytes.isEmpty()); // the encoder has no table-size rule of its own
    QVERIFY(rejects(bytes));

    // The per-row list caps are enforced the same way.
    Snapshot tooManyInterfaces = makeSnapshot();
    tooManyInterfaces.connectionStatus = ConnectionStatus{true, false, false, {}};
    ConnectionSnapshot wide = makeConnection(1);
    for (int i = 0; i <= ball::connection::kMaxInterfaces; ++i) {
        wide.interfaces.append(u"if%1"_s.arg(i));
    }
    tooManyInterfaces.connections = {wide};
    QVERIFY(rejects(encode(tooManyInterfaces)));

    Snapshot tooManyIo = makeSnapshot();
    tooManyIo.connectionStatus = ConnectionStatus{true, false, false, {}};
    ConnectionSnapshot busy = makeConnection(1);
    busy.io.clear();
    for (int i = 0; i <= ball::connection::kMaxIo; ++i) {
        busy.io.append(ConnectionProcess{i + 1, quint64(i), u"p%1"_s.arg(i), 1, 1});
    }
    tooManyIo.connections = {busy};
    QVERIFY(rejects(encode(tooManyIo)));

    // Exactly the cap is still legal, so the bound is a limit and not an
    // off-by-one bug.
    Snapshot atCap = makeSnapshot();
    atCap.connectionStatus = ConnectionStatus{true, false, false, {}};
    ConnectionSnapshot full = makeConnection(1);
    full.interfaces.clear();
    full.io.clear();
    for (int i = 0; i < ball::connection::kMaxInterfaces; ++i) {
        full.interfaces.append(u"if%1"_s.arg(i));
    }
    for (int i = 0; i < ball::connection::kMaxIo; ++i) {
        full.io.append(ConnectionProcess{i + 1, quint64(i), u"p%1"_s.arg(i), 1, 1});
    }
    atCap.connections = {full};
    const Snapshot decoded = requireSnapshot(encode(atCap));
    QCOMPARE(decoded.connections.first().interfaces.size(),
             qsizetype(ball::connection::kMaxInterfaces));
    QCOMPARE(decoded.connections.first().io.size(), qsizetype(ball::connection::kMaxIo));
}

void TestConnections::acceptsLongTextAndManyParticipants() {
    // A real SNI domain can reach 253 bytes and a detail may carry several DNS
    // explanations; neither may be trimmed or refused.
    const QString longName = u"a"_s.repeated(241) + u".example.com"_s;
    QCOMPARE(longName.toUtf8().size(), 253);

    ConnectionSnapshot row = makeConnection(1);
    row.name = longName;
    row.detail = u"SNI %1; Host %2; CNAME edge.example.net; SVCB alpn=h2"_s.arg(longName, longName);
    // The detail names the same domain twice, so the cap must fit several
    // explanations and not just one endpoint.
    QCOMPARE(row.detail.count(longName), 2);
    QVERIFY(row.detail.size() > 2 * longName.size());
    row.source = u"[2001:db8::1]:443"_s;

    // A connection that many processes touched is normal for a shared socket.
    row.io.clear();
    for (int i = 0; i < 8; ++i) {
        row.io.append(ConnectionProcess{i + 1, quint64(i) + 9'007'199'254'740'993ULL,
                                        u"worker-%1"_s.arg(i), quint64(i), quint64(i) * 2});
    }
    row.interfaces = {u"eno1"_s, u"wg0"_s, u"docker0"_s};

    const Snapshot decoded = requireSnapshot(oneConnectionFrame(row));
    const ConnectionSnapshot& back = decoded.connections.first();
    QCOMPARE(back.name, longName);
    QCOMPARE(back.detail, row.detail);
    QCOMPARE(back.io.size(), 8);
    QCOMPARE(back.io.last().startNs, quint64(9'007'199'254'741'000ULL));
    QCOMPARE(back.interfaces, row.interfaces);

    // A list of notes may be long enough to explain several quality problems.
    Snapshot withNotes = makeSnapshot();
    withNotes.connectionStatus = ConnectionStatus{true, false, false, {}};
    withNotes.connectionStatus.notes = {u"collector has no flow kind"_s,
                                        u"expired PIDs: 3 identities, RX 1, TX 2"_s,
                                        u"truncated after 8192 connections"_s};
    const Snapshot decodedNotes = requireSnapshot(encode(withNotes));
    QCOMPARE(decodedNotes.connectionStatus.notes.size(), 3);
    QCOMPARE(decodedNotes.connectionStatus.notes.last(), u"truncated after 8192 connections"_s);
}

void TestConnections::rejectsMalformedStatusAndInfo() {
    for (const auto key :
         {QLatin1StringView("connections"), QLatin1StringView("connection_status")}) {
        QVERIFY(
            rejects(patchFrame(oneConnectionFrame(), [&](msgpack::object& root, msgpack::zone&) {
                QVERIFY(removeMember(root, key));
            })));
    }
    // The status booleans are required and must be booleans.
    QVERIFY(
        rejects(patchFrame(oneConnectionFrame(), [](msgpack::object& root, msgpack::zone& zone) {
            msgpack::object* status = member(root, QLatin1StringView("connection_status"));
            QVERIFY(status != nullptr);
            msgpack::object* truncated = member(*status, QLatin1StringView("truncated"));
            QVERIFY(truncated != nullptr);
            *truncated = msgpack::object(std::string("yes"), zone);
        })));
    QVERIFY(rejects(patchFrame(oneConnectionFrame(), [](msgpack::object& root, msgpack::zone&) {
        msgpack::object* status = member(root, QLatin1StringView("connection_status"));
        QVERIFY(status != nullptr);
        QVERIFY(removeMember(*status, QLatin1StringView("stale")));
    })));
    QVERIFY(
        rejects(patchFrame(oneConnectionFrame(), [](msgpack::object& root, msgpack::zone& zone) {
            msgpack::object* status = member(root, QLatin1StringView("connection_status"));
            QVERIFY(status != nullptr);
            msgpack::object* notes = member(*status, QLatin1StringView("notes"));
            QVERIFY(notes != nullptr);
            *notes = msgpack::object(std::string("not a list"), zone);
        })));
    QVERIFY(rejects(patchFrame(oneConnectionFrame(), [](msgpack::object& root, msgpack::zone&) {
        msgpack::object* status = member(root, QLatin1StringView("connection_status"));
        QVERIFY(status != nullptr);
        QVERIFY(removeMember(*status, QLatin1StringView("notes")));
    })));

    // `info`, when present, is validated like every other shown string list: a
    // wrong container is refused and so is an element of the wrong type.
    QVERIFY(
        rejects(patchFrame(oneConnectionFrame(), [](msgpack::object& root, msgpack::zone& zone) {
            msgpack::object* status = member(root, QLatin1StringView("process_status"));
            QVERIFY(status != nullptr);
            msgpack::object* info = member(*status, QLatin1StringView("info"));
            QVERIFY(info != nullptr);
            *info = msgpack::object(std::string("not a list"), zone);
        })));

    // One list element of the wrong type is enough to reject the frame, both for
    // process_status.info and for connection_status.notes.
    Snapshot withInfo = makeSnapshot();
    withInfo.processStatus.info = {u"fine"_s};
    withInfo.connectionStatus = ConnectionStatus{true, false, false, {u"fine"_s}};
    const QByteArray withElements = encode(withInfo);
    QVERIFY(decodeFrame(withElements).snapshot.has_value());
    const auto breakElement = [&withElements](QLatin1StringView statusKey,
                                              QLatin1StringView listKey) {
        return patchFrame(withElements, [&](msgpack::object& root, msgpack::zone& zone) {
            msgpack::object* status = member(root, statusKey);
            QVERIFY(status != nullptr);
            msgpack::object* list = member(*status, listKey);
            QVERIFY(list != nullptr && list->via.array.size == 1);
            list->via.array.ptr[0] = msgpack::object(uint64_t(5), zone);
        });
    };
    QVERIFY(rejects(breakElement(QLatin1StringView("process_status"), QLatin1StringView("info"))));
    QVERIFY(
        rejects(breakElement(QLatin1StringView("connection_status"), QLatin1StringView("notes"))));
}

void TestConnections::rejectsOversizedFrame() {
    Snapshot snapshot = makeSnapshot();
    ConnectionSnapshot row = makeConnection(1);
    row.detail = QString(ball::connection::kMaxTextBytes, u'x');
    for (int i = 0; i < 4096; ++i) {
        row.id = quint64(i) + 1;
        snapshot.connections.append(row);
    }
    bool ok = true;
    const QByteArray bytes = ball::encodeSnapshot(snapshot, ok);
    QVERIFY(!ok);
    QVERIFY(bytes.isEmpty());
}

QTEST_GUILESS_MAIN(TestConnections)
#include "connections.moc"
