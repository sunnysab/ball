// ball: a network traffic HUD for KDE Wayland sessions.
// Offscreen tests for the snapshot parser, the read-only socket client and the
// HUD widgets. A real QLocalServer mock stands in for the backend, so no root
// and no network access are needed; the layer-shell protocol itself is verified
// in a Wayland session instead.
#include "process.h"
#include "ui/autostart.h"
#include "ui/details.h"
#include "ui/format.h"
#include "ui/hud.h"
#include "ui/placement.h"
#include "ui/preferences.h"
#include "ui/snapshot.h"
#include "ui/snapshot_client.h"
#include "ui/traffic_model.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QFile>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QUuid>

#include <cmath>
#include <limits>
#include <unistd.h>

using ball::ui::Hud;
using ball::ui::InterfaceKind;
using ball::ui::InterfaceSnapshot;
using ball::ui::ProcessSnapshot;
using ball::ui::Snapshot;
using ball::ui::SnapshotClient;
using ball::ui::SnapshotParseResult;
using namespace Qt::StringLiterals;

namespace {

// One snapshot frame with two physical interfaces, one loopback and three
// processes, so the defaults (physical only, idle hidden) are observable.
QByteArray snapshotLine(const QString& at = u"2026-10-14T14:30:02+08:00"_s,
                        bool interfacesAvailable = true, bool processAvailable = true,
                        bool processStale = false) {
    const QJsonArray interfaces{
        QJsonObject{
            {u"index"_s, 2},
            {u"name"_s, u"eno1"_s},
            {u"kind"_s, u"physical"_s},
            {u"up"_s, true},
            {u"today"_s, QJsonObject{{u"rx_bytes"_s, u"2048"_s}, {u"tx_bytes"_s, u"512"_s}}},
            {u"session"_s, QJsonObject{{u"rx_bytes"_s, u"2048"_s}, {u"tx_bytes"_s, u"512"_s}}},
            {u"rate"_s, QJsonObject{{u"rx_bps"_s, 1024.0}, {u"tx_bps"_s, 256.0}}}},
        QJsonObject{{u"index"_s, 3},
                    {u"name"_s, u"wlan0"_s},
                    {u"kind"_s, u"physical"_s},
                    {u"up"_s, true},
                    {u"today"_s,
                     QJsonObject{{u"rx_bytes"_s, u"9007199254740993"_s}, {u"tx_bytes"_s, u"0"_s}}},
                    {u"session"_s,
                     QJsonObject{{u"rx_bytes"_s, u"9007199254740993"_s}, {u"tx_bytes"_s, u"0"_s}}},
                    {u"rate"_s, QJsonObject{{u"rx_bps"_s, 500.0}, {u"tx_bps"_s, 0.0}}}},
        QJsonObject{{u"index"_s, 1},
                    {u"name"_s, u"lo"_s},
                    {u"kind"_s, u"loopback"_s},
                    {u"up"_s, true},
                    {u"today"_s, QJsonObject{{u"rx_bytes"_s, u"77"_s}, {u"tx_bytes"_s, u"77"_s}}},
                    {u"session"_s, QJsonObject{{u"rx_bytes"_s, u"77"_s}, {u"tx_bytes"_s, u"77"_s}}},
                    {u"rate"_s, QJsonObject{{u"rx_bps"_s, 9.0}, {u"tx_bps"_s, 9.0}}}},
        QJsonObject{{u"index"_s, 4},
                    {u"name"_s, u"tun0"_s},
                    {u"kind"_s, u"virtual"_s},
                    {u"up"_s, false},
                    {u"today"_s, QJsonObject{{u"rx_bytes"_s, u"5"_s}, {u"tx_bytes"_s, u"5"_s}}},
                    {u"session"_s, QJsonObject{{u"rx_bytes"_s, u"5"_s}, {u"tx_bytes"_s, u"5"_s}}},
                    {u"rate"_s, QJsonObject{{u"rx_bps"_s, 0.0}, {u"tx_bps"_s, 0.0}}}}};

    const QJsonArray processes{
        QJsonObject{
            {u"pid"_s, 1234},
            {u"start_ns"_s, u"9007199254740993"_s},
            {u"name"_s, u"firefox"_s},
            {u"present"_s, true},
            {u"today"_s, QJsonObject{{u"rx_bytes"_s, u"1024"_s}, {u"tx_bytes"_s, u"128"_s}}},
            {u"session"_s, QJsonObject{{u"rx_bytes"_s, u"1024"_s}, {u"tx_bytes"_s, u"128"_s}}},
            {u"rate"_s, QJsonObject{{u"rx_bps"_s, 512.0}, {u"tx_bps"_s, 64.0}}}},
        QJsonObject{{u"pid"_s, 42},
                    {u"start_ns"_s, u"1"_s},
                    {u"name"_s, u"code"_s},
                    {u"present"_s, true},
                    {u"today"_s, QJsonObject{{u"rx_bytes"_s, u"99"_s}, {u"tx_bytes"_s, u"99"_s}}},
                    {u"session"_s, QJsonObject{{u"rx_bytes"_s, u"99"_s}, {u"tx_bytes"_s, u"99"_s}}},
                    {u"rate"_s, QJsonObject{{u"rx_bps"_s, 9.0}, {u"tx_bps"_s, 9.0}}}},
        QJsonObject{{u"pid"_s, 7},
                    {u"start_ns"_s, u"2"_s},
                    {u"name"_s, u"dead"_s},
                    {u"present"_s, false},
                    {u"today"_s, QJsonObject{{u"rx_bytes"_s, u"7"_s}, {u"tx_bytes"_s, u"7"_s}}},
                    {u"session"_s, QJsonObject{{u"rx_bytes"_s, u"7"_s}, {u"tx_bytes"_s, u"7"_s}}},
                    {u"rate"_s, QJsonObject{{u"rx_bps"_s, 0.0}, {u"tx_bps"_s, 0.0}}}}};

    const QJsonObject snapshot{
        {u"version"_s, 1},
        {u"kind"_s, u"snapshot"_s},
        {u"started"_s, u"2026-10-14T14:30:00+08:00"_s},
        {u"at"_s, at},
        {u"interfaces_available"_s, interfacesAvailable},
        {u"interface_error"_s, interfacesAvailable ? u""_s : u"rtnetlink dump failed"_s},
        {u"process_status"_s, QJsonObject{{u"available"_s, processAvailable},
                                          {u"stale"_s, processStale},
                                          {u"incomplete"_s, false},
                                          {u"error"_s, u""_s},
                                          {u"notes"_s, QJsonArray{}}}},
        {u"interfaces"_s, interfaces},
        {u"processes"_s, processes}};
    return QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
}

// Runs the event loop until `predicate` is true or `timeoutMs` elapses.
template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QTest::qWait(10);
    }
    return predicate();
}

QString findText(QAbstractItemModel* model, int row, int column) {
    return model->index(row, column).data(Qt::DisplayRole).toString();
}

} // namespace

