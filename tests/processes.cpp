// ball: a network traffic HUD for KDE Wayland sessions.
#include "daemon/processes.h"

#include <QDateTime>
#include <QList>
#include <QTest>
#include <QTime>

using ball::ProcessCounter;
using ball::Processes;
using ball::ProcessTraffic;
using namespace Qt::StringLiterals;

namespace {

ProcessCounter makeCounter(int pid, const QString& name, quint64 startNs,
                           const ball::TrafficCounters& bytes) {
    ProcessCounter counter;
    counter.pid = pid;
    counter.startNs = startNs;
    counter.name = name;
    counter.rxBytes = bytes.rxBytes;
    counter.txBytes = bytes.txBytes;
    return counter;
}

const QDateTime& referenceDate() {
    static const QDateTime value(QDate(2026, 10, 13), QTime(14, 0, 0));
    return value;
}

} // namespace

class TestProcesses : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void firstSampleEstablishesBaseline();
    void realElapsedAddsBothDirections();
    void newProcessCountsExistingBytes();
    void recycledPidIsANewIdentity();
    void counterRollbackStartsNewSegment();
    void absentProcessKeepsTotalsAndCountsOnReturn();
    void backendRestartRebasesAndKeepsTotals();
    void crossingMidnightClearsTodayOnly();
    void zeroElapsedKeepsBaseline();
    void wallClockStepKeepsMonotonicRates();
    void invalidSamplesAreRejected();
    void idleProcessRateIsZeroAndMissingPidIgnored();
};

void TestProcesses::firstSampleEstablishesBaseline() {
    Processes processes;
    QVERIFY(processes.update({makeCounter(42, u"firefox"_s, 1000, {1'000'000, 2'000'000})},
                             referenceDate(), 0));

    const ProcessTraffic row = processes.row(42, 1000);
    QCOMPARE(row.pid, 42);
    QCOMPARE(row.startNs, quint64(1000));
    QCOMPARE(row.name, u"firefox"_s);
    QVERIFY(row.present);
    QCOMPARE(row.today.rxBytes, quint64(0));
    QCOMPARE(row.session.txBytes, quint64(0));
    QCOMPARE(row.rate.rxBytesPerSecond, 0.0);
    QCOMPARE(processes.monitoringStart(), referenceDate());
    QCOMPARE(processes.rows().size(), 1);
}

