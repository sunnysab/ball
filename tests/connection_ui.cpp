// ball: a network traffic HUD for KDE Wayland sessions.
// Offscreen tests for the connection query page. They use synthetic snapshots,
// so no collector, session or network access is needed.
#include "snapshot.h"
#include "ui/connections.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTableWidget>
#include <QTest>
#include <QToolButton>

#include <limits>

using ball::ConnectionProcess;
using ball::ConnectionSnapshot;
using ball::Snapshot;
using ball::ui::ConnectionsView;

namespace {

ConnectionProcess process(int pid, quint64 startNs, const QString& name) {
    ConnectionProcess out;
    out.pid = pid;
    out.startNs = startNs;
    out.name = name;
    return out;
}

ConnectionSnapshot row(quint64 id, const QString& protocol, const QString& source,
                       const QString& target) {
    ConnectionSnapshot out;
    out.id = id;
    out.protocol = protocol;
    out.app = QStringLiteral("app");
    out.state = QStringLiteral("established");
    out.source = source;
    out.target = target;
    out.firstSeen = QDateTime::currentDateTimeUtc();
    out.lastSeen = out.firstSeen;
    return out;
}

Snapshot snapshotOf(const QList<ConnectionSnapshot>& rows) {
    Snapshot snapshot;
    snapshot.at = QDateTime::currentDateTimeUtc();
    snapshot.connectionStatus.available = true;
    snapshot.connectionStatus.stale = false;
    snapshot.connections = rows;
    return snapshot;
}

QString cellText(QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item ? item->text() : QString();
}

quint64 selectedId(QTableWidget* table) {
    const int row = table->currentRow();
    if (row < 0) {
        return 0;
    }
    const QTableWidgetItem* item = table->item(row, 0);
    return item ? item->data(Qt::UserRole).toULongLong() : 0;
}

} // namespace

class ConnectionUiTest : public QObject {
    Q_OBJECT

private slots:
    void filtersThenRanksThenLimits();
    void processFilterMatchesAllThreeSources();
    void processFilterRejectsRecycledPid();
    void combinedFiltersIntersect();
    void sortingSwitchesMetric();
    void cumulativeBytesDoNotOverflowOrLosePrecision();
    void pauseFreezesRowsAndStaleHidesRates();
    void selectionIdSurvivesRefresh();
    void copySelectionWritesClipboard();
    void handlesTwoThousandRows();
    void connectionStatusControlsFreshness();
    void removingSelectionClearsDetails();
    void absentProtocolKeepsFilter();
    void subscriptionChangesClearSelection();
    void connectionStatusNotesAreVisible();
};

void ConnectionUiTest::filtersThenRanksThenLimits() {
    ConnectionsView view;
    view.resize(1200, 700);
    view.show();

    QList<ConnectionSnapshot> rows;
    for (quint64 i = 1; i <= 50; ++i) {
        ConnectionSnapshot item = row(i, QStringLiteral("tcp"), QStringLiteral("10.0.0.%1").arg(i),
                                      QStringLiteral("10.0.0.%1:443").arg(i));
        item.rxBps = double(i);
        item.txBps = 0.0;
        rows.append(item);
    }
    view.setSnapshot(snapshotOf(rows), true);

    auto* limit = view.findChild<QSpinBox*>(QStringLiteral("connections.limit"));
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    QVERIFY(limit);
    QVERIFY(table);

    limit->setValue(5);
    QCOMPARE(table->rowCount(), 5);
    // Highest total rate first, regardless of the formatted string.
    QCOMPARE(cellText(table, 0, 2), QStringLiteral("10.0.0.50"));

    limit->setValue(0);
    QCOMPARE(table->rowCount(), 50);
}