class TestUi : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    // Parser
    void parsesFullSnapshot();
    void keepsLargeDecimalStringsExact();
    void rejectsBadFrames();
    void rejectsTimestampWithoutZone();

    // Client
    void receivesInitialSnapshot();
    void handlesSplitAndMultipleFrames();
    void rejectedFrameKeepsLastSnapshot();
    void disconnectMarksStale();
    void oversizedFrameDropsConnection();

    // Aggregation
    void sumsOnlySelectedInterfaces();
    void aggregateRejectsOverflowAndCountsUniqueNames();
    void emptyInterfaceSelectionSurvivesSnapshotsAndRestart();
    void defaultSelectionIsEnabledPhysicalOnly();

    // Formatting and placement
    void formatsRatesWithFixedWidth();
    void unavailableValueIsNotZero();
    void clampsIntoVisibleArea();
    void snapsToScreenEdge();

    // Preferences
    void preferencesRoundTrip();

    // Widgets
    void showsDigitsFromSnapshot();
    void switchesTabsAndSortsProcesses();
    void filtersIdleProcessesAndSearch();
    void interfaceSelectionExcludesLoopback();
    void clickOpensIndependentDetailsAndDragDoesNot();
    void lockedPositionStillOpensDetails();
    void waylandDragTracksRequestedPosition();
    void waylandContinuousMotionTracksSurfaceMovement();
    void waylandLockedClickAndDragAreDistinct();
    void waylandDragSavesRequestedPosition();
    void waylandDragRequestsSurfaceCommit();
    void detailsSurviveDeactivationAndEscapeClosesOnlyDetails();
    void remembersPositionAndOpacity();
    void appearanceRoundTripAndInvalidPreferences();
    void barLayoutsStayCompactAndDoNotClip();
    void themeChangesAreLocalAndKeepTextLegible();
    void fontChangesResizeTheBarWithoutClipping();
    void allMetricsAreVisibleAndSortNumerically();
    void commandLineFormattingPreservesArguments();
    void processTooltipReadsVerifiedCommandLineAndEscapesMarkup();
    void processQualityDoesNotInvalidateInterfaceRates();
    void partialInterfaceSelectionIsVisible();
    void staleRatesAreUnavailableInBothWindows();
    void detailsGeometrySurvivesRestart();
    void appearanceMenuUpdatesPreferences();
    void contextMenuStaysCompact();
    void autostartMenuUsesUserConfiguration();

private:
    QString startServer();
    QLocalSocket* acceptConnection(int timeoutMs = 3000);
    void send(const QByteArray& raw, bool newline = true);

    QLocalServer m_server;
    QLocalSocket* m_client = nullptr;
    QString m_name;
    QTemporaryDir m_settingsDir;
    QString m_settingsPath;
};

namespace {

// A Hud that pretends the compositor owns the surface position, which is what
// layer-shell does on Wayland. Offscreen this lets a test drive the real drag
// math while QWidget::pos() stays unrelated to the requested position.
class WaylandHud : public Hud {
public:
    using Hud::Hud;
    bool compositorOwnsPosition() const override { return true; }
    int paintCount = 0;

protected:
    void paintEvent(QPaintEvent* event) override {
        ++paintCount;
        Hud::paintEvent(event);
    }
};

} // namespace

void TestUi::init() {
    QTest::failOnWarning(QRegularExpression(u"Could not parse stylesheet.*"_s));
    m_name = u"ball-ui-%1"_s.arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(m_server.listen(m_name));
    m_settingsPath = m_settingsDir.filePath(m_name + u".ini"_s);
}

void TestUi::cleanup() {
    delete m_client;
    m_client = nullptr;
    m_server.close();
    QLocalServer::removeServer(m_name);
}

QString TestUi::startServer() { return m_server.serverName(); }

