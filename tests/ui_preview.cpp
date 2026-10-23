// ball: a network traffic HUD for KDE Wayland sessions.
// Renders the details and all built-in HUD presets with mock data
// without a desktop session. It feeds the widget through the real local socket
// protocol and writes only generated data, never user preferences or traffic.
//
// Usage: ball_ui_preview [output.png]
#include "snapshot.h"
#include "ui/details.h"
#include "ui/hud.h"

#include <QApplication>
#include <QByteArray>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <cstdio>

using namespace Qt::StringLiterals;

namespace {

// The samples the preview renders. The values are chosen to exercise the HUD:
// byte rates, a stopped process, a quality note and three synthetic
// connections with distinct protocols, names and processes.
ball::Snapshot mockSnapshot() {
    const auto at = [](const QString& text) { return QDateTime::fromString(text, Qt::ISODate); };
    const auto interface = [](quint32 index, const QString& name, ball::InterfaceKind kind,
                              quint64 todayRx, quint64 todayTx, quint64 sessionRx,
                              quint64 sessionTx, double rx, double tx) {
        ball::InterfaceSnapshot row;
        row.index = index;
        row.name = name;
        row.kind = kind;
        row.up = true;
        row.today = {todayRx, todayTx};
        row.session = {sessionRx, sessionTx};
        row.rxBps = rx;
        row.txBps = tx;
        return row;
    };

    ball::Snapshot snapshot;
    snapshot.version = 1;
    snapshot.started = at(u"2026-10-14T14:30:00+08:00"_s);
    snapshot.at = at(u"2026-10-15T09:41:07+08:00"_s);
    snapshot.interfacesAvailable = true;
    snapshot.interfaceError.clear();
    snapshot.ipPackets = 2'450'000;
    snapshot.ipBytes = 3'892'314'112ULL;
    snapshot.processStatus = {true, false, true, QString(), {u"PID events: dropped 3"_s}};
    snapshot.interfaces = {
        interface(2, u"eno1"_s, ball::InterfaceKind::Physical, 3221225472ULL, 536870912ULL,
                  1073741824ULL, 268435456ULL, 1625292.8, 231424.0),
        interface(3, u"wlan0"_s, ball::InterfaceKind::Physical, 268435456ULL, 10485760ULL,
                  104857600ULL, 5242880ULL, 524288.0, 20480.0),
        interface(4, u"wg0"_s, ball::InterfaceKind::Virtual, 4194304ULL, 1048576ULL, 2097152ULL,
                  524288ULL, 12000.0, 4000.0),
        interface(1, u"lo"_s, ball::InterfaceKind::Loopback, 1048576ULL, 1048576ULL, 1048576ULL,
                  1048576ULL, 900.0, 900.0),
    };

    const auto process = [](int pid, quint64 startNs, const QString& name, bool present,
                            quint64 rxToday, quint64 txToday, double rx, double tx) {
        ball::ProcessSnapshot row;
        row.pid = pid;
        row.startNs = startNs;
        row.name = name;
        row.present = present;
        row.today = {rxToday, txToday};
        row.session = {rxToday / 2, txToday / 2};
        row.rxBps = rx;
        row.txBps = tx;
        return row;
    };
    snapshot.processes = {
        process(4821, 9007199254740993ULL, u"firefox"_s, true, 966367641ULL, 52428800ULL,
                1509949.44, 52428.8),
        process(9134, 1770000000000000000ULL, u"code-insiders"_s, true, 268435456ULL, 10485760ULL,
                262144.0, 10485.76),
        process(2210, 1600000000000000000ULL, u"mpv"_s, true, 2147483648ULL, 2097152ULL, 3145728.0,
                1024.0),
        process(733, 1500000000000000000ULL, u"systemd-networkd"_s, false, 1048576ULL, 262144ULL,
                0.0, 0.0),
    };

    const auto connection = [&at](quint64 id, const QString& protocol, const QString& name,
                                  const QString& app, const QString& source, const QString& target,
                                  const QString& direction, double rx, double tx) {
        ball::ConnectionSnapshot row;
        row.id = id;
        row.protocol = protocol;
        row.name = name;
        row.app = app;
        row.state = u"established"_s;
        row.direction = direction;
        row.source = source;
        row.target = target;
        row.firstSeen = at(u"2026-10-15T09:40:00+08:00"_s);
        row.lastSeen = at(u"2026-10-15T09:41:07+08:00"_s);
        row.rxBps = rx;
        row.txBps = tx;
        return row;
    };
    ball::ConnectionSnapshot https =
        connection(1, u"tcp"_s, u"example.org"_s, u"firefox"_s, u"10.0.0.2:51000"_s,
                   u"93.184.216.34:443"_s, u"outbound"_s, 1500000.0, 51200.0);
    https.client = {4821, 9007199254740993ULL, u"firefox"_s, 966367641ULL, 52428800ULL};
    https.interfaces = {u"eno1"_s};
    https.packets = 51234;
    https.rttUs = 12000;
    https.rttSource = u"tcp_info"_s;
    https.detail = u"TLS SNI: example.org"_s;
    ball::ConnectionSnapshot dns =
        connection(2, u"udp"_s, u"api.example.com"_s, u"code-insiders"_s, u"10.0.0.2:53000"_s,
                   u"203.0.113.7:53"_s, u"outbound"_s, 24000.0, 18000.0);
    dns.server = {9134, 1770000000000000000ULL, u"code-insiders"_s, 268435456ULL, 10485760ULL};
    dns.interfaces = {u"eno1"_s};
    dns.packets = 320;
    ball::ConnectionSnapshot quic =
        connection(3, u"quic"_s, u"cdn.example.net"_s, QString(), u"10.0.0.2:54000"_s,
                   u"198.51.100.9:443"_s, u"outbound"_s, 380000.0, 42000.0);
    quic.io.append({2210, 1600000000000000000ULL, u"mpv"_s, 2147483648ULL, 2097152ULL});
    quic.interfaces = {u"eno1"_s};
    quic.packets = 2048;
    quic.retransmits = 3;
    snapshot.connections = {https, dns, quic};
    snapshot.connectionStatus.available = true;
    snapshot.connectionStatus.stale = false;
    return snapshot;
}

// The frame bytes the GUI socket carries.
QByteArray mockSnapshotFrame() {
    bool ok = false;
    const QByteArray frame = ball::encodeSnapshot(mockSnapshot(), ok);
    Q_ASSERT(ok);
    return frame;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const QString output =
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : u"/tmp/ball-ui-preview.png"_s;

    QTemporaryDir prefs;
    QSettings settings(prefs.filePath(u"preview.ini"_s), QSettings::IniFormat);

    QLocalServer server;
    const QString name = u"ball-ui-preview-%1"_s.arg(QCoreApplication::applicationPid());
    if (!server.listen(name)) {
        std::fprintf(stderr, "preview: cannot listen on %s\n", qPrintable(name));
        return 1;
    }

    QObject::connect(&server, &QLocalServer::newConnection, &app, [&server] {
        if (auto* peer = server.nextPendingConnection()) {
            peer->write(mockSnapshotFrame());
            peer->flush();
        }
    });

    ball::ui::Hud hud(name, settings);
    hud.showDetails();
    hud.show();

    QTimer::singleShot(500, [&] {
        QPixmap bars(1350, 220);
        bars.fill(QColor("#e5e7eb"));
        QPainter painter(&bars);
        int column = 0;
        for (const auto theme :
             {ball::ui::Theme::Porcelain, ball::ui::Theme::Paper, ball::ui::Theme::Graphite}) {
            hud.setTheme(theme);
            painter.setPen(Qt::black);
            painter.drawText(column * 450 + 16, 24, ball::ui::themeName(theme));
            int y = 40;
            for (const auto layout :
                 {ball::ui::BarLayout::TwoLine, ball::ui::BarLayout::SingleLine}) {
                hud.setBarLayout(layout);
                QCoreApplication::processEvents();
                const auto shot = hud.grab();
                painter.drawPixmap(QRect(column * 450 + 16, y, hud.width(), hud.height()), shot);
                y += hud.height() + 16;
            }
            ++column;
        }
        painter.end();
        const QFileInfo file(output);
        const QString barsOutput = file.absolutePath() + QLatin1Char('/') +
                                   file.completeBaseName() + QStringLiteral("-bars.png");
        if (!bars.save(barsOutput)) {
            app.exit(1);
            return;
        }
        auto* details = hud.detailsWindow();
        auto history = mockSnapshot();
        history.processStatus.incomplete = false;
        history.processStatus.notes.clear();
        const auto until = history.at;
        for (int second = 0; second <= 300; ++second) {
            history.at = until.addSecs(second - 300);
            const double burst = 0.65 + 0.2 * std::sin(second / 20.0) +
                                 2.2 * std::exp(-std::pow(second - 80.0, 2) / 240) +
                                 1.4 * std::exp(-std::pow(second - 210.0, 2) / 800);
            history.interfaces[0].rxBps = 1625292.8 * burst;
            history.interfaces[0].txBps = 231424.0 * (1 + 0.4 * std::sin(second / 14.0));
            details->setSnapshot(history, second < 140 || second > 155, {u"eno1"_s, u"wlan0"_s});
        }
        details->setStatus({}, {});
        hud.setTheme(ball::ui::Theme::Porcelain);
        QCoreApplication::processEvents();
        const QPixmap shot = details->grab();
        if (!shot.save(output)) {
            std::fprintf(stderr, "preview: cannot write %s\n", qPrintable(output));
            app.exit(1);
            return;
        }
        auto* tabs = details->findChild<QTabWidget*>(u"details.tabs"_s);
        const auto saveVariant = [&](const QString& suffix) {
            QCoreApplication::processEvents();
            const QString path = file.absolutePath() + QLatin1Char('/') + file.completeBaseName() +
                                 suffix + QStringLiteral(".png");
            return details->grab().save(path);
        };
        hud.setTheme(ball::ui::Theme::Graphite);
        if (!saveVariant(u"-dark"_s)) {
            app.exit(1);
            return;
        }
        hud.setTheme(ball::ui::Theme::Porcelain);
        if (tabs != nullptr) {
            tabs->setCurrentIndex(1);
        }
        if (!saveVariant(u"-processes"_s)) {
            app.exit(1);
            return;
        }
        // Select one synthetic connection so the resizable detail panel is populated.
        auto* table = details->findChild<QTableWidget*>(u"connections.table"_s);
        if (tabs != nullptr) {
            tabs->setCurrentIndex(3);
            QCoreApplication::processEvents();
            if (table != nullptr && table->rowCount() > 0) {
                table->selectRow(0);
                QCoreApplication::processEvents();
            }
        }
        const QPixmap connectionShot = details->grab();
        const QString connectionsOutput = file.absolutePath() + QLatin1Char('/') +
                                          file.completeBaseName() +
                                          QStringLiteral("-connections.png");
        if (!connectionShot.save(connectionsOutput)) {
            std::fprintf(stderr, "preview: cannot write %s\n", qPrintable(connectionsOutput));
            app.exit(1);
            return;
        }
        std::fprintf(stdout, "%s\n", qPrintable(connectionsOutput));
        std::fprintf(stdout, "%s\n", qPrintable(output));
        app.exit(0);
    });
    return app.exec();
}
