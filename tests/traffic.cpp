// ball: a network traffic HUD for KDE Wayland sessions.
#include "daemon/traffic.h"
#include "daemon/interfaces.h"

#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QList>
#include <QTemporaryDir>
#include <QTest>
#include <QTime>

#include <cstring>

using ball::InterfaceInfo;
using ball::InterfaceKind;
using ball::Interfaces;
using ball::Traffic;
using namespace Qt::StringLiterals;

namespace {

// Serialises the raw netlink wire format so the decoder is tested without a
// kernel round trip, including deliberately damaged frames.
struct RawAttribute {
    quint16 type = 0;
    QByteArray payload;
};

struct RawLink {
    quint32 index = 0;
    quint32 flags = 0;
    quint32 sequence = 1;
    quint16 msgFlags = 0;
    QList<RawAttribute> attributes;
    QByteArray tail; // bytes after the attributes, to forge a malformed tail
};

struct LinkSpec {
    quint32 index = 0;
    quint32 flags = 0;
    quint32 sequence = 1;
    QString name;
    quint64 rx = 0;
    quint64 tx = 0;
    bool linkInfo = false;
    qsizetype statsSize = 200;
};

constexpr quint32 errorSequence = 4;

struct DoneSpec {
    quint32 sequence = 1;
    qint32 status = 0;
    quint16 flags = 0;
};

QByteArray statsPayload(qsizetype size, const ball::TrafficCounters& counters) {
    QByteArray payload(size, '\0');
    if (size >= 24) {
        std::memcpy(payload.data() + 16, &counters.rxBytes, sizeof(counters.rxBytes));
    }
    if (size >= 32) {
        std::memcpy(payload.data() + 24, &counters.txBytes, sizeof(counters.txBytes));
    }
    return payload;
}

QByteArray encodeRawLink(const RawLink& link) {
    QByteArray body;
    for (const RawAttribute& attribute : link.attributes) {
        rtattr header{};
        header.rta_type = attribute.type;
        header.rta_len = static_cast<unsigned short>(RTA_LENGTH(attribute.payload.size()));
        body.append(reinterpret_cast<const char*>(&header), sizeof(header));
        body.append(attribute.payload);
        body.append(static_cast<qsizetype>(RTA_ALIGN(header.rta_len)) -
                        static_cast<qsizetype>(header.rta_len),
                    '\0');
    }

    nlmsghdr header{};
    header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg)) + static_cast<quint32>(body.size()) +
                       static_cast<quint32>(link.tail.size());
    header.nlmsg_type = RTM_NEWLINK;
    header.nlmsg_flags = link.msgFlags;
    header.nlmsg_seq = link.sequence;

    ifinfomsg info{};
    info.ifi_family = AF_UNSPEC;
    // Assigning through the signed field keeps a deliberately negative index.
    const int rawIndex = static_cast<int>(link.index);
    std::memcpy(&info.ifi_index, &rawIndex, sizeof(info.ifi_index));
    info.ifi_flags = link.flags;

    QByteArray message(reinterpret_cast<const char*>(&header), sizeof(header));
    message.append(reinterpret_cast<const char*>(&info), sizeof(info));
    message.append(body);
    message.append(link.tail);
    return message;
}

QByteArray encodeLink(const LinkSpec& spec) {
    RawLink raw{.index = spec.index, .flags = spec.flags, .sequence = spec.sequence};
    raw.attributes = {{IFLA_IFNAME, spec.name.toUtf8() + '\0'},
                      {IFLA_STATS64, statsPayload(spec.statsSize, {spec.rx, spec.tx})}};
    if (spec.linkInfo) {
        raw.attributes.append({IFLA_LINKINFO, QByteArray(4, '\0')});
    }
    return encodeRawLink(raw);
}

QByteArray encodeDone(const DoneSpec& spec) {
    nlmsghdr header{};
    header.nlmsg_len = NLMSG_LENGTH(sizeof(spec.status));
    header.nlmsg_type = NLMSG_DONE;
    header.nlmsg_flags = spec.flags;
    header.nlmsg_seq = spec.sequence;
    QByteArray message(reinterpret_cast<const char*>(&header), sizeof(header));
    message.append(reinterpret_cast<const char*>(&spec.status), sizeof(spec.status));
    return message;
}