QLocalSocket* TestUi::acceptConnection(int timeoutMs) {
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

void TestUi::send(const QByteArray& raw, bool newline) {
    QLocalSocket* socket = m_client;
    QVERIFY(socket != nullptr);
    if (socket == nullptr) {
        return; // the caller's acceptConnection() already failed the test
    }
    QByteArray payload = raw;
    if (newline) {
        payload.append('\n');
    }
    QCOMPARE(socket->write(payload), qint64(payload.size()));
    socket->flush();
}

void TestUi::parsesFullSnapshot() {
    const SnapshotParseResult result = ball::ui::parseSnapshot(snapshotLine());
    QVERIFY2(result.snapshot.has_value(), qPrintable(result.error));
    const Snapshot snapshot = result.snapshot.value_or(Snapshot{});
    QCOMPARE(snapshot.version, 1);
    QVERIFY(snapshot.interfacesAvailable);
    QVERIFY(snapshot.processStatus.available);
    QCOMPARE(snapshot.interfaces.size(), 4);
    QCOMPARE(snapshot.processes.size(), 3);
    QCOMPARE(snapshot.interfaces.at(0).name, u"eno1"_s);
    QCOMPARE(snapshot.interfaces.at(0).kind, InterfaceKind::Physical);
    QCOMPARE(snapshot.interfaces.at(0).today.rxBytes, quint64(2048));
    QCOMPARE(snapshot.interfaces.at(2).kind, InterfaceKind::Loopback);
    QCOMPARE(snapshot.interfaces.at(3).kind, InterfaceKind::Virtual);
    QCOMPARE(snapshot.processes.at(0).pid, 1234);
    QCOMPARE(snapshot.processes.at(2).present, false);
}

void TestUi::keepsLargeDecimalStringsExact() {
    const SnapshotParseResult result = ball::ui::parseSnapshot(snapshotLine());
    QVERIFY(result.snapshot.has_value());
    // 9007199254740993 is 2^53 + 1, which no double can represent.
    const Snapshot snapshot = result.snapshot.value_or(Snapshot{});
    QCOMPARE(snapshot.interfaces.at(1).today.rxBytes, quint64(9007199254740993ULL));
    QCOMPARE(snapshot.processes.at(0).startNs, quint64(9007199254740993ULL));
}

void TestUi::rejectsBadFrames() {
    const auto rejected = [](const QByteArray& line) {
        return !ball::ui::parseSnapshot(line).snapshot.has_value();
    };
    QVERIFY(rejected("{oops"));
    QVERIFY(rejected("[1,2]"));
    QVERIFY(rejected("{}"));
    // Wrong version, wrong kind and missing quality fields are all bad frames.
    QJsonObject object = QJsonDocument::fromJson(snapshotLine()).object();
    object[u"version"_s] = 2;
    QVERIFY(rejected(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    object = QJsonDocument::fromJson(snapshotLine()).object();
    object[u"kind"_s] = u"hello"_s;
    QVERIFY(rejected(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    object = QJsonDocument::fromJson(snapshotLine()).object();
    object.remove(u"process_status"_s);
    QVERIFY(rejected(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    // A negative byte count and a non-decimal string must not be accepted.
    object = QJsonDocument::fromJson(snapshotLine()).object();
    QJsonObject snapshot = object[u"snapshot"_s].toObject();
    QJsonArray interfaces = object[u"interfaces"_s].toArray();
    QJsonObject first = interfaces.at(0).toObject();
    first[u"today"_s] = QJsonObject{{u"rx_bytes"_s, u"1.5"_s}, {u"tx_bytes"_s, u"0"_s}};
    interfaces.replace(0, first);
    object[u"interfaces"_s] = interfaces;
    QVERIFY(rejected(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void TestUi::rejectsTimestampWithoutZone() {
    QJsonObject object = QJsonDocument::fromJson(snapshotLine()).object();
    object[u"at"_s] = u"2026-10-14T14:30:02"_s;
    QVERIFY(!ball::ui::parseSnapshot(QJsonDocument(object).toJson(QJsonDocument::Compact))
                 .snapshot.has_value());
    // Payment for the "at" field is not a time either.
    object[u"at"_s] = u"not a time"_s;
    QVERIFY(!ball::ui::parseSnapshot(QJsonDocument(object).toJson(QJsonDocument::Compact))
                 .snapshot.has_value());
}

void TestUi::receivesInitialSnapshot() {
    SnapshotClient client;
    QSignalSpy spy(&client, &SnapshotClient::updated);
    QVERIFY(spy.isValid());
    client.start(startServer());
    QLocalSocket* socket = acceptConnection();
    QVERIFY(socket != nullptr);
    send(snapshotLine());
    QVERIFY(waitFor([&client] { return client.fresh(); }));
    QVERIFY(client.snapshot().has_value());
    QCOMPARE(client.snapshot().value_or(Snapshot{}).interfaces.at(0).name, u"eno1"_s);
    QVERIFY(client.error().isEmpty());
}

void TestUi::handlesSplitAndMultipleFrames() {
    SnapshotClient client;
    client.start(startServer());
    QVERIFY(acceptConnection() != nullptr);

    const QByteArray first = snapshotLine();
    const QByteArray second = snapshotLine(u"2026-10-14T14:30:03+08:00"_s);
    // Half a frame, then the rest plus a whole second frame in one write.
    const qsizetype split = first.size() / 2;
    send(first.left(split), false);
    QTest::qWait(30);
    QCOMPARE(client.snapshot().has_value(), false);
    QByteArray tail = first.mid(split);
    tail.append('\n');
    tail.append(second);
    send(tail, true);

    QVERIFY(waitFor([&client] { return client.fresh(); }));
    // Only the newest whole snapshot is kept: the second frame's `at` wins.
    QCOMPARE(client.snapshot().value_or(Snapshot{}).at,
             QDateTime::fromString(u"2026-10-14T14:30:03+08:00"_s, Qt::ISODate));
    QCOMPARE(client.incomplete(), false);
}

void TestUi::rejectedFrameKeepsLastSnapshot() {
    SnapshotClient client;
    client.start(startServer());
    QVERIFY(acceptConnection() != nullptr);
    send(snapshotLine());
    QVERIFY(waitFor([&client] { return client.fresh(); }));
    const QDateTime kept = client.snapshot().value_or(Snapshot{}).at;

    send("{oops");
    QVERIFY(waitFor([&client] { return !client.error().isEmpty(); }));
    // The good snapshot is still there; a bad frame never partially updates it.
    QVERIFY(client.snapshot().has_value());
    QCOMPARE(client.snapshot().value_or(Snapshot{}).at, kept);
    QVERIFY(client.incomplete());
    QVERIFY(!client.fresh());
}

void TestUi::disconnectMarksStale() {
    SnapshotClient client;
    client.start(startServer());
    QLocalSocket* socket = acceptConnection();
    QVERIFY(socket != nullptr);
    send(snapshotLine());
    QVERIFY(waitFor([&client] { return client.fresh(); }));

    socket->disconnectFromServer();
    QVERIFY(waitFor([&client] { return !client.fresh(); }));
    QVERIFY(client.incomplete());
    // The last snapshot stays visible, but it is no longer presented as live.
    QVERIFY(client.snapshot().has_value());
}

void TestUi::oversizedFrameDropsConnection() {
    SnapshotClient client;
    client.start(startServer());
    QLocalSocket* socket = acceptConnection();
    QVERIFY(socket != nullptr);
    if (socket == nullptr) {
        return; // acceptConnection() already failed the test
    }
    // More than kMaxLineBytes without a newline must not grow memory or be
    // parsed; the client drops the connection instead.
    QByteArray huge(SnapshotClient::kMaxLineBytes + 1024, 'x');
    huge.prepend("{\"kind\":\"snapshot\",\"pad\":\"");
    QTest::qWait(10);
    QVERIFY(socket->write(huge) > 0);
    socket->flush();
    QVERIFY(waitFor([&client] { return !client.error().isEmpty(); }, 5000));
    QVERIFY(!client.snapshot().has_value());
}

void TestUi::sumsOnlySelectedInterfaces() {
    const SnapshotParseResult result = ball::ui::parseSnapshot(snapshotLine());
    QVERIFY(result.snapshot.has_value());
    const QList<InterfaceSnapshot>& interfaces = result.snapshot.value_or(Snapshot{}).interfaces;

    const ball::ui::InterfaceTotals totals =
        ball::ui::sumSelectedInterfaces(interfaces, {u"eno1"_s, u"wlan0"_s});
    QVERIFY(totals.valid);
    QVERIFY(totals.complete);
    // loopback and virtual rows contribute nothing.
    QCOMPARE(totals.today.rxBytes, quint64(2048) + quint64(9007199254740993ULL));
    QCOMPARE(totals.rxBps, 1524.0);

    const ball::ui::InterfaceTotals loopback =
        ball::ui::sumSelectedInterfaces(interfaces, {u"lo"_s});
    QVERIFY(loopback.valid);
    QCOMPARE(loopback.rxBps, 9.0);

    // A name that is not in the snapshot must not fabricate a zero total.
    const ball::ui::InterfaceTotals missing =
        ball::ui::sumSelectedInterfaces(interfaces, {u"eno1"_s, u"missing0"_s});
    QVERIFY(missing.valid);
    QVERIFY(!missing.complete);
    QCOMPARE(missing.rxBps, 1024.0);
    QVERIFY(!ball::ui::sumSelectedInterfaces(interfaces, {}).valid);
}

void TestUi::aggregateRejectsOverflowAndCountsUniqueNames() {
    const auto parsed = ball::ui::parseSnapshot(snapshotLine());
    QVERIFY(parsed.snapshot.has_value());
    auto row = parsed.snapshot.value_or(Snapshot{}).interfaces.first();
    auto retired = row;
    retired.index = row.index + 100;
    retired.up = false;
    QList<InterfaceSnapshot> rows{row, retired};
    const auto summed = ball::ui::sumSelectedInterfaces(rows, {row.name, row.name});
    QVERIFY(summed.valid);
    QVERIFY(summed.complete);
    QCOMPARE(summed.presentCount, 1);
    QCOMPARE(summed.selectedCount, 1);
    QCOMPARE(summed.today.rxBytes, 2 * row.today.rxBytes);

    rows[0].today.rxBytes = std::numeric_limits<quint64>::max();
    rows[1].today.rxBytes = 1;
    QVERIFY(!ball::ui::sumSelectedInterfaces(rows, {row.name}).valid);
    rows[0].today = {};
    rows[1].today = {};
    rows[0].rxBps = std::numeric_limits<double>::max();
    rows[1].rxBps = std::numeric_limits<double>::max();
    QVERIFY(!ball::ui::sumSelectedInterfaces(rows, {row.name}).valid);
}

void TestUi::emptyInterfaceSelectionSurvivesSnapshotsAndRestart() {
    {
        QSettings settings(m_settingsPath, QSettings::IniFormat);
        Hud hud(startServer(), settings);
        QVERIFY(acceptConnection());
        send(snapshotLine());
        QVERIFY(waitFor([&hud] { return !hud.selectedInterfaces().isEmpty(); }));
        hud.setSelectedInterfaces({});
        QVERIFY(hud.selectedInterfaces().isEmpty());
        QSignalSpy changed(&hud, &Hud::snapshotChanged);
        send(snapshotLine());
        QVERIFY(waitFor([&changed] { return !changed.isEmpty(); }));
        QVERIFY(hud.selectedInterfaces().isEmpty());
    }
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    QVERIFY(ball::ui::Preferences::load(settings).hasInterfaceSelection);
    Hud restored(startServer(), settings);
    QVERIFY(acceptConnection());
    QSignalSpy changed(&restored, &Hud::snapshotChanged);
    send(snapshotLine());
    QVERIFY(waitFor([&changed] { return !changed.isEmpty(); }));
    QVERIFY(restored.selectedInterfaces().isEmpty());
}

void TestUi::defaultSelectionIsEnabledPhysicalOnly() {
    const SnapshotParseResult result = ball::ui::parseSnapshot(snapshotLine());
    QVERIFY(result.snapshot.has_value());
    const QStringList defaults =
        ball::ui::defaultInterfaceSelection(result.snapshot.value_or(Snapshot{}).interfaces);
    QCOMPARE(defaults, QStringList({u"eno1"_s, u"wlan0"_s}));
    QVERIFY(!defaults.contains(u"lo"_s));
    QVERIFY(!defaults.contains(u"tun0"_s));
}

void TestUi::formatsRatesWithFixedWidth() {
    const QString kibs = ball::ui::formatRate(1024.0);
    const QString mibs = ball::ui::formatRate(2.5 * 1024 * 1024);
    const QString bytes = ball::ui::formatRate(10.0);
    QVERIFY(kibs.contains(u"KiB/s"_s));
    QVERIFY(mibs.contains(u"MiB/s"_s));
    QVERIFY(bytes.contains(u"B/s"_s));
    QVERIFY(kibs.contains(u"1.00"_s));
    QVERIFY(mibs.contains(u"2.50"_s));
    // A fixed width keeps the HUD from jumping as the number grows.
    QCOMPARE(kibs.size(), mibs.size());
    QCOMPARE(kibs.size(), bytes.size());
    QCOMPARE(ball::ui::formatBytes(9007199254740993ULL).size(), ball::ui::formatBytes(0).size());
}

void TestUi::unavailableValueIsNotZero() {
    const QString unavailable = ball::ui::unavailableRate();
    QVERIFY(unavailable.contains(QLatin1Char('-')));
    QVERIFY(!unavailable.contains(QLatin1Char('0')));
    // A non-finite rate is shown as unavailable, never as 0.00 B/s.
    QCOMPARE(ball::ui::formatRate(std::nan("")), unavailable);
    QCOMPARE(ball::ui::formatRate(-1.0), unavailable);
    QCOMPARE(ball::ui::formatRate(std::numeric_limits<double>::infinity()), unavailable);
}

void TestUi::clampsIntoVisibleArea() {
    const QRect screen(0, 0, 1920, 1080);
    const QRect window(1800, 1000, 300, 120);
    const QRect clamped = ball::ui::clampRectInto(window, screen);
    QCOMPARE(clamped.right(), screen.right());
    QCOMPARE(clamped.bottom(), screen.bottom());
    // A window larger than the screen is pinned at the top-left, not pushed off.
    const QRect oversized = ball::ui::clampRectInto(QRect(100, 100, 3000, 2000), screen);
    QCOMPARE(oversized.topLeft(), screen.topLeft());
    QVERIFY(oversized.right() <= screen.right());
}

void TestUi::snapsToScreenEdge() {
    const QRect screen(0, 0, 1920, 1080);
    QCOMPARE(ball::ui::snappedPosition(QRect(8, 300, 200, 40), screen, 14).x(), 0);
    QCOMPARE(ball::ui::snappedPosition(QRect(1710, 300, 200, 40), screen, 14).x(), 1920 - 200);
    // Far from every edge: only clamped, not snapped to a corner.
    QCOMPARE(ball::ui::snappedPosition(QRect(600, 400, 200, 40), screen, 14), QPoint(600, 400));
}

void TestUi::preferencesRoundTrip() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    ball::ui::Preferences saved;
    saved.hasPosition = true;
    saved.position = QPoint(321, 654);
    saved.screenName = u"HDMI-A-1"_s;
    saved.opacity = 0.7;
    saved.interfaces = {u"eno1"_s, u"wlan0"_s};
    saved.save(settings);
    settings.sync();

    QSettings reread(m_settingsPath, QSettings::IniFormat);
    const ball::ui::Preferences loaded = ball::ui::Preferences::load(reread);
    QCOMPARE(loaded.position, saved.position);
    QCOMPARE(loaded.screenName, saved.screenName);
    QCOMPARE(loaded.opacity, saved.opacity);
    QCOMPARE(loaded.interfaces, saved.interfaces);
    QVERIFY(loaded.hasPosition);

    // A corrupted opacity must not be able to make the HUD invisible.
    reread.setValue(u"hud/opacity"_s, 0.0);
    QCOMPARE(ball::ui::Preferences::load(reread).opacity, 0.96);
}

void TestUi::showsDigitsFromSnapshot() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    QVERIFY(acceptConnection() != nullptr);
    send(snapshotLine());

    auto* rx = hud.findChild<QLabel*>(u"hud.rx"_s);
    auto* status = hud.findChild<QLabel*>(u"hud.status"_s);
    QVERIFY(rx != nullptr);
    QVERIFY(status != nullptr);
    // Default selection is eno1 + wlan0: 1024 + 500 = 1524 B/s down.
    QVERIFY(waitFor([rx] { return rx->text().contains(u"1.49"_s); }));
    QCOMPARE(rx->text(), u"1.49"_s);
    QVERIFY(status->text().isEmpty());
}

void TestUi::switchesTabsAndSortsProcesses() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    QVERIFY(acceptConnection() != nullptr);
    send(snapshotLine());
    QVERIFY(waitFor([&hud] {
        return hud.detailsWindow()->findChild<QLabel*>(u"details.currentDown"_s) != nullptr &&
               hud.detailsWindow()
                   ->findChild<QLabel*>(u"details.currentDown"_s)
                   ->text()
                   .contains(u"1.49"_s);
    }));

    auto* tabs = hud.detailsWindow()->findChild<QTabWidget*>(u"details.tabs"_s);
    auto* table = hud.detailsWindow()->findChild<QTableView*>(u"details.processTable"_s);
    QVERIFY(tabs != nullptr);
    QVERIFY(table != nullptr);
    QCOMPARE(tabs->tabText(0), u"进程"_s);
    QCOMPARE(tabs->tabText(1), u"网卡"_s);

    // firefox (512 B/s) sorts above code (9 B/s) with the real rate column.
    QAbstractItemModel* model = table->model();
    QVERIFY(waitFor([model] { return model->rowCount() == 2; }));
    QCOMPARE(findText(model, 0, 0), u"firefox"_s);
    QCOMPARE(findText(model, 1, 0), u"code"_s);
}

void TestUi::filtersIdleProcessesAndSearch() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    QVERIFY(acceptConnection() != nullptr);
    send(snapshotLine());
    auto* table = hud.detailsWindow()->findChild<QTableView*>(u"details.processTable"_s);
    auto* search = hud.detailsWindow()->findChild<QLineEdit*>(u"details.search"_s);
    auto* showIdle = hud.detailsWindow()->findChild<QCheckBox*>(u"details.showIdle"_s);
    QVERIFY(table != nullptr && search != nullptr && showIdle != nullptr);
    QVERIFY(waitFor([table] { return table->model()->rowCount() == 2; }));

    // "dead" is not running: hidden by default, visible and still counted when
    // the user asks for idle rows.
    showIdle->setChecked(true);
    QCOMPARE(table->model()->rowCount(), 3);

    search->setText(u"cod"_s);
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(findText(table->model(), 0, 0), u"code"_s);

    // Searching by PID matches too.
    search->setText(u"1234"_s);
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(findText(table->model(), 0, 0), u"firefox"_s);

    search->clear();
    showIdle->setChecked(false);
    QCOMPARE(table->model()->rowCount(), 2);
}

void TestUi::interfaceSelectionExcludesLoopback() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    QVERIFY(acceptConnection() != nullptr);
    send(snapshotLine());
    QVERIFY(waitFor(
        [&hud] { return hud.selectedInterfaces() == QStringList({u"eno1"_s, u"wlan0"_s}); }));
    // The first-run default is the enabled physical interfaces only.
    QCOMPARE(hud.selectedInterfaces(), QStringList({u"eno1"_s, u"wlan0"_s}));

    auto* interfaceTable = hud.detailsWindow()->findChild<QTableView*>(u"details.interfaceTable"_s);
    QVERIFY(interfaceTable != nullptr);
    // All four rows exist in the table; only the totals use the selection. The
    // down tunnel is hidden as idle until the user asks for it.
    QVERIFY(waitFor([interfaceTable] { return interfaceTable->model()->rowCount() == 3; }));

    const auto totalDown = [&hud] {
        return hud.detailsWindow()->findChild<QLabel*>(u"details.currentDown"_s)->text();
    };
    const QString before = totalDown();
    hud.setSelectedInterfaces({u"lo"_s});
    const QString after = totalDown();
    QVERIFY(before != after);
    // Switching to loopback alone must not include the physical interfaces.
    QVERIFY(waitFor([&totalDown] { return totalDown().contains(u"9.00"_s); }));

    // Preferences keep the selection.
    QSettings reread(m_settingsPath, QSettings::IniFormat);
    QCOMPARE(ball::ui::Preferences::load(reread).interfaces, QStringList({u"lo"_s}));
}

void TestUi::clickOpensIndependentDetailsAndDragDoesNot() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    hud.move(100, 100);
    hud.show();
    QVERIFY(waitFor([&hud] { return hud.isVisible(); }));
    const QSize barSize = hud.size();
    const QPoint barPosition = hud.requestedPosition();
    auto* details = hud.detailsWindow();
    QVERIFY(details->isWindow());
    QVERIFY(details->parentWidget() == nullptr);
    QCOMPARE(details->windowModality(), Qt::NonModal);
    QCOMPARE(details->windowType(), Qt::Window);
    QSignalSpy exit(&hud, &Hud::exitRequested);

    for (int i = 0; i < 5; ++i) {
        QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QVERIFY(details->isVisible());
        QCOMPARE(hud.detailsWindow(), details);
        QCOMPARE(hud.size(), barSize);
        QCOMPARE(hud.requestedPosition(), barPosition);
        details->close();
        QVERIFY(!details->isVisible());
        QCOMPARE(hud.size(), barSize);
        QVERIFY(hud.isVisible());
    }
    // Repeated clicks bring back one window, rather than toggling or duplicating it.
    hud.showDetails();
    QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QVERIFY(details->isVisible());
    details->close();
    const QPoint start(20, 20);
    QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&hud, start + QPoint(60, 60));
    QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, start + QPoint(60, 60));
    QVERIFY(!details->isVisible());
    QVERIFY(exit.isEmpty());
}

void TestUi::lockedPositionStillOpensDetails() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    hud.move(200, 200);
    hud.show();
    QVERIFY(waitFor([&hud] { return hud.isVisible(); }));
    hud.setLocked(true);

    // Locking the HUD must not prevent opening details.
    QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QVERIFY(hud.detailsWindow()->isVisible());
    // Opening details never moves or resizes the locked HUD.
    const QPoint lockedAt = hud.pos();

    // A drag on a locked HUD is ignored; the surface stays where it was.
    const QPoint start(20, 20);
    QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&hud, start + QPoint(80, 80));
    QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, start + QPoint(80, 80));
    QCOMPARE(hud.pos(), lockedAt);
    // A drag is not a click, even on a locked HUD, so details stay open; a
    // large locked drag must never accidentally toggle it.
    QVERIFY(hud.detailsWindow()->isVisible());
}

