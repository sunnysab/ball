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
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

using namespace Qt::StringLiterals;

namespace {

// The samples the preview renders. The values are chosen to exercise the HUD:
// byte rates, a stopped process and a quality note.
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
        hud.setTheme(ball::ui::Theme::Porcelain);
        QCoreApplication::processEvents();
        const QPixmap shot = hud.detailsWindow()->grab();
        if (!shot.save(output)) {
            std::fprintf(stderr, "preview: cannot write %s\n", qPrintable(output));
            app.exit(1);
            return;
        }
        std::fprintf(stdout, "%s\n", qPrintable(output));
        app.exit(0);
    });
    return app.exec();
}