QByteArray encodeError(int error) {
    nlmsgerr payload{};
    payload.error = error;
    nlmsghdr header{};
    header.nlmsg_len = NLMSG_LENGTH(sizeof(nlmsgerr));
    header.nlmsg_type = NLMSG_ERROR;
    header.nlmsg_seq = errorSequence;
    QByteArray message(reinterpret_cast<const char*>(&header), sizeof(header));
    message.append(reinterpret_cast<const char*>(&payload), sizeof(payload));
    return message;
}

InterfaceInfo makeInfo(quint32 index, const QString& name, quint64 rx, quint64 tx,
                       InterfaceKind kind = InterfaceKind::Physical, bool up = true) {
    InterfaceInfo info;
    info.index = index;
    info.name = name;
    info.kind = kind;
    info.up = up;
    info.rxBytes = rx;
    info.txBytes = tx;
    return info;
}

InterfaceInfo require(const std::optional<InterfaceInfo>& value, const char* what) {
    if (!value.has_value()) {
        QTest::qFail(what, __FILE__, __LINE__);
        return {};
    }
    return *value;
}

const QDateTime& referenceDate() {
    static const QDateTime value(QDate(2026, 10, 13), QTime(14, 0, 0));
    return value;
}

} // namespace

class TestTraffic : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void firstSampleEstablishesBaseline();
    void realElapsedAddsBothDirections();
    void zeroElapsedKeepsBaseline();
    void counterResetRebases();
    void crossingMidnightClearsToday();
    void removedInterfaceKeepsTotalsAndRebuildsBaseline();
    void renameKeepsIdentity();
    void wallClockStepKeepsMonotonicRates();
    void invalidSamplesAreRejected();
    void kindUsesSysfsDeviceEntry();
    void decoderAcceptsCompatibleStatsPayload();
    void decodeLinkMessageRejectsDamagedInput();
    void decodeLinkMessageAppliesAttributeTypeMask();
    void decodeLinkDumpHandlesSplitAndDone();
    void decodeLinkDumpRejectsBadFrameLength();
    void decodeLinkDumpRejectsErrorPayload();
    void decodeLinkDumpRejectsInterruptedDump();
    void decodeLinkDumpRejectsStatusDone();
    void realInterfacesSmoke();
};

void TestTraffic::firstSampleEstablishesBaseline() {
    Traffic traffic;
    QVERIFY(traffic.update({makeInfo(2, u"eth0"_s, 1'000'000, 2'000'000)}, referenceDate(), 0));

    const ball::InterfaceTraffic row = traffic.row(2);
    QCOMPARE(row.index, quint32(2));
    QCOMPARE(row.today.rxBytes, quint64(0));
    QCOMPARE(row.session.txBytes, quint64(0));
    QCOMPARE(row.rate.rxBytesPerSecond, 0.0);
    QCOMPARE(traffic.monitoringStart(), referenceDate());
}