void TestUi::waylandDragTracksRequestedPosition() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    WaylandHud hud(startServer(), settings);
    hud.show();
    QVERIFY(waitFor([&hud] { return hud.isVisible(); }));

    const QPoint start = hud.requestedPosition();
    QVERIFY(!start.isNull());
    // Offscreen there is no compositor to apply the margins, so QWidget::pos()
    // stays at the origin: that is exactly the "requested position is nonzero
    // while Qt pos differs" reproduction. On a real compositor the surface may
    // follow, which is fine; the requested position is still the authority.
    const bool offscreen = QGuiApplication::platformName() != QLatin1String("wayland");
    if (offscreen) {
        QCOMPARE(hud.pos(), QPoint(0, 0));
        QVERIFY(hud.pos() != start);
    }

    const QPoint pressLocal(20, 20);
    QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, pressLocal);
    QTest::mouseMove(&hud, pressLocal + QPoint(100, 70));
    QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, pressLocal + QPoint(100, 70));

    // The drag advanced the authoritative requested position by the pointer
    // delta. Adopting the unrelated QWidget::pos() would have snapped it back.
    QCOMPARE(hud.requestedPosition(), start + QPoint(100, 70));
    if (offscreen) {
        QCOMPARE(hud.pos(), QPoint(0, 0));
    }
    // A drag is not a click, so details stay closed.
    QVERIFY(!hud.detailsWindow()->isVisible());
}

