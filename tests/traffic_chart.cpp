#include "ui/traffic_chart.h"
#include "ui/connections.h"
#include "ui/details.h"

#include <QAction>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <limits>

using namespace Qt::StringLiterals;
using ball::ui::TrafficChart;

namespace {
QDateTime start() { return QDateTime::fromString(u"2026-10-23T12:00:00Z"_s, Qt::ISODate); }

ball::Snapshot snapshot() {
    ball::Snapshot result;
    result.started = start();
    result.at = start().addSecs(1);
    result.interfacesAvailable = true;
    result.processStatus.available = true;
    result.processStatus.stale = false;
    result.connectionStatus.available = true;
    result.connectionStatus.stale = false;
    for (int i = 0; i < 2; ++i) {
        ball::ConnectionSnapshot connection;
        connection.id = quint64(i + 1);
        connection.protocol = u"tcp"_s;
        connection.target = u"192.0.2.1:443"_s;
        connection.source = u"192.0.2.2:5000"_s;
        connection.client.pid = 101;
        connection.client.startNs = 9'007'199'254'740'994ULL + quint64(i);
        result.connections.append(connection);
    }
    ball::InterfaceSnapshot nic;
    nic.name = u"eno1"_s;
    nic.rxBps = 100;
    nic.txBps = 20;
    result.interfaces.append(nic);
    for (int i = 1; i <= 7; ++i) {
        ball::ProcessSnapshot process;
        process.pid = 100 + i;
        process.startNs = 9'007'199'254'740'993ULL + quint64(i);
        process.name = u"process-%1"_s.arg(i);
        process.present = true;
        process.rxBps = i;
        process.txBps = i == 1 ? 1000 : 0;
        result.processes.append(process);
    }
    return result;
}
} // namespace

class TestChart : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void boundsHistoryAndBuckets();
    void resetsOnlyForScopeSessionOrClockChanges();
    void interruptsWithoutInventingZeroes();
    void drawsLinesButNotAcrossGaps();
    void overviewUsesNicScopeAndExactProcessIdentity();
    void columnsCanBeRestoredWithoutResettingWidths();
};

void TestChart::boundsHistoryAndBuckets() {
    TrafficChart chart;
    for (int i = 0; i < 400; ++i) {
        chart.addSample(start().addSecs(i), start(), {u"eno1"_s}, i, 1, true);
    }
    QCOMPARE(chart.samples().size(), 301);
    QCOMPARE(chart.samples().first().at, start().addSecs(99));
    chart.addSample(start().addSecs(399).addMSecs(500), start(), {u"eno1"_s}, 900, 2, true);
    QCOMPARE(chart.samples().size(), 301);
    QCOMPARE(chart.samples().last().rx, 900.0);
    chart.addSample(start().addSecs(1000), start(), {u"eno1"_s}, 1, 2, true);
    QCOMPARE(chart.samples().size(), 1);
    QVERIFY(chart.samples().last().breakBefore);
}

void TestChart::resetsOnlyForScopeSessionOrClockChanges() {
    TrafficChart chart;
    chart.addSample(start(), start(), {u"eno1"_s, u"wlan0"_s}, 1, 2, true);
    chart.addSample(start().addSecs(1), start(), {u"wlan0"_s, u"eno1"_s, u"eno1"_s}, 1, 2, true);
    QCOMPARE(chart.samples().size(), 2);
    QVERIFY(!chart.samples().last().breakBefore);
    chart.addSample(start().addSecs(2), start(), {u"eno1"_s}, 1, 2, true);
    QCOMPARE(chart.samples().size(), 1);
    chart.addSample(start().addSecs(3), start().addSecs(2), {u"eno1"_s}, 1, 2, true);
    QCOMPARE(chart.samples().size(), 1);
    chart.addSample(start().addSecs(2), start().addSecs(2), {u"eno1"_s}, 1, 2, true);
    QCOMPARE(chart.samples().size(), 1);
    QCOMPARE(chart.samples().first().at, start().addSecs(2));
}

void TestChart::interruptsWithoutInventingZeroes() {
    TrafficChart chart;
    chart.addSample(start(), start(), {u"eno1"_s}, 100, 20, true);
    chart.addSample(start(), start(), {u"eno1"_s}, 100, 20, false);
    QCOMPARE(chart.samples().size(), 1);
    QCOMPARE(chart.samples().last().rx, 100.0);
    QVERIFY(chart.accessibleDescription().contains(u"中断"_s));
    chart.addSample(start().addSecs(1), start(), {u"eno1"_s}, 200, 30, true);
    QVERIFY(chart.samples().last().breakBefore);
    chart.addSample(start().addSecs(2), start(), {u"eno1"_s}, 200, 30, true);
    QVERIFY(!chart.samples().last().breakBefore);
    chart.addSample(start().addSecs(6), start(), {u"eno1"_s}, 200, 30, true);
    QVERIFY(chart.samples().last().breakBefore);
    const auto size = chart.samples().size();
    chart.addSample(start().addSecs(7), start(), {u"eno1"_s}, -1, 0, true);
    chart.addSample(start().addSecs(8), start(), {u"eno1"_s},
                    std::numeric_limits<double>::infinity(), 0, true);
    chart.addSample(start().addSecs(9), start(), {u"eno1"_s},
                    std::numeric_limits<double>::quiet_NaN(), 0, true);
    QCOMPARE(chart.samples().size(), size);
    chart.addSample(start().addSecs(10), start(), {u"eno1"_s}, 0, 0, true);
    QVERIFY(chart.samples().last().breakBefore);
    QCOMPARE(chart.samples().last().rx, 0.0); // A valid idle sample is not a gap.
}