void TestProcesses::realElapsedAddsBothDirections() {
    Processes processes;
    processes.update({makeCounter(42, u"firefox"_s, 1000, {1'000, 2'000})}, referenceDate(), 0);
    processes.update({makeCounter(42, u"firefox"_s, 1000, {4'000, 3'000})}, referenceDate(),
                     10'000);

    const ProcessTraffic row = processes.row(42, 1000);
    QCOMPARE(row.today.rxBytes, quint64(3'000));
    QCOMPARE(row.today.txBytes, quint64(1'000));
    QCOMPARE(row.session.rxBytes, quint64(3'000));
    QCOMPARE(row.session.txBytes, quint64(1'000));
    QCOMPARE(row.rate.rxBytesPerSecond, 300.0);
    QCOMPARE(row.rate.txBytesPerSecond, 100.0);
    QVERIFY(row.present);
}

void TestProcesses::newProcessCountsExistingBytes() {
    Processes processes;
    processes.update({makeCounter(42, u"firefox"_s, 1000, {1'000, 1'000})}, referenceDate(), 0);
    // pid 43 appears for the first time; its segment is already non-zero.
    processes.update({makeCounter(42, u"firefox"_s, 1000, {1'500, 1'500}),
                      makeCounter(43, u"curl"_s, 2000, {5'000, 6'000})},
                     referenceDate(), 2'000);

    const ProcessTraffic fresh = processes.row(43, 2000);
    QVERIFY(fresh.present);
    QCOMPARE(fresh.today.rxBytes, quint64(5'000));
    QCOMPARE(fresh.session.txBytes, quint64(6'000));
    QCOMPARE(fresh.rate.rxBytesPerSecond, 2'500.0);
    QCOMPARE(processes.row(42, 1000).today.rxBytes, quint64(500));
}

void TestProcesses::recycledPidIsANewIdentity() {
    Processes processes;
    processes.update({makeCounter(7, u"old"_s, 100, {1'000, 1'000})}, referenceDate(), 0);
    processes.update({makeCounter(7, u"old"_s, 100, {1'100, 1'100})}, referenceDate(), 1'000);
    // The PID is reused by a different incarnation: a separate row, not a
    // counter rollback on the old one.
    processes.update({makeCounter(7, u"new"_s, 999, {50, 60})}, referenceDate(), 2'000);

    const ProcessTraffic oldRow = processes.row(7, 100);
    QVERIFY(!oldRow.present);
    QCOMPARE(oldRow.session.rxBytes, quint64(100));
    QCOMPARE(oldRow.name, u"old"_s);

    const ProcessTraffic newRow = processes.row(7, 999);
    QVERIFY(newRow.present);
    QCOMPARE(newRow.name, u"new"_s);
    QCOMPARE(newRow.session.rxBytes, quint64(50)); // first sight of this identity
    QCOMPARE(newRow.session.txBytes, quint64(60));
    QCOMPARE(processes.rows().size(), 2);
}

void TestProcesses::counterRollbackStartsNewSegment() {
    Processes processes;
    processes.update({makeCounter(9, u"app"_s, 1, {10'000, 10'000})}, referenceDate(), 0);
    // rx restarted while tx kept growing: each direction is handled on its own.
    processes.update({makeCounter(9, u"app"_s, 1, {500, 12'000})}, referenceDate(), 1'000);

    const ProcessTraffic row = processes.row(9, 1);
    QCOMPARE(row.session.rxBytes, quint64(500)); // the new segment, not an underflow
    QCOMPARE(row.session.txBytes, quint64(2'000));
    QCOMPARE(row.today.rxBytes, quint64(500));
    QCOMPARE(row.rate.rxBytesPerSecond, 500.0);
    QCOMPARE(row.rate.txBytesPerSecond, 2'000.0);
}

void TestProcesses::absentProcessKeepsTotalsAndCountsOnReturn() {
    Processes processes;
    processes.update({makeCounter(5, u"app"_s, 1, {1'000, 1'000})}, referenceDate(), 0);
    processes.update({makeCounter(5, u"app"_s, 1, {1'500, 1'500})}, referenceDate(), 1'000);
    QCOMPARE(processes.row(5, 1).session.rxBytes, quint64(500));

    // Gone: the row and its observed traffic survive, the rate goes to zero.
    processes.update({}, referenceDate(), 2'000);
    const ProcessTraffic absent = processes.row(5, 1);
    QVERIFY(!absent.present);
    QCOMPARE(absent.session.rxBytes, quint64(500));
    QCOMPARE(absent.rate.rxBytesPerSecond, 0.0);
    QCOMPARE(absent.rate.txBytesPerSecond, 0.0);

    // Back with counters that cannot be compared to the old segment: the whole
    // current segment is new bytes on top of the preserved totals.
    processes.update({makeCounter(5, u"app"_s, 1, {900'000, 900'000})}, referenceDate(), 3'000);
    QCOMPARE(processes.row(5, 1).session.rxBytes, quint64(900'500));
    QVERIFY(processes.row(5, 1).present);

    processes.update({makeCounter(5, u"app"_s, 1, {900'100, 900'100})}, referenceDate(), 4'000);
    QCOMPARE(processes.row(5, 1).session.rxBytes, quint64(900'600));
}

void TestProcesses::backendRestartRebasesAndKeepsTotals() {
    Processes processes;
    processes.update({makeCounter(3, u"app"_s, 1, {1'000, 2'000})}, referenceDate(), 0);
    processes.update({makeCounter(3, u"app"_s, 1, {3'000, 4'000})}, referenceDate(), 1'000);
    QCOMPARE(processes.row(3, 1).session.rxBytes, quint64(2'000));

    // The collector restarted: counters are unrelated, so nothing is counted and
    // the sample rate is zero, but the observed totals survive.
    const bool ok =
        processes.update({makeCounter(3, u"app"_s, 1, {50, 70})}, referenceDate(), 2'000,
                         /*backendSessionChanged=*/true);
    QVERIFY(ok);
    const ProcessTraffic rebooted = processes.row(3, 1);
    QCOMPARE(rebooted.session.rxBytes, quint64(2'000));
    QCOMPARE(rebooted.session.txBytes, quint64(2'000));
    QCOMPARE(rebooted.today.rxBytes, quint64(2'000));
    QCOMPARE(rebooted.rate.rxBytesPerSecond, 0.0);
    QVERIFY(rebooted.present);

    // The new session still accumulates normally from its own baseline.
    processes.update({makeCounter(3, u"app"_s, 1, {250, 270})}, referenceDate(), 3'000);
    QCOMPARE(processes.row(3, 1).session.rxBytes, quint64(2'200));
    QCOMPARE(processes.row(3, 1).session.txBytes, quint64(2'200));
    QCOMPARE(processes.row(3, 1).rate.rxBytesPerSecond, 200.0);
}

void TestProcesses::crossingMidnightClearsTodayOnly() {
    Processes processes;
    const QDateTime beforeMidnight(QDate(2026, 10, 14), QTime(23, 59, 50));
    processes.update({makeCounter(2, u"app"_s, 1, {100, 100})}, beforeMidnight, 0);
    const QDateTime afterMidnight(QDate(2026, 10, 15), QTime(0, 0, 10));
    processes.update({makeCounter(2, u"app"_s, 1, {200, 200})}, afterMidnight, 10'000);

    // The interval spanning midnight is credited to the new day.
    const ProcessTraffic row = processes.row(2, 1);
    QCOMPARE(row.today.rxBytes, quint64(100));
    QCOMPARE(row.session.rxBytes, quint64(100));

    processes.update({makeCounter(2, u"app"_s, 1, {250, 250})}, afterMidnight.addSecs(10), 20'000);
    QCOMPARE(processes.row(2, 1).today.rxBytes, quint64(150));
    QCOMPARE(processes.row(2, 1).session.rxBytes, quint64(150));
    QCOMPARE(processes.monitoringStart(), beforeMidnight); // the first valid sample wins
}

void TestProcesses::zeroElapsedKeepsBaseline() {
    Processes processes;
    processes.update({makeCounter(2, u"app"_s, 1, {1'000, 1'000})}, referenceDate(), 0);
    processes.update({makeCounter(2, u"app"_s, 1, {2'000, 2'000})}, referenceDate(), 0);
    QCOMPARE(processes.row(2, 1).today.rxBytes, quint64(0));

    // The zero-length interval must not consume the baseline, so the bytes that
    // arrived while elapsed was zero are still counted once time really passes.
    processes.update({makeCounter(2, u"app"_s, 1, {3'000, 3'000})}, referenceDate(), 10'000);
    QCOMPARE(processes.row(2, 1).today.rxBytes, quint64(2'000));
    QCOMPARE(processes.row(2, 1).rate.rxBytesPerSecond, 200.0);
}

void TestProcesses::wallClockStepKeepsMonotonicRates() {
    Processes processes;
    // Wall clock jumps five minutes backwards, monotonic time advances 10 s.
    processes.update({makeCounter(2, u"app"_s, 1, {1'000, 1'000})}, referenceDate(), 0);
    processes.update({makeCounter(2, u"app"_s, 1, {3'000, 2'000})}, referenceDate().addSecs(-300),
                     10'000);

    const ProcessTraffic row = processes.row(2, 1);
    QCOMPARE(row.session.rxBytes, quint64(2'000));
    QCOMPARE(row.session.txBytes, quint64(1'000));
    QCOMPARE(row.rate.rxBytesPerSecond, 200.0);
    QCOMPARE(row.rate.txBytesPerSecond, 100.0);
    QCOMPARE(processes.monitoringStart(), referenceDate()); // the first valid sample wins

    // The wall clock jumping forwards must not break the monotonic rate either.
    processes.update({makeCounter(2, u"app"_s, 1, {4'000, 2'000})}, referenceDate().addSecs(3'600),
                     20'000);
    QCOMPARE(processes.row(2, 1).session.rxBytes, quint64(3'000));
    QCOMPARE(processes.row(2, 1).rate.rxBytesPerSecond, 100.0);
}

void TestProcesses::invalidSamplesAreRejected() {
    Processes processes;
    QVERIFY(processes.update({makeCounter(2, u"app"_s, 1, {1'000, 1'000})}, referenceDate(), 0));
    QCOMPARE(processes.row(2, 1).session.rxBytes, quint64(0));

    // Invalid wall clock or monotonic stamp: no totals move, no baseline moves.
    QVERIFY(!processes.update({makeCounter(2, u"app"_s, 1, {5'000, 5'000})}, QDateTime(), 1'000));
    QVERIFY(!processes.update({makeCounter(2, u"app"_s, 1, {5'000, 5'000})}, referenceDate(), -1));
    QCOMPARE(processes.row(2, 1).session.rxBytes, quint64(0));
    QVERIFY(processes.row(2, 1).present);

    // A rejected sample must not consume the elapsed gap.
    processes.update({makeCounter(2, u"app"_s, 1, {5'000, 5'000})}, referenceDate(), 10'000);
    QCOMPARE(processes.row(2, 1).session.rxBytes, quint64(4'000));
    QCOMPARE(processes.row(2, 1).rate.rxBytesPerSecond, 400.0);

    Processes fresh;
    QVERIFY(!fresh.update({makeCounter(2, u"app"_s, 1, {1, 1})}, QDateTime(), 0));
    QVERIFY(!fresh.monitoringStart().isValid());
    QVERIFY(fresh.rows().isEmpty());
}

void TestProcesses::idleProcessRateIsZeroAndMissingPidIgnored() {
    Processes processes;
    processes.update({makeCounter(2, u"app"_s, 1, {1'000, 1'000})}, referenceDate(), 0);
    // Idle process: counters did not move, so the rate stays zero but the
    // accumulated row survives.
    processes.update({makeCounter(2, u"app"_s, 1, {1'000, 1'000})}, referenceDate(), 1'000);
    const ProcessTraffic idle = processes.row(2, 1);
    QVERIFY(idle.present);
    QCOMPARE(idle.rate.rxBytesPerSecond, 0.0);
    QCOMPARE(idle.session.rxBytes, quint64(0));

    // A non-positive PID cannot name a process and must not create a row.
    processes.update({makeCounter(0, u"bogus"_s, 5, {100, 100})}, referenceDate(), 2'000);
    QCOMPARE(processes.rows().size(), 1);
}

QTEST_GUILESS_MAIN(TestProcesses)
#include "processes.moc"