void TestUi::waylandContinuousMotionTracksSurfaceMovement() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    WaylandHud hud(startServer(), settings);
    hud.show();
    QVERIFY(waitFor([&hud] { return hud.isVisible(); }));

    const QPoint start = hud.requestedPosition();
    const QPoint pressLocal(30, 10);
    // The cursor's global path, and the surface position at the time each
    // motion event arrives (the compositor applies the previous request one
    // step behind). The local coordinate therefore shrinks by exactly the
    // movement already applied, which is what a real surface reports.
    const QPoint cursorStep(40, 0);
    QPoint surface = start;

    QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, pressLocal);
    for (int step = 1; step <= 3; ++step) {
        const QPoint global = pressLocal + step * cursorStep;
        // Surface moved with the previous request; the cursor has advanced.
        const QPoint local = global - (surface - start);
        QTest::mouseMove(&hud, local);
        surface = hud.requestedPosition();
    }
    QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier,
                        pressLocal + 3 * cursorStep - (surface - start));

    // After three cursor steps the requested position tracked the full travel,
    // not a single local sample: this is the "follows the hand" behaviour.
    QCOMPARE(hud.requestedPosition(), start + 3 * cursorStep);
}

void TestUi::waylandLockedClickAndDragAreDistinct() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    WaylandHud hud(startServer(), settings);
    hud.show();
    QVERIFY(waitFor([&hud] { return hud.isVisible(); }));
    hud.setLocked(true);

    // A click still opens details while the HUD position is locked.
    QTest::mouseClick(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QVERIFY(hud.detailsWindow()->isVisible());
    const QPoint lockedAt = hud.requestedPosition();

    // A press-move-release past the threshold is a drag, not a click: it must
    // neither move the locked surface nor close the details window.
    QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QTest::mouseMove(&hud, QPoint(120, 100));
    QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(120, 100));
    QCOMPARE(hud.requestedPosition(), lockedAt);
    QVERIFY(hud.detailsWindow()->isVisible());
}

