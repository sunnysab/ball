// ball: a network traffic HUD for KDE Wayland sessions.
// Regression tests for the bounded connection cache in daemon/connections.h.
#include "daemon/connections.h"

#include <QTest>

#include <algorithm>

using ball::Connections;
using ball::ConnectionSnapshot;

namespace {

ConnectionSnapshot makeRow(quint64 id, quint64 rx = 0, quint64 tx = 0) {
    ConnectionSnapshot row;
    row.id = id;
    row.protocol = QStringLiteral("tcp");
    row.source = QStringLiteral("10.0.0.1:1000");
    row.target = QStringLiteral("10.0.0.2:443");
    row.firstSeen = QDateTime::fromMSecsSinceEpoch(1000);
    row.lastSeen = QDateTime::fromMSecsSinceEpoch(2000);
    row.rxBytes = rx;
    row.txBytes = tx;
    return row;
}

bool hasId(const QList<ConnectionSnapshot>& rows, quint64 id) {
    return std::any_of(rows.begin(), rows.end(),
                       [id](const ConnectionSnapshot& row) { return row.id == id; });
}

const ConnectionSnapshot* findRow(const QList<ConnectionSnapshot>& rows, quint64 id) {
    for (const ConnectionSnapshot& row : rows) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

class TestConnectionCache : public QObject {
    Q_OBJECT

private slots:
    void updateReplacesByIdInsteadOfAccumulating();
    void firstSampleOnlySetsBaseline();
    void newBytesProduceRate();
    void repeatedAndBackwardSampleKeepBaseline();
    void unchangedCountersProduceZeroRate();
    void counterRegressionRebaselinesWithoutSpike();
    void finishedConnectionRatesAreZero();
    void resetClearsEverything();
    void capacityEvictsEarliestFinishedFirst();
    void capacityEvictsLeastRecentlyUpdated();
    void byteBudgetEvictsAndFlagsTruncated();
    void oversizedRowIsRejected();
};

void TestConnectionCache::updateReplacesByIdInsteadOfAccumulating() {
    Connections cache;
    cache.update(makeRow(1, 10, 5));
    cache.update(makeRow(1, 50, 40));

    const QList<ConnectionSnapshot> rows = cache.rows();
    QCOMPARE(rows.size(), qsizetype(1));
    QCOMPARE(rows.first().rxBytes, quint64(50));
    QCOMPARE(rows.first().txBytes, quint64(40));
}

void TestConnectionCache::firstSampleOnlySetsBaseline() {
    Connections cache;
    cache.update(makeRow(1, 1000, 500));
    cache.sample(1000);

    const QList<ConnectionSnapshot> rows = cache.rows();
    QCOMPARE(rows.size(), qsizetype(1));
    QCOMPARE(rows.first().rxBps, 0.0);
    QCOMPARE(rows.first().txBps, 0.0);
}

void TestConnectionCache::newBytesProduceRate() {
    Connections cache;
    cache.update(makeRow(1, 0, 0));
    cache.sample(1000);
    cache.update(makeRow(1, 100, 250));
    cache.sample(2000);

    const QList<ConnectionSnapshot> rows = cache.rows();
    const ConnectionSnapshot* row = findRow(rows, 1);
    QVERIFY(row != nullptr);
    QVERIFY(qFuzzyCompare(row->rxBps, 100.0)); // 100 bytes in 1 s
    QVERIFY(qFuzzyCompare(row->txBps, 250.0));
}

void TestConnectionCache::repeatedAndBackwardSampleKeepBaseline() {
    Connections cache;
    cache.update(makeRow(1, 100, 100));
    cache.sample(1000); // baseline
    cache.update(makeRow(1, 200, 200));
    cache.sample(1000); // repeated time: no interval

    const QList<ConnectionSnapshot> repeatedRows = cache.rows();
    const ConnectionSnapshot* repeated = findRow(repeatedRows, 1);
    QVERIFY(repeated != nullptr);
    QCOMPARE(repeated->rxBps, 0.0);

    cache.sample(500); // backward time must not reset the baseline
    const QList<ConnectionSnapshot> backwardRows = cache.rows();
    QCOMPARE(findRow(backwardRows, 1)->rxBps, 0.0);

    // The baseline is still the first sample at t=1000, so 100 bytes from
    // t=1000 to t=2000 is 100 B/s.
    cache.sample(2000);
    const QList<ConnectionSnapshot> rows = cache.rows();
    const ConnectionSnapshot* row = findRow(rows, 1);
    QVERIFY(qFuzzyCompare(row->rxBps, 100.0));
    QVERIFY(qFuzzyCompare(row->txBps, 100.0));
}

void TestConnectionCache::unchangedCountersProduceZeroRate() {
    Connections cache;
    cache.update(makeRow(1, 500, 500));
    cache.sample(1000);
    cache.update(makeRow(1, 500, 500));
    cache.sample(2000);

    const QList<ConnectionSnapshot> rows = cache.rows();
    const ConnectionSnapshot* row = findRow(rows, 1);
    QVERIFY(row != nullptr);
    QCOMPARE(row->rxBps, 0.0);
    QCOMPARE(row->txBps, 0.0);
}

void TestConnectionCache::counterRegressionRebaselinesWithoutSpike() {
    Connections cache;
    cache.update(makeRow(1, 1000, 1000));
    cache.sample(1000);               // baseline
    cache.update(makeRow(1, 50, 50)); // collector counter went backwards
    cache.sample(2000);

    const QList<ConnectionSnapshot> regressed = cache.rows();
    const ConnectionSnapshot* row = findRow(regressed, 1);
    QVERIFY(row != nullptr);
    QCOMPARE(row->rxBps, 0.0);
    QCOMPARE(row->txBps, 0.0);

    // The new baseline is 50, so 100 more bytes in 1 s is 100 B/s.
    cache.update(makeRow(1, 150, 150));
    cache.sample(3000);
    const QList<ConnectionSnapshot> rows = cache.rows();
    row = findRow(rows, 1);
    QVERIFY(row != nullptr);
    QVERIFY(qFuzzyCompare(row->rxBps, 100.0));
    QVERIFY(qFuzzyCompare(row->txBps, 100.0));
}

void TestConnectionCache::finishedConnectionRatesAreZero() {
    Connections cache;
    cache.update(makeRow(1, 0, 0));
    cache.sample(1000);
    cache.update(makeRow(1, 1000, 1000));

    ConnectionSnapshot finished = makeRow(1, 1000, 1000);
    finished.end = QStringLiteral("reset");
    cache.update(finished);
    cache.sample(2000);

    const QList<ConnectionSnapshot> rows = cache.rows();
    const ConnectionSnapshot* row = findRow(rows, 1);
    QVERIFY(row != nullptr);
    QCOMPARE(row->rxBps, 0.0);
    QCOMPARE(row->txBps, 0.0);
}

void TestConnectionCache::resetClearsEverything() {
    Connections cache;
    cache.update(makeRow(1, 10, 10));
    cache.sample(1000);
    cache.update(makeRow(2, 20, 20));
    cache.sample(2000);
    cache.reset();

    QCOMPARE(cache.rows().size(), qsizetype(0));
    QVERIFY(!cache.truncated());

    // After reset the first sample of a new row is a baseline again.
    cache.update(makeRow(3, 100, 100));
    cache.sample(3000);
    const QList<ConnectionSnapshot> baselineRows = cache.rows();
    const ConnectionSnapshot* row = findRow(baselineRows, 3);
    QVERIFY(row != nullptr);
    QCOMPARE(row->rxBps, 0.0);

    cache.update(makeRow(3, 200, 200));
    cache.sample(4000);
    const QList<ConnectionSnapshot> rows = cache.rows();
    QVERIFY(qFuzzyCompare(findRow(rows, 3)->rxBps, 100.0));
}

void TestConnectionCache::capacityEvictsEarliestFinishedFirst() {
    Connections cache;
    for (quint64 id = 1; id <= quint64(Connections::kMaxConnections); ++id) {
        cache.update(makeRow(id));
    }

    ConnectionSnapshot early = makeRow(7000);
    early.end = QStringLiteral("timeout");
    early.lastSeen = QDateTime::fromMSecsSinceEpoch(1000);
    cache.update(early);

    ConnectionSnapshot later = makeRow(6000);
    later.end = QStringLiteral("timeout");
    later.lastSeen = QDateTime::fromMSecsSinceEpoch(5000);
    cache.update(later);

    cache.update(makeRow(8193));

    const QList<ConnectionSnapshot> rows = cache.rows();
    QCOMPARE(rows.size(), qsizetype(Connections::kMaxConnections));
    QVERIFY(hasId(rows, 8193));
    QVERIFY(!hasId(rows, 7000));
    QVERIFY(hasId(rows, 6000));
    QVERIFY(cache.truncated());
}

void TestConnectionCache::capacityEvictsLeastRecentlyUpdated() {
    Connections cache;
    for (quint64 id = 1; id <= quint64(Connections::kMaxConnections); ++id) {
        cache.update(makeRow(id));
    }
    cache.update(makeRow(1)); // id 1 is now the most recently updated

    cache.update(makeRow(8193));

    const QList<ConnectionSnapshot> rows = cache.rows();
    QCOMPARE(rows.size(), qsizetype(Connections::kMaxConnections));
    QVERIFY(hasId(rows, 1));
    QVERIFY(!hasId(rows, 2));
    QVERIFY(hasId(rows, 8193));
}

void TestConnectionCache::byteBudgetEvictsAndFlagsTruncated() {
    Connections cache;
    // Eight rows of ~1 MiB text cost already reach the 8 MiB budget, so a
    // ninth must evict rather than grow the cache.
    const QString payload(1024 * 1024, QLatin1Char('a'));
    for (quint64 id = 1; id <= 9; ++id) {
        ConnectionSnapshot row = makeRow(id);
        row.detail = payload;
        cache.update(row);
    }

    const QList<ConnectionSnapshot> rows = cache.rows();
    QVERIFY(rows.size() < 9);
    QVERIFY(hasId(rows, 9));
    QVERIFY(!hasId(rows, 1));
    QVERIFY(cache.truncated());
}

void TestConnectionCache::oversizedRowIsRejected() {
    Connections cache;
    ConnectionSnapshot row = makeRow(1);
    row.detail = QString(9 * 1024 * 1024, QLatin1Char('a'));
    cache.update(row);

    QCOMPARE(cache.rows().size(), qsizetype(0));
    QVERIFY(cache.truncated());
}

QTEST_GUILESS_MAIN(TestConnectionCache)
#include "connection_cache.moc"