void TestTraffic::realElapsedAddsBothDirections() {
    Traffic traffic;
    traffic.update({makeInfo(2, u"eth0"_s, 1'000, 2'000)}, referenceDate(), 0);
    traffic.update({makeInfo(2, u"eth0"_s, 4'000, 3'000)}, referenceDate(), 10'000);

    const ball::InterfaceTraffic row = traffic.row(2);
    QCOMPARE(row.today.rxBytes, quint64(3'000));
    QCOMPARE(row.today.txBytes, quint64(1'000));
    QCOMPARE(row.session.rxBytes, quint64(3'000));
    QCOMPARE(row.rate.rxBytesPerSecond, 300.0);
    QCOMPARE(row.rate.txBytesPerSecond, 100.0);
}

void TestTraffic::zeroElapsedKeepsBaseline() {
    Traffic traffic;
    traffic.update({makeInfo(2, u"eth0"_s, 1'000, 1'000)}, referenceDate(), 0);
    traffic.update({makeInfo(2, u"eth0"_s, 2'000, 2'000)}, referenceDate(), 0);
    QCOMPARE(traffic.row(2).today.rxBytes, quint64(0));

    // The zero-length interval must not consume the baseline, so the bytes that
    // arrived while elapsed was zero are still counted once time really passes.
    traffic.update({makeInfo(2, u"eth0"_s, 3'000, 3'000)}, referenceDate(), 10'000);
    QCOMPARE(traffic.row(2).today.rxBytes, quint64(2'000));
    QCOMPARE(traffic.row(2).rate.rxBytesPerSecond, 200.0);
}

void TestTraffic::counterResetRebases() {
    Traffic traffic;
    traffic.update({makeInfo(2, u"eth0"_s, 10'000, 10'000)}, referenceDate(), 0);
    // rx dropped to a fresh counter, tx kept growing: rebase rx, count tx.
    traffic.update({makeInfo(2, u"eth0"_s, 500, 12'000)}, referenceDate(), 1'000);

    const ball::InterfaceTraffic row = traffic.row(2);
    QCOMPARE(row.today.rxBytes, quint64(0));
    QCOMPARE(row.session.rxBytes, quint64(0));
    QCOMPARE(row.today.txBytes, quint64(2'000));
    QCOMPARE(row.rate.rxBytesPerSecond, 0.0);
    QCOMPARE(row.rate.txBytesPerSecond, 2'000.0);
}

void TestTraffic::crossingMidnightClearsToday() {
    Traffic traffic;
    const QDateTime beforeMidnight(QDate(2026, 10, 14), QTime(23, 59, 50));
    traffic.update({makeInfo(2, u"eth0"_s, 100, 100)}, beforeMidnight, 0);
    const QDateTime afterMidnight(QDate(2026, 10, 15), QTime(0, 0, 10));
    traffic.update({makeInfo(2, u"eth0"_s, 200, 200)}, afterMidnight, 10'000);

    // The interval spanning midnight is credited to the new day.
    const ball::InterfaceTraffic row = traffic.row(2);
    QCOMPARE(row.today.rxBytes, quint64(100));
    QCOMPARE(row.session.rxBytes, quint64(100));

    traffic.update({makeInfo(2, u"eth0"_s, 250, 250)}, afterMidnight.addSecs(10), 20'000);
    QCOMPARE(traffic.row(2).today.rxBytes, quint64(150));
    QCOMPARE(traffic.row(2).session.rxBytes, quint64(150));
}

void TestTraffic::removedInterfaceKeepsTotalsAndRebuildsBaseline() {
    Traffic traffic;
    traffic.update({makeInfo(7, u"veth7"_s, 1'000, 1'000)}, referenceDate(), 0);
    traffic.update({makeInfo(7, u"veth7"_s, 1'500, 1'500)}, referenceDate(), 1'000);
    QCOMPARE(traffic.row(7).session.rxBytes, quint64(500));

    traffic.update({}, referenceDate(), 2'000);
    QCOMPARE(traffic.row(7).session.rxBytes, quint64(500)); // observed traffic is kept
    QVERIFY(!traffic.row(7).up);

    // Reappears with unrelated counters: fresh baseline, no mixed values, and
    // the traffic observed before it disappeared is still there.
    traffic.update({makeInfo(7, u"veth7"_s, 900'000, 900'000)}, referenceDate(), 3'000);
    QCOMPARE(traffic.row(7).today.rxBytes, quint64(500));
    QCOMPARE(traffic.row(7).session.rxBytes, quint64(500));

    traffic.update({makeInfo(7, u"veth7"_s, 900'100, 900'100)}, referenceDate(), 4'000);
    QCOMPARE(traffic.row(7).session.rxBytes, quint64(600));
}

void TestTraffic::renameKeepsIdentity() {
    Traffic traffic;
    traffic.update({makeInfo(2, u"eth0"_s, 100, 100)}, referenceDate(), 0);
    traffic.update({makeInfo(2, u"enp1s0"_s, 200, 200)}, referenceDate(), 1'000);

    const ball::InterfaceTraffic row = traffic.row(2);
    QCOMPARE(row.name, u"enp1s0"_s);
    QCOMPARE(row.today.rxBytes, quint64(100));
    QCOMPARE(row.session.rxBytes, quint64(100));
}

void TestTraffic::wallClockStepKeepsMonotonicRates() {
    Traffic traffic;
    // Wall clock jumps five minutes backwards, monotonic time advances 10 s.
    traffic.update({makeInfo(2, u"eth0"_s, 1'000, 1'000)}, referenceDate(), 0);
    traffic.update({makeInfo(2, u"eth0"_s, 3'000, 2'000)}, referenceDate().addSecs(-300), 10'000);

    const ball::InterfaceTraffic row = traffic.row(2);
    QCOMPARE(row.session.rxBytes, quint64(2'000));
    QCOMPARE(row.session.txBytes, quint64(1'000));
    QCOMPARE(row.rate.rxBytesPerSecond, 200.0);
    QCOMPARE(row.rate.txBytesPerSecond, 100.0);
    QCOMPARE(traffic.monitoringStart(), referenceDate()); // the first valid sample wins

    // The wall clock jumping forwards must not break the monotonic rate either.
    traffic.update({makeInfo(2, u"eth0"_s, 4'000, 2'000)}, referenceDate().addSecs(3'600), 20'000);
    QCOMPARE(traffic.row(2).session.rxBytes, quint64(3'000));
    QCOMPARE(traffic.row(2).rate.rxBytesPerSecond, 100.0);
}

void TestTraffic::invalidSamplesAreRejected() {
    Traffic traffic;
    QVERIFY(traffic.update({makeInfo(2, u"eth0"_s, 1'000, 1'000)}, referenceDate(), 0));
    QCOMPARE(traffic.row(2).session.rxBytes, quint64(0));

    // Invalid wall clock or monotonic stamp: no totals move, no baseline moves.
    QVERIFY(!traffic.update({makeInfo(2, u"eth0"_s, 5'000, 5'000)}, QDateTime(), 1'000));
    QVERIFY(!traffic.update({makeInfo(2, u"eth0"_s, 5'000, 5'000)}, referenceDate(), -1));
    QCOMPARE(traffic.row(2).session.rxBytes, quint64(0));

    // A rejected sample must not consume the elapsed gap.
    traffic.update({makeInfo(2, u"eth0"_s, 5'000, 5'000)}, referenceDate(), 10'000);
    QCOMPARE(traffic.row(2).session.rxBytes, quint64(4'000));
    QCOMPARE(traffic.row(2).rate.rxBytesPerSecond, 400.0);

    Traffic fresh;
    QVERIFY(!fresh.update({makeInfo(2, u"eth0"_s, 1, 1)}, QDateTime(), 0));
    QVERIFY(!fresh.monitoringStart().isValid());
    QVERIFY(fresh.rows().isEmpty());
}

void TestTraffic::kindUsesSysfsDeviceEntry() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(u"eth0"_s));
    QVERIFY(root.mkpath(u"veth0"_s));
    // A real netdev has a `device` symlink next to it; a software device does not.
    QVERIFY(QFile::link(root.filePath(u"eth0"_s), root.filePath(u"eth0/device"_s)));

    QVERIFY(!ball::interfaceIsVirtual(u"eth0"_s, false, {temporary.path()}));
    QVERIFY(ball::interfaceIsVirtual(u"veth0"_s, false, {temporary.path()}));
    // Unknown sysfs tree: fall back to the IFLA_LINKINFO hint.
    QVERIFY(ball::interfaceIsVirtual(u"wg0"_s, true, {QStringLiteral("/nonexistent-ball-root")}));
    QVERIFY(!ball::interfaceIsVirtual(u"wg0"_s, false, {QStringLiteral("/nonexistent-ball-root")}));
}

void TestTraffic::decoderAcceptsCompatibleStatsPayload() {
    // Older kernels may ship a shorter IFLA_STATS64; only rx/tx are needed.
    const QByteArray shortStats = encodeLink(
        {.index = 2, .flags = IFF_UP, .name = u"eth0"_s, .rx = 7, .tx = 9, .statsSize = 32});
    const InterfaceInfo info = require(
        ball::decodeLinkMessage(shortStats.constData(), shortStats.size()), "32-byte stats");
    QCOMPARE(info.rxBytes, quint64(7));
    QCOMPARE(info.txBytes, quint64(9));

    // A payload that stops before tx_bytes is unusable.
    const QByteArray tooShort = encodeLink(
        {.index = 2, .flags = IFF_UP, .name = u"eth0"_s, .rx = 7, .tx = 9, .statsSize = 24});
    QVERIFY(!ball::decodeLinkMessage(tooShort.constData(), tooShort.size()).has_value());
}

void TestTraffic::decodeLinkMessageRejectsDamagedInput() {
    QVERIFY(!ball::decodeLinkMessage(nullptr, 0).has_value());
    const QByteArray valid =
        encodeLink({.index = 2, .flags = IFF_UP, .name = u"eth0"_s, .rx = 1, .tx = 2});
    QVERIFY(!ball::decodeLinkMessage(valid.constData(), 8).has_value());

    QByteArray wrongType = valid;
    wrongType[4] = static_cast<char>(static_cast<unsigned>(RTM_DELLINK) & 0xFFu); // nlmsg_type
    QVERIFY(!ball::decodeLinkMessage(wrongType.constData(), wrongType.size()).has_value());

    const QByteArray noName = encodeRawLink(
        {.index = 2, .flags = IFF_UP, .attributes = {{IFLA_STATS64, statsPayload(200, {1, 2})}}});
    QVERIFY(!ball::decodeLinkMessage(noName.constData(), noName.size()).has_value());

    const QByteArray noStats =
        encodeRawLink({.index = 2, .flags = IFF_UP, .attributes = {{IFLA_IFNAME, "eth0"}}});
    QVERIFY(!ball::decodeLinkMessage(noStats.constData(), noStats.size()).has_value());

    // A negative kernel index, a non-NUL-terminated name and trailing garbage
    // must all be rejected rather than silently patched up.
    const QByteArray negativeIndex = encodeLink(
        {.index = static_cast<quint32>(-3), .flags = IFF_UP, .name = u"eth0"_s, .rx = 1, .tx = 2});
    QVERIFY(!ball::decodeLinkMessage(negativeIndex.constData(), negativeIndex.size()).has_value());

    const QByteArray unterminatedName =
        encodeRawLink({.index = 2,
                       .flags = IFF_UP,
                       .attributes = {{IFLA_IFNAME, QByteArray("eth0", 4)},
                                      {IFLA_STATS64, statsPayload(200, {1, 2})}}});
    QVERIFY(!ball::decodeLinkMessage(unterminatedName.constData(), unterminatedName.size())
                 .has_value());

    const QByteArray trailingGarbage = encodeRawLink(
        {.index = 2,
         .flags = IFF_UP,
         .attributes = {{IFLA_IFNAME, "eth0"}, {IFLA_STATS64, statsPayload(200, {1, 2})}},
         .tail = QByteArray("\x01\x02\x03", 3)});
    QVERIFY(
        !ball::decodeLinkMessage(trailingGarbage.constData(), trailingGarbage.size()).has_value());
}

void TestTraffic::decodeLinkMessageAppliesAttributeTypeMask() {
    // IFLA_IFNAME with the NLA_F_NESTED flag set must still be read as a name.
    const QByteArray nestedName = encodeRawLink(
        {.index = 2,
         .flags = IFF_UP,
         .attributes = {{static_cast<quint16>(IFLA_IFNAME | NLA_F_NESTED), QByteArray("eth9\0", 5)},
                        {IFLA_STATS64, statsPayload(200, {5, 6})}}});
    const InterfaceInfo info = require(
        ball::decodeLinkMessage(nestedName.constData(), nestedName.size()), "masked attribute");
    QCOMPARE(info.name, u"eth9"_s);
    QCOMPARE(info.rxBytes, quint64(5));
}

void TestTraffic::decodeLinkDumpHandlesSplitAndDone() {
    const QByteArray first = encodeLink(
        {.index = 2, .flags = IFF_UP, .sequence = 9, .name = u"eth0"_s, .rx = 10, .tx = 20});
    const QByteArray second = encodeLink({.index = 3,
                                          .flags = IFF_UP,
                                          .sequence = 9,
                                          .name = u"veth0"_s,
                                          .rx = 30,
                                          .tx = 40,
                                          .linkInfo = true});

    QByteArray buffer = first + second.left(10);
    ball::LinkDump dump = ball::decodeLinkDump(buffer, 9);
    QCOMPARE(dump.interfaces.size(), 1);
    QVERIFY(!dump.complete);
    QVERIFY(!dump.failed);
    QVERIFY(!buffer.isEmpty()); // the partial frame waits for the rest

    buffer.append(second.mid(10) + encodeDone({.sequence = 9}));
    dump = ball::decodeLinkDump(buffer, 9);
    QCOMPARE(dump.interfaces.size(), 1);
    QVERIFY(dump.complete);
    QVERIFY(!dump.failed);
    QVERIFY(buffer.isEmpty());

    // Frames of another dump are consumed, not mixed in.
    QByteArray mismatched = encodeLink(
        {.index = 2, .flags = IFF_UP, .sequence = 5, .name = u"eth0"_s, .rx = 10, .tx = 20});
    dump = ball::decodeLinkDump(mismatched, 6);
    QVERIFY(dump.interfaces.isEmpty());
    QVERIFY(!dump.complete);
    QVERIFY(!dump.failed);
    QVERIFY(mismatched.isEmpty());

    QByteArray staleDone = encodeDone({.sequence = 5}) + encodeDone({.sequence = 6});
    dump = ball::decodeLinkDump(staleDone, 6);
    QVERIFY(dump.complete);
    QVERIFY(!dump.failed);
    QVERIFY(staleDone.isEmpty());
}

void TestTraffic::decodeLinkDumpRejectsBadFrameLength() {
    nlmsghdr header{};
    header.nlmsg_len = 4; // smaller than a header: unrecoverable framing
    header.nlmsg_type = RTM_NEWLINK;
    header.nlmsg_seq = 1;
    QByteArray buffer(reinterpret_cast<const char*>(&header), sizeof(header));
    buffer.append(static_cast<qsizetype>(NLMSG_HDRLEN) - static_cast<qsizetype>(sizeof(header)),
                  '\0');

    const ball::LinkDump dump = ball::decodeLinkDump(buffer, 1);
    QVERIFY(dump.interfaces.isEmpty());
    QVERIFY(dump.failed);
    QVERIFY(!dump.complete);
    QVERIFY(buffer.isEmpty());
}

void TestTraffic::decodeLinkDumpRejectsErrorPayload() {
    // A valid interface followed by an error reply must reject the whole dump,
    // so the caller cannot mistake the partial list for a real inventory.
    const QByteArray link = encodeLink({.index = 2,
                                        .flags = IFF_UP,
                                        .sequence = errorSequence,
                                        .name = u"eth0"_s,
                                        .rx = 10,
                                        .tx = 20});
    QByteArray buffer = link + encodeError(-EINVAL);
    const ball::LinkDump dump = ball::decodeLinkDump(buffer, errorSequence);
    QVERIFY(!dump.complete);
    QVERIFY(dump.failed);
    QVERIFY(buffer.isEmpty());
}

void TestTraffic::decodeLinkDumpRejectsInterruptedDump() {
    // NLM_F_DUMP_INTR says the kernel-visible list changed mid-dump.
    QByteArray buffer = encodeLink({.index = 2,
                                    .flags = IFF_UP,
                                    .sequence = 11,
                                    .name = u"eth0"_s,
                                    .rx = 1,
                                    .tx = 2,
                                    .linkInfo = false});
    nlmsghdr header{};
    std::memcpy(&header, buffer.constData(), sizeof(header));
    header.nlmsg_flags = NLM_F_DUMP_INTR;
    std::memcpy(buffer.data(), &header, sizeof(header));

    const ball::LinkDump interrupted = ball::decodeLinkDump(buffer, 11);
    QVERIFY(interrupted.failed);
    QVERIFY(!interrupted.complete);
    QVERIFY(interrupted.interfaces.isEmpty());
}

void TestTraffic::decodeLinkDumpRejectsStatusDone() {
    // A zero status is the normal dump terminator...
    QByteArray terminator = encodeDone({.sequence = 12, .flags = NLM_F_MULTI});
    const ball::LinkDump done = ball::decodeLinkDump(terminator, 12);
    QVERIFY(done.complete);
    QVERIFY(!done.failed);
    QVERIFY(terminator.isEmpty());

    // ...but a nonzero embedded status must reject the dump.
    QByteArray buffer = encodeDone({.sequence = 12, .status = -EINVAL, .flags = NLM_F_MULTI});
    const ball::LinkDump dump = ball::decodeLinkDump(buffer, 12);
    QVERIFY(dump.failed);
    QVERIFY(!dump.complete);
    QVERIFY(buffer.isEmpty());
}

void TestTraffic::realInterfacesSmoke() {
    Interfaces interfaces;
    const std::optional<QList<InterfaceInfo>> dump = interfaces.poll(2000);
    QVERIFY2(dump.has_value(), "a real rtnetlink dump should complete");
    const QList<InterfaceInfo> list = dump.value_or(QList<InterfaceInfo>{});
    QVERIFY(!list.isEmpty());

    bool sawLoopback = false;
    for (const InterfaceInfo& info : list) {
        QVERIFY(info.index != 0);
        QVERIFY(!info.name.isEmpty());
        QVERIFY(info.kind != InterfaceKind::Loopback || info.name == u"lo"_s);
        if (info.kind == InterfaceKind::Loopback) {
            sawLoopback = true;
        }
    }
    QVERIFY(sawLoopback);
}

QTEST_GUILESS_MAIN(TestTraffic)
#include "traffic.moc"