void TestUi::waylandDragSavesRequestedPosition() {
    QPoint dragged;
    {
        QSettings settings(m_settingsPath, QSettings::IniFormat);
        WaylandHud hud(startServer(), settings);
        hud.show();
        QVERIFY(waitFor([&hud] { return hud.isVisible(); }));
        QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTest::mouseMove(&hud, QPoint(120, 90));
        QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(120, 90));
        dragged = hud.requestedPosition();
    }

    // The requested position survives a restart, even though QWidget::pos()
    // never held it while the surface was alive.
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    const ball::ui::Preferences saved = ball::ui::Preferences::load(settings);
    QVERIFY(saved.hasPosition);
    QCOMPARE(saved.position, dragged);

    WaylandHud restored(startServer(), settings);
    QCOMPARE(restored.requestedPosition(), dragged);
}

void TestUi::waylandDragRequestsSurfaceCommit() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    WaylandHud hud(startServer(), settings);
    hud.show();
    QVERIFY(waitFor([&hud] { return hud.isVisible() && hud.paintCount > 0; }));
    QTest::qWait(40);
    const int before = hud.paintCount;
    QTest::mousePress(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QTest::mouseMove(&hud, QPoint(70, 20));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
    QVERIFY2(hud.paintCount > before, "placement changes must schedule a surface commit");
    QTest::mouseRelease(&hud, Qt::LeftButton, Qt::NoModifier, QPoint(70, 20));
}

void TestUi::detailsSurviveDeactivationAndEscapeClosesOnlyDetails() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    hud.show();
    hud.showDetails();
    auto* details = hud.detailsWindow();
    const QSize barSize = hud.size();
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(&hud, &deactivate);
    QCoreApplication::sendEvent(details, &deactivate);
    QVERIFY(details->isVisible());
    auto* search = details->findChild<QLineEdit*>(u"details.search"_s);
    QTest::keyClick(search, Qt::Key_Escape);
    QVERIFY(!details->isVisible());
    QVERIFY(hud.isVisible());
    QCOMPARE(hud.size(), barSize);

    hud.showDetails();
    auto* top = details->findChild<QToolButton*>(u"details.onTop"_s);
    top->setChecked(true);
    QVERIFY(details->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QVERIFY(details->isVisible());
    top->setChecked(false);
    QVERIFY(!details->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    QVERIFY(details->isVisible());
}

void TestUi::remembersPositionAndOpacity() {
    {
        QSettings settings(m_settingsPath, QSettings::IniFormat);
        Hud hud(startServer(), settings);
        hud.move(400, 300);
        hud.setOpacity(0.5);
    }
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    QCOMPARE(ball::ui::Preferences::load(settings).opacity, 0.5);
    QVERIFY(ball::ui::Preferences::load(settings).hasPosition);
}

void TestUi::contextMenuStaysCompact() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    hud.show();
    hud.openContextMenu(hud.mapToGlobal(QPoint(20, 20)));
    auto* menu = hud.findChild<QMenu*>(u"hud.menu"_s);
    QVERIFY(menu != nullptr);
    QVERIFY(waitFor([menu] { return menu->isVisible(); }));
    const auto bounds = hud.screen()->geometry();
    QVERIFY(menu->width() < bounds.width() / 2);
    QVERIFY(menu->height() < bounds.height() / 2);
    menu->close();
    const auto childMenus = menu->findChildren<QMenu*>().size();
    for (int i = 0; i < 3; ++i) {
        hud.openContextMenu(hud.mapToGlobal(QPoint(20, 20)));
        QCOMPARE(menu->findChildren<QMenu*>().size(), childMenus);
        menu->close();
    }
}

void TestUi::autostartMenuUsesUserConfiguration() {
    QTemporaryDir configuration;
    QVERIFY(configuration.isValid());
    const bool hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    const auto previous = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configuration.path().toUtf8());
    const auto restore = qScopeGuard([hadConfig, previous] {
        if (hadConfig) {
            qputenv("XDG_CONFIG_HOME", previous);
        } else {
            qunsetenv("XDG_CONFIG_HOME");
        }
    });
    QCOMPARE(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation),
             configuration.path());
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    auto* action = hud.findChild<QAction*>(u"hud.action.autostart"_s);
    QVERIFY(action != nullptr);
    QVERIFY(action->isCheckable());
    QVERIFY(!action->isChecked());
    action->trigger();
    QVERIFY(action->isChecked());
    QVERIFY(ball::ui::autostartEnabled(configuration.path()));
    action->trigger();
    QVERIFY(!action->isChecked());
    QVERIFY(!ball::ui::autostartEnabled(configuration.path()));
}

void TestUi::appearanceRoundTripAndInvalidPreferences() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    const auto defaults = ball::ui::Preferences::load(settings);
    QCOMPARE(defaults.theme, ball::ui::Theme::Porcelain);
    QCOMPARE(defaults.barLayout, ball::ui::BarLayout::TwoLine);
    {
        Hud hud(startServer(), settings);
        hud.setTheme(ball::ui::Theme::Paper);
        hud.setBarLayout(ball::ui::BarLayout::SingleLine);
        hud.setOpacity(0.7);
    }
    {
        Hud restored(startServer(), settings);
        QCOMPARE(restored.theme(), ball::ui::Theme::Paper);
        QCOMPARE(restored.barLayout(), ball::ui::BarLayout::SingleLine);
        QCOMPARE(restored.opacity(), 0.7);
    }
    settings.setValue(u"hud/theme"_s, u"unknown"_s);
    settings.setValue(u"hud/layout"_s, u"unknown"_s);
    const auto invalid = ball::ui::Preferences::load(settings);
    QCOMPARE(invalid.theme, defaults.theme);
    QCOMPARE(invalid.barLayout, defaults.barLayout);
}