void TestChart::drawsLinesButNotAcrossGaps() {
    TrafficChart chart;
    chart.resize(600, 240);
    for (int i = 0; i <= 300; ++i) {
        chart.addSample(start().addSecs(i), start(), {u"eno1"_s}, 100, 20, i < 130 || i > 170);
    }
    const auto render = [&] {
        const auto image = chart.grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        const auto color = [&](int x, int y) {
            return image.pixelColor(qRound(x * dpr), qRound(y * dpr));
        };
        // Both points are under the constant RX line; only the first has data.
        return color(240, 90) != color(338, 90);
    };
    QVERIFY(render());
    chart.setTheme(ball::ui::Theme::Graphite);
    QVERIFY(render());
}

void TestChart::overviewUsesNicScopeAndExactProcessIdentity() {
    ball::ui::Details details;
    details.show();
    auto sample = snapshot();
    details.setSnapshot(sample, true, {u"eno1"_s});
    auto* tabs = details.findChild<QTabWidget*>(u"details.tabs"_s);
    auto* chart = details.findChild<TrafficChart*>();
    auto* top = details.findChild<QTableView*>(u"details.overviewProcesses"_s);
    QCOMPARE(tabs->currentIndex(), 0);
    QCOMPARE(top->model()->rowCount(), 5);
    QCOMPARE(top->model()->index(0, 1).data().toInt(), 101); // TX-heavy process is first.
    QCOMPARE(chart->samples().last().rx, 100.0);             // Not process or IP bytes.
    top->selectRow(0);
    top->doubleClicked(top->model()->index(0, 0));
    QCOMPARE(tabs->currentIndex(), 3);
    auto* filter = details.findChild<QLabel*>(u"connections.processFilter"_s);
    QVERIFY(filter->text().contains(u"101"_s));
    auto* connections = details.findChild<QTableWidget*>(u"connections.table"_s);
    QCOMPARE(connections->rowCount(), 1);
    QCOMPARE(connections->item(0, 0)->data(Qt::UserRole).toULongLong(), quint64(1));

    sample.at = sample.at.addSecs(1);
    sample.processes[0].txBps = 6;
    details.setSnapshot(sample, true, {u"eno1"_s});
    const auto selected = top->selectionModel()->selectedRows();
    QCOMPARE(selected.size(), 1);
    QCOMPARE(top->model()->index(selected[0].row(), 1).data().toInt(), 101);
    sample.at = sample.at.addSecs(1);
    details.setSnapshot(sample, true, {u"eno1"_s, u"missing"_s});
    QVERIFY(chart->samples().isEmpty()); // Incomplete scope is not a comparable total.
    sample.at = sample.at.addSecs(1);
    sample.processStatus.available = false;
    details.setSnapshot(sample, true, {u"eno1"_s});
    QCOMPARE(top->model()->rowCount(), 0);
    QCOMPARE(chart->samples().size(), 1); // Process loss does not invalidate NIC history.
}

void TestChart::columnsCanBeRestoredWithoutResettingWidths() {
    ball::ui::Details details;
    details.setSnapshot(snapshot(), true, {u"eno1"_s});
    auto* table = details.findChild<QTableView*>(u"details.processTable"_s);
    QVERIFY(table->isColumnHidden(7));
    table->setColumnWidth(3, 222);
    bool found = false;
    QTimer::singleShot(0, &details, [&] {
        auto* menu = details.findChild<QMenu*>();
        if (!menu) {
            return;
        }
        for (auto* action : menu->actions()) {
            if (action->text() == u"本次↓"_s) {
                action->setChecked(true);
                found = true;
            }
        }
        menu->close();
    });
    Q_EMIT table->horizontalHeader()->customContextMenuRequested(QPoint());
    QVERIFY(found);
    QVERIFY(!table->isColumnHidden(7));
    details.setSnapshot(snapshot(), true, {u"eno1"_s});
    QVERIFY(!table->isColumnHidden(7));
    QCOMPARE(table->columnWidth(3), 222);
    auto* connections = details.findChild<QTableWidget*>(u"connections.table"_s);
    QVERIFY(!connections->showGrid());
    QCOMPARE(connections->verticalHeader()->defaultSectionSize(), 36);
    QVERIFY(details.findChild<QSplitter*>(u"connections.splitter"_s) != nullptr);
}

QTEST_MAIN(TestChart)
#include "traffic_chart.moc"