void ConnectionUiTest::processFilterMatchesAllThreeSources() {
    ConnectionSnapshot clientRow =
        row(1, QStringLiteral("tcp"), QStringLiteral("c"), QStringLiteral("s"));
    clientRow.client = process(100, 7, QStringLiteral("client-app"));
    ConnectionSnapshot serverRow =
        row(2, QStringLiteral("tcp"), QStringLiteral("c2"), QStringLiteral("s2"));
    serverRow.server = process(200, 8, QStringLiteral("server-app"));
    ConnectionSnapshot ioRow =
        row(3, QStringLiteral("udp"), QStringLiteral("c3"), QStringLiteral("s3"));
    ioRow.io.append(process(300, 9, QStringLiteral("io-app")));

    ConnectionsView view;
    view.resize(1200, 700);
    view.show();
    view.setSnapshot(snapshotOf({clientRow, serverRow, ioRow}), true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    QVERIFY(table);

    view.setProcessFilter(100, 7, QStringLiteral("client-app"));
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(selectedId(table), quint64(0)); // no explicit selection

    view.setProcessFilter(200, 8, QStringLiteral("server-app"));
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(cellText(table, 0, 0), QStringLiteral("app"));

    view.setProcessFilter(300, 9, QStringLiteral("io-app"));
    QCOMPARE(table->rowCount(), 1);

    view.clearProcessFilter();
    QCOMPARE(table->rowCount(), 3);
}

void ConnectionUiTest::processFilterRejectsRecycledPid() {
    ConnectionSnapshot reused =
        row(1, QStringLiteral("tcp"), QStringLiteral("c"), QStringLiteral("s"));
    reused.client = process(4242, 111, QStringLiteral("old"));
    ConnectionSnapshot current =
        row(2, QStringLiteral("tcp"), QStringLiteral("c2"), QStringLiteral("s2"));
    current.client = process(4242, 222, QStringLiteral("new"));

    ConnectionsView view;
    view.resize(1200, 700);
    view.show();
    view.setSnapshot(snapshotOf({reused, current}), true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    QVERIFY(table);

    view.setProcessFilter(4242, 222, QStringLiteral("new"));
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(selectedId(table), quint64(0));
    QCOMPARE(cellText(table, 0, 2), QStringLiteral("c2"));
}

void ConnectionUiTest::combinedFiltersIntersect() {
    ConnectionSnapshot keep =
        row(1, QStringLiteral("tcp"), QStringLiteral("192.168.1.5"), QStringLiteral("1.1.1.1:443"));
    keep.app = QStringLiteral("firefox");
    ConnectionSnapshot wrongProtocol =
        row(2, QStringLiteral("udp"), QStringLiteral("192.168.1.5"), QStringLiteral("1.1.1.1:443"));
    ConnectionSnapshot ended =
        row(3, QStringLiteral("tcp"), QStringLiteral("10.0.0.9"), QStringLiteral("2.2.2.2:443"));
    ended.end = QStringLiteral("timeout");

    ConnectionsView view;
    view.resize(1200, 700);
    view.show();
    view.setSnapshot(snapshotOf({keep, wrongProtocol, ended}), true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    auto* search = view.findChild<QLineEdit*>(QStringLiteral("connections.search"));
    auto* protocol = view.findChild<QComboBox*>(QStringLiteral("connections.protocol"));
    auto* state = view.findChild<QComboBox*>(QStringLiteral("connections.state"));
    QVERIFY(table && search && protocol && state);

    search->setText(QStringLiteral("firefox"));
    QCOMPARE(table->rowCount(), 1);
    search->setText(QStringLiteral("192.168"));
    QCOMPARE(table->rowCount(), 2);

    const int tcp = protocol->findData(QStringLiteral("tcp"));
    QVERIFY(tcp >= 0);
    protocol->setCurrentIndex(tcp);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(cellText(table, 0, 0), QStringLiteral("firefox"));

    state->setCurrentIndex(1); // ended only
    QCOMPARE(table->rowCount(), 0);
    search->clear();
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(cellText(table, 0, 0), QStringLiteral("app"));
}

void ConnectionUiTest::sortingSwitchesMetric() {
    ConnectionSnapshot slow =
        row(1, QStringLiteral("tcp"), QStringLiteral("a"), QStringLiteral("b"));
    slow.rxBps = 1.0;
    slow.txBps = 1.0;
    slow.retransmits = 99;
    ConnectionSnapshot fast =
        row(2, QStringLiteral("tcp"), QStringLiteral("c"), QStringLiteral("d"));
    fast.rxBps = 100.0;
    fast.txBps = 100.0;
    fast.retransmits = 0;

    ConnectionsView view;
    view.resize(1200, 700);
    view.show();
    view.setSnapshot(snapshotOf({slow, fast}), true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    auto* metric = view.findChild<QComboBox*>(QStringLiteral("connections.metric"));
    QVERIFY(table && metric);

    QCOMPARE(cellText(table, 0, 2), QStringLiteral("c")); // total rate
    metric->setCurrentIndex(4);                           // retransmits
    QCOMPARE(cellText(table, 0, 2), QStringLiteral("a"));
}

void ConnectionUiTest::cumulativeBytesDoNotOverflowOrLosePrecision() {
    // A: rx = 2^64-1, tx = 0  -> sum 2^64-1.
    // B: rx = 2^64-2, tx = 2  -> sum 2^64, wrapping to 0 as uint64 and losing
    // precision as double. The correct wide comparison must rank B first.
    ConnectionSnapshot a = row(1, QStringLiteral("tcp"), QStringLiteral("a"), QStringLiteral("b"));
    a.rxBytes = std::numeric_limits<quint64>::max();
    a.txBytes = 0;
    ConnectionSnapshot b = row(2, QStringLiteral("tcp"), QStringLiteral("c"), QStringLiteral("d"));
    b.rxBytes = std::numeric_limits<quint64>::max() - 1;
    b.txBytes = 2;

    ConnectionsView view;
    view.resize(1200, 700);
    view.show();
    view.setSnapshot(snapshotOf({a, b}), true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    auto* metric = view.findChild<QComboBox*>(QStringLiteral("connections.metric"));
    QVERIFY(table && metric);

    metric->setCurrentIndex(1); // cumulative IP bytes
    QCOMPARE(selectedId(table), quint64(0));
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("c"));
    QCOMPARE(table->item(1, 2)->text(), QStringLiteral("a"));
    QVERIFY(table->item(0, 8)->text().contains(QLatin1Char('+')));
}

void ConnectionUiTest::pauseFreezesRowsAndStaleHidesRates() {
    ConnectionSnapshot first =
        row(1, QStringLiteral("tcp"), QStringLiteral("a"), QStringLiteral("b"));
    first.rxBps = 10.0;
    ConnectionsView view;
    view.resize(1200, 700);
    view.show();
    view.setSnapshot(snapshotOf({first}), true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    auto* pause = view.findChild<QToolButton*>(QStringLiteral("connections.pause"));
    auto* summary = view.findChild<QLabel*>(QStringLiteral("connections.summary"));
    QVERIFY(table && pause && summary);

    pause->click(); // pause
    ConnectionSnapshot second =
        row(2, QStringLiteral("tcp"), QStringLiteral("c"), QStringLiteral("d"));
    view.setSnapshot(snapshotOf({second}), false); // stale while paused
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(cellText(table, 0, 2), QStringLiteral("a"));
    QCOMPARE(cellText(table, 0, 6).trimmed(), QStringLiteral("--")); // rate not live
    QVERIFY(summary->text().contains(QStringLiteral("已暂停")));
    QVERIFY(summary->text().contains(QStringLiteral("过期")));

    pause->click(); // resume shows the latest row
    QCOMPARE(cellText(table, 0, 2), QStringLiteral("c"));
}

void ConnectionUiTest::selectionIdSurvivesRefresh() {
    ConnectionSnapshot a = row(1, QStringLiteral("tcp"), QStringLiteral("a"), QStringLiteral("b"));
    ConnectionSnapshot b = row(2, QStringLiteral("tcp"), QStringLiteral("c"), QStringLiteral("d"));
    ConnectionsView view;
    view.resize(1200, 700);
    view.show();
    view.setSnapshot(snapshotOf({a, b}), true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    QVERIFY(table);

    table->selectRow(1); // ID 2
    QCOMPARE(selectedId(table), quint64(2));

    // A newer row with a higher rate reorders the table; the selection must
    // follow the ID, not the row index.
    b.rxBps = 100.0;
    a.rxBps = 50.0;
    view.setSnapshot(snapshotOf({b, a}), true);
    QCOMPARE(selectedId(table), quint64(2));
    QCOMPARE(table->currentRow(), 0);
}

void ConnectionUiTest::copySelectionWritesClipboard() {
    ConnectionSnapshot a = row(42, QStringLiteral("tcp"), QStringLiteral("10.0.0.1:1234"),
                               QStringLiteral("1.1.1.1:443"));
    a.client = process(9, 1, QStringLiteral("curl"));
    ConnectionsView view;
    view.resize(1200, 700);
    view.show();
    view.setSnapshot(snapshotOf({a}), true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    auto* copy = view.findChild<QToolButton*>(QStringLiteral("connections.copy"));
    QVERIFY(table && copy);

    table->selectRow(0);
    copy->click();
    const QString text = QApplication::clipboard()->text();
    QVERIFY(text.contains(QStringLiteral("ID: 42")));
    QVERIFY(text.contains(QStringLiteral("curl")));
    QVERIFY(text.contains(QStringLiteral("10.0.0.1:1234")));
}

void ConnectionUiTest::handlesTwoThousandRows() {
    QList<ConnectionSnapshot> rows;
    rows.reserve(2000);
    for (quint64 i = 1; i <= 2000; ++i) {
        ConnectionSnapshot item =
            row(i, QStringLiteral("tcp"), QStringLiteral("10.0.0.%1").arg(i % 250),
                QStringLiteral("1.1.1.1:443"));
        item.rxBps = double(2000 - i);
        rows.append(item);
    }
    ConnectionsView view;
    view.resize(1200, 700);
    view.show();
    view.setSnapshot(snapshotOf(rows), true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    auto* limit = view.findChild<QSpinBox*>(QStringLiteral("connections.limit"));
    QVERIFY(table && limit);
    QCOMPARE(table->rowCount(), 20); // default TopN

    limit->setValue(0);
    QCOMPARE(table->rowCount(), 2000);
    QCOMPARE(cellText(table, 0, 2), QStringLiteral("10.0.0.1")); // ID 1 has the top rate
}

void ConnectionUiTest::connectionStatusControlsFreshness() {
    ConnectionsView view;
    view.show();
    auto data =
        snapshotOf({row(1, QStringLiteral("tcp"), QStringLiteral("a"), QStringLiteral("b"))});
    data.connections.first().rxBps = 100;
    data.connectionStatus.stale = true;
    view.setSnapshot(data, true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    QCOMPARE(cellText(table, 0, 6).trimmed(), QStringLiteral("--"));
    auto* summary = view.findChild<QLabel*>(QStringLiteral("connections.summary"));
    QVERIFY(summary->text().contains(QStringLiteral("过期")));
}

void ConnectionUiTest::removingSelectionClearsDetails() {
    ConnectionsView view;
    view.show();
    view.setSnapshot(
        snapshotOf({row(1, QStringLiteral("tcp"), QStringLiteral("a"), QStringLiteral("b")),
                    row(2, QStringLiteral("udp"), QStringLiteral("c"), QStringLiteral("d"))}),
        true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    auto* search = view.findChild<QLineEdit*>(QStringLiteral("connections.search"));
    auto* details = view.findChild<QPlainTextEdit*>(QStringLiteral("connections.details"));
    table->selectRow(0);
    QVERIFY(details->toPlainText().contains(QStringLiteral("ID: 1")));
    search->setText(QStringLiteral("udp-no-match"));
    QVERIFY(details->toPlainText().isEmpty());
    search->clear();
    QVERIFY(details->toPlainText().isEmpty());
}

void ConnectionUiTest::absentProtocolKeepsFilter() {
    ConnectionsView view;
    view.show();
    view.setSnapshot(
        snapshotOf({row(1, QStringLiteral("tcp"), QStringLiteral("a"), QStringLiteral("b"))}),
        true);
    auto* protocol = view.findChild<QComboBox*>(QStringLiteral("connections.protocol"));
    protocol->setCurrentIndex(protocol->findData(QStringLiteral("tcp")));
    view.setSnapshot(
        snapshotOf({row(2, QStringLiteral("udp"), QStringLiteral("c"), QStringLiteral("d"))}),
        true);
    QCOMPARE(protocol->currentData().toString(), QStringLiteral("tcp"));
    QCOMPARE(view.findChild<QTableWidget*>(QStringLiteral("connections.table"))->rowCount(), 0);
    view.setProcessFilter(7, 8, QStringLiteral("<b>literal</b>"));
    QCOMPARE(view.findChild<QLabel*>(QStringLiteral("connections.processFilter"))->textFormat(),
             Qt::PlainText);
}

void ConnectionUiTest::subscriptionChangesClearSelection() {
    ConnectionsView view;
    view.show();
    Snapshot first =
        snapshotOf({row(7, QStringLiteral("tcp"), QStringLiteral("old"), QStringLiteral("b"))});
    first.started = QDateTime::fromMSecsSinceEpoch(1000);
    first.connectionStatus.generation = 1;
    view.setSnapshot(first, true);
    auto* table = view.findChild<QTableWidget*>(QStringLiteral("connections.table"));
    auto* details = view.findChild<QPlainTextEdit*>(QStringLiteral("connections.details"));
    table->selectRow(0);
    QVERIFY(!details->toPlainText().isEmpty());
    Snapshot next = first;
    next.connections.first().source = QStringLiteral("unrelated");
    next.connectionStatus.generation = 2;
    view.setSnapshot(next, true);
    QVERIFY(table->selectionModel()->selectedRows().isEmpty());
    QVERIFY(details->toPlainText().isEmpty());

    table->selectRow(0);
    auto* pause = view.findChild<QToolButton*>(QStringLiteral("connections.pause"));
    pause->setChecked(true);
    next.connectionStatus.generation = 3;
    view.setSnapshot(next, true);
    QVERIFY(!details->toPlainText().isEmpty());
    pause->setChecked(false);
    QVERIFY(details->toPlainText().isEmpty());

    table->selectRow(0);
    next.started = next.started.addSecs(1);
    view.setSnapshot(next, true);
    QVERIFY(details->toPlainText().isEmpty());
}

void ConnectionUiTest::connectionStatusNotesAreVisible() {
    ConnectionsView view;
    view.show();
    auto snapshot = snapshotOf({});
    snapshot.connectionStatus.notes = {QStringLiteral("precise <b>diagnostic</b>")};
    view.setSnapshot(snapshot, true);
    auto* summary = view.findChild<QLabel*>(QStringLiteral("connections.summary"));
    QCOMPARE(summary->textFormat(), Qt::PlainText);
    QVERIFY(summary->text().contains(QStringLiteral("precise <b>diagnostic</b>")));
}

QTEST_MAIN(ConnectionUiTest)
#include "connection_ui.moc"