void TestUi::barLayoutsStayCompactAndDoNotClip() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    hud.show();
    auto* rx = hud.findChild<QLabel*>(u"hud.rx"_s);
    auto* tx = hud.findChild<QLabel*>(u"hud.tx"_s);
    QSize twoLine;
    for (const auto layout : {ball::ui::BarLayout::TwoLine, ball::ui::BarLayout::SingleLine,
                              ball::ui::BarLayout::TwoLine}) {
        hud.setBarLayout(layout);
        QCoreApplication::processEvents();
        QVERIFY(hud.width() < 600);
        QVERIFY(hud.height() < 120);
        QCOMPARE(hud.minimumSize(), hud.maximumSize());
        for (auto* label : {rx, tx}) {
            QVERIFY(label->width() >= QFontMetrics(label->font()).horizontalAdvance(u"1023.99"_s));
            QVERIFY(hud.rect().contains(QRect(label->mapTo(&hud, QPoint()), label->size())));
        }
        const auto rxPoint = rx->mapTo(&hud, QPoint());
        const auto txPoint = tx->mapTo(&hud, QPoint());
        if (layout == ball::ui::BarLayout::TwoLine) {
            QCOMPARE(rxPoint.x(), txPoint.x());
            QVERIFY(txPoint.y() > rxPoint.y());
            twoLine = hud.size();
        } else {
            QCOMPARE(rxPoint.y(), txPoint.y());
            QVERIFY(txPoint.x() > rxPoint.x());
            QVERIFY(hud.width() > twoLine.width());
            QVERIFY(hud.height() < twoLine.height());
        }
        const auto size = hud.size();
        hud.showDetails();
        hud.detailsWindow()->resize(1000, 700);
        hud.detailsWindow()->close();
        QCOMPARE(hud.size(), size);
    }
}

void TestUi::fontChangesResizeTheBarWithoutClipping() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    hud.show();
    auto* rx = hud.findChild<QLabel*>(u"hud.rx"_s);
    const auto before = hud.size();
    QFont larger = hud.font();
    larger.setPointSizeF(larger.pointSizeF() + 4);
    hud.setFont(larger);
    QCoreApplication::processEvents();
    QVERIFY(hud.width() > before.width());
    QVERIFY(hud.height() > before.height());
    QCOMPARE(rx->width(), QFontMetrics(rx->font()).horizontalAdvance(u"1023.99"_s));
    hud.setBarLayout(ball::ui::BarLayout::SingleLine);
    QVERIFY(hud.rect().contains(QRect(rx->mapTo(&hud, QPoint()), rx->size())));
}

void TestUi::themeChangesAreLocalAndKeepTextLegible() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    const QPalette applicationPalette = qApp->palette();
    hud.show();
    hud.showDetails();
    for (const auto theme :
         {ball::ui::Theme::Porcelain, ball::ui::Theme::Paper, ball::ui::Theme::Graphite}) {
        hud.setTheme(theme);
        const auto colors = ball::ui::themeColors(theme);
        QCOMPARE(hud.palette().color(QPalette::Window), colors.background);
        QCOMPARE(hud.detailsWindow()->palette().color(QPalette::Window), colors.background);
        QCOMPARE(hud.detailsWindow()->palette().color(QPalette::Text), colors.text);
        QCOMPARE(qApp->palette(), applicationPalette);
        QCOMPARE(hud.findChild<QLabel*>(u"hud.rx"_s)->palette().color(QPalette::WindowText),
                 colors.text);
        QVERIFY(std::abs(colors.text.lightness() - colors.background.lightness()) > 140);
        const auto size = hud.size();
        hud.setOpacity(0.5);
        const auto image = hud.grab().toImage();
        const auto background =
            image.pixelColor(qRound(8 * image.devicePixelRatio()), image.height() / 2);
        QVERIFY(background.alpha() >= 120 && background.alpha() <= 135);
        QCOMPARE(hud.size(), size);
    }
}

void TestUi::allMetricsAreVisibleAndSortNumerically() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    QVERIFY(acceptConnection() != nullptr);
    send(snapshotLine());
    auto* details = hud.detailsWindow();
    auto* table = details->findChild<QTableView*>(u"details.processTable"_s);
    auto* search = details->findChild<QLineEdit*>(u"details.search"_s);
    QVERIFY(waitFor([table] { return table->model()->rowCount() == 2; }));
    QVERIFY(details->findChild<QObject*>(u"details.period"_s) == nullptr);
    QCOMPARE(table->model()->columnCount(), 9);
    for (int column = 3; column < 9; ++column) {
        QVERIFY(!table->isColumnHidden(column));
        table->sortByColumn(column, Qt::AscendingOrder);
        QCOMPARE(findText(table->model(), 0, 0), u"code"_s);
        table->sortByColumn(column, Qt::DescendingOrder);
        QCOMPARE(findText(table->model(), 0, 0), u"firefox"_s);
    }
    QVERIFY(findText(table->model(), 0, 3).contains(u"/s"_s));
    QVERIFY(!findText(table->model(), 0, 5).contains(u"/s"_s));
    QVERIFY(!findText(table->model(), 0, 7).contains(u"/s"_s));
    search->setText(u"1234"_s);
    QCOMPARE(table->model()->rowCount(), 1);
    search->clear();
    details->findChild<QCheckBox*>(u"details.showIdle"_s)->setChecked(true);
    QCOMPARE(table->model()->rowCount(), 3);
    auto* interfaces = details->findChild<QTableView*>(u"details.interfaceTable"_s);
    QCOMPARE(interfaces->model()->columnCount(), 9);
}

void TestUi::commandLineFormattingPreservesArguments() {
    QCOMPARE(ball::ui::formatCommandLine({u"/usr/bin/app"_s, u"--flag"_s, u"two words"_s, u"a'b"_s,
                                          QString(), u"$(whoami)"_s}),
             u"/usr/bin/app --flag 'two words' 'a'\\''b' '' '$(whoami)'"_s);
}

void TestUi::processTooltipReadsVerifiedCommandLineAndEscapesMarkup() {
    QFile stat(u"/proc/self/stat"_s);
    QVERIFY(stat.open(QIODevice::ReadOnly));
    const auto data = stat.readAll();
    const auto ticks = data.mid(data.lastIndexOf(')') + 2).split(' ').at(19).toULongLong();
    const auto hz = quint64(::sysconf(_SC_CLK_TCK));
    const quint64 startNs = (ticks / hz) * 1'000'000'000 + (ticks % hz) * 1'000'000'000 / hz;
    const int pid = int(QCoreApplication::applicationPid());
    const auto argv = ball::readProcessCommandLine({pid, startNs});
    QVERIFY(!argv.isEmpty());

    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    const auto parsed = ball::ui::parseSnapshot(snapshotLine());
    QVERIFY(parsed.snapshot.has_value());
    auto snapshot = parsed.snapshot.value_or(Snapshot{});
    auto& process = snapshot.processes.first();
    process.pid = pid;
    process.startNs = startNs;
    process.name = u"tokio-rt-worker"_s;
    hud.detailsWindow()->setSnapshot(snapshot, true, {u"eno1"_s});
    auto* table = hud.detailsWindow()->findChild<QTableView*>(u"details.processTable"_s);
    const auto index = table->model()->index(0, 0);
    QCOMPARE(index.data().toString(), ball::readProcessName({pid, startNs}));
    const auto tooltip = index.data(Qt::ToolTipRole).toString();
    QVERIFY(tooltip.contains(ball::ui::formatCommandLine(argv).toHtmlEscaped()));
    QVERIFY(!tooltip.contains(u"命令行不可用"_s));

    process.pid = std::numeric_limits<int>::max();
    process.name = u"<img src='x'>"_s;
    hud.detailsWindow()->setSnapshot(snapshot, true, {u"eno1"_s});
    const auto unavailable = table->model()->index(0, 0).data(Qt::ToolTipRole).toString();
    QVERIFY(unavailable.contains(u"命令行不可用"_s));
    QVERIFY(unavailable.contains(u"&lt;img"_s));
    QVERIFY(!unavailable.contains(u"<img"_s));
}

void TestUi::processQualityDoesNotInvalidateInterfaceRates() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    QVERIFY(acceptConnection() != nullptr);
    auto object = QJsonDocument::fromJson(snapshotLine()).object();
    auto process = object[u"process_status"_s].toObject();
    process[u"incomplete"_s] = true;
    object[u"process_status"_s] = process;
    send(QJsonDocument(object).toJson(QJsonDocument::Compact));
    auto* status = hud.findChild<QLabel*>(u"hud.status"_s);
    auto* rx = hud.findChild<QLabel*>(u"hud.rx"_s);
    auto* quality = hud.detailsWindow()->findChild<QLabel*>(u"details.quality"_s);
    auto* reasons = hud.detailsWindow()->findChild<QLabel*>(u"details.reasons"_s);
    QVERIFY(waitFor([status] { return status->text() == u"!"_s; }));
    QCOMPARE(rx->text(), u"1.49"_s);
    QVERIFY(quality->text().contains(u"当前采集正常"_s));
    QVERIFY(quality->text().contains(u"历史缺口"_s));
    QVERIFY(!reasons->text().isEmpty());
    hud.showDetails();
    auto* toggle = hud.detailsWindow()->findChild<QToolButton*>(u"details.showReasons"_s);
    QVERIFY(!reasons->isVisible());
    toggle->setChecked(true);
    QVERIFY(reasons->isVisible());

    send(snapshotLine(u"2026-10-14T14:30:03+08:00"_s, true, false, true));
    QVERIFY(waitFor([quality] { return quality->text().contains(u"进程采集不可用"_s); }));
    QCOMPARE(rx->text(), u"1.49"_s);
    process[u"incomplete"_s] = false;
    process[u"error"_s] = u"collector error"_s;
    object[u"process_status"_s] = process;
    send(QJsonDocument(object).toJson(QJsonDocument::Compact));
    QVERIFY(waitFor([reasons] { return reasons->text().contains(u"collector error"_s); }));
    QVERIFY(quality->text().contains(u"进程采集不可用"_s));
    send(snapshotLine());
    QVERIFY(waitFor([status] { return status->text().isEmpty(); }));
}

void TestUi::partialInterfaceSelectionIsVisible() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    QVERIFY(acceptConnection() != nullptr);
    send(snapshotLine());
    QVERIFY(waitFor([&hud] { return !hud.selectedInterfaces().isEmpty(); }));
    hud.setSelectedInterfaces({u"eno1"_s, u"missing"_s});
    auto* status = hud.findChild<QLabel*>(u"hud.status"_s);
    auto* quality = hud.detailsWindow()->findChild<QLabel*>(u"details.quality"_s);
    QCOMPARE(status->text(), u"!"_s);
    QVERIFY(quality->text().contains(u"部分所选网卡缺失"_s));
    hud.setSelectedInterfaces({});
    QCOMPARE(status->text(), u"未选"_s);
    QCOMPARE(hud.findChild<QLabel*>(u"hud.rx"_s)->text(), u"—"_s);
    hud.setSelectedInterfaces({u"missing"_s});
    QCOMPARE(status->text(), u"异常"_s);
}

void TestUi::staleRatesAreUnavailableInBothWindows() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    QVERIFY(acceptConnection() != nullptr);
    send(snapshotLine());
    auto* rx = hud.findChild<QLabel*>(u"hud.rx"_s);
    QVERIFY(waitFor([rx] { return rx->text() == u"1.49"_s; }));
    m_client->disconnectFromServer();
    QVERIFY(waitFor([rx] { return rx->text() == u"—"_s; }));
    auto* details = hud.detailsWindow();
    QVERIFY(details->findChild<QLabel*>(u"details.currentDown"_s)->text().contains(u"—"_s));
    for (const auto& name : {u"details.processTable"_s, u"details.interfaceTable"_s}) {
        auto* table = details->findChild<QTableView*>(name);
        QVERIFY(findText(table->model(), 0, 3).contains(u"--"_s));
    }
    QCOMPARE(hud.findChild<QLabel*>(u"hud.status"_s)->text(), u"过期"_s);
}

void TestUi::detailsGeometrySurvivesRestart() {
    QSize saved;
    {
        QSettings settings(m_settingsPath, QSettings::IniFormat);
        Hud hud(startServer(), settings);
        hud.showDetails();
        hud.detailsWindow()->resize(740, 520);
        saved = hud.detailsWindow()->size();
        hud.detailsWindow()->close();
    }
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud restored(startServer(), settings);
    restored.showDetails();
    QCOMPARE(restored.detailsWindow()->size(), saved);
}

void TestUi::appearanceMenuUpdatesPreferences() {
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    Hud hud(startServer(), settings);
    auto* theme = hud.findChild<QAction*>(u"hud.action.theme.1"_s);
    auto* layout = hud.findChild<QAction*>(u"hud.action.layout.1"_s);
    QVERIFY(theme != nullptr && layout != nullptr);
    theme->trigger();
    layout->trigger();
    QCOMPARE(hud.theme(), ball::ui::Theme::Paper);
    QCOMPARE(hud.barLayout(), ball::ui::BarLayout::SingleLine);
    const auto saved = ball::ui::Preferences::load(settings);
    QCOMPARE(saved.theme, hud.theme());
    QCOMPARE(saved.barLayout, hud.barLayout());
    QVERIFY(hud.findChild<QAction*>(u"hud.action.pin"_s) == nullptr);
}

QTEST_MAIN(TestUi)
#include "ui.moc"
