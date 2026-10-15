// ball: a network traffic HUD for KDE Wayland sessions.
// Renders the details and all built-in HUD presets with mock data
// without a desktop session. It feeds the widget through the real local socket
// protocol and writes only generated data, never user preferences or traffic.
//
// Usage: ball_ui_preview [output.png]
#include "ui/details.h"
#include "ui/hud.h"

#include <QApplication>
#include <QByteArray>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

QByteArray mockSnapshot() {
    const QJsonArray interfaces{
        QJsonObject{{u"index"_s, 2},
                    {u"name"_s, u"eno1"_s},
                    {u"kind"_s, u"physical"_s},
                    {u"up"_s, true},
                    {u"today"_s, QJsonObject{{u"rx_bytes"_s, u"3221225472"_s},
                                             {u"tx_bytes"_s, u"536870912"_s}}},
                    {u"session"_s, QJsonObject{{u"rx_bytes"_s, u"1073741824"_s},
                                               {u"tx_bytes"_s, u"268435456"_s}}},
                    {u"rate"_s, QJsonObject{{u"rx_bps"_s, 1625292.8}, {u"tx_bps"_s, 231424.0}}}},
        QJsonObject{{u"index"_s, 3},
                    {u"name"_s, u"wlan0"_s},
                    {u"kind"_s, u"physical"_s},
                    {u"up"_s, true},
                    {u"today"_s,
                     QJsonObject{{u"rx_bytes"_s, u"268435456"_s}, {u"tx_bytes"_s, u"10485760"_s}}},
                    {u"session"_s,
                     QJsonObject{{u"rx_bytes"_s, u"104857600"_s}, {u"tx_bytes"_s, u"5242880"_s}}},
                    {u"rate"_s, QJsonObject{{u"rx_bps"_s, 524288.0}, {u"tx_bps"_s, 20480.0}}}},
        QJsonObject{
            {u"index"_s, 4},
            {u"name"_s, u"wg0"_s},
            {u"kind"_s, u"virtual"_s},
            {u"up"_s, true},
            {u"today"_s, QJsonObject{{u"rx_bytes"_s, u"4194304"_s}, {u"tx_bytes"_s, u"1048576"_s}}},
            {u"session"_s,
             QJsonObject{{u"rx_bytes"_s, u"2097152"_s}, {u"tx_bytes"_s, u"524288"_s}}},
            {u"rate"_s, QJsonObject{{u"rx_bps"_s, 12000.0}, {u"tx_bps"_s, 4000.0}}}},
        QJsonObject{
            {u"index"_s, 1},
            {u"name"_s, u"lo"_s},
            {u"kind"_s, u"loopback"_s},
            {u"up"_s, true},
            {u"today"_s, QJsonObject{{u"rx_bytes"_s, u"1048576"_s}, {u"tx_bytes"_s, u"1048576"_s}}},
            {u"session"_s,
             QJsonObject{{u"rx_bytes"_s, u"1048576"_s}, {u"tx_bytes"_s, u"1048576"_s}}},
            {u"rate"_s, QJsonObject{{u"rx_bps"_s, 900.0}, {u"tx_bps"_s, 900.0}}}}};

    const auto process = [](int pid, const QString& startNs, const QString& name, bool present,
                            qint64 rxToday, qint64 txToday, double rx, double tx) {
        return QJsonObject{
            {u"pid"_s, pid},
            {u"start_ns"_s, startNs},
            {u"name"_s, name},
            {u"present"_s, present},
            {u"today"_s, QJsonObject{{u"rx_bytes"_s, QString::number(rxToday)},
                                     {u"tx_bytes"_s, QString::number(txToday)}}},
            {u"session"_s, QJsonObject{{u"rx_bytes"_s, QString::number(rxToday / 2)},
                                       {u"tx_bytes"_s, QString::number(txToday / 2)}}},
            {u"rate"_s, QJsonObject{{u"rx_bps"_s, rx}, {u"tx_bps"_s, tx}}}};
    };
    const QJsonArray processes{process(4821, u"9007199254740993"_s, u"firefox"_s, true, 966367641,
                                       52428800, 1509949.44, 52428.8),
                               process(9134, u"1770000000000000000"_s, u"code-insiders"_s, true,
                                       268435456, 10485760, 262144.0, 10485.76),
                               process(2210, u"1600000000000000000"_s, u"mpv"_s, true, 2147483648,
                                       2097152, 3145728.0, 1024.0),
                               process(733, u"1500000000000000000"_s, u"systemd-networkd"_s, false,
                                       1048576, 262144, 0.0, 0.0)};

    const QJsonObject snapshot{
        {u"version"_s, 1},
        {u"kind"_s, u"snapshot"_s},
        {u"started"_s, u"2026-10-14T14:30:00+08:00"_s},
        {u"at"_s, u"2026-10-15T09:41:07+08:00"_s},
        {u"interfaces_available"_s, true},
        {u"interface_error"_s, u""_s},
        {u"process_status"_s, QJsonObject{{u"available"_s, true},
                                          {u"stale"_s, false},
                                          {u"incomplete"_s, true},
                                          {u"error"_s, u""_s},
                                          {u"notes"_s, QJsonArray{u"PID events: dropped 3"_s}}}}};
    QJsonObject result = snapshot;
    result[u"interfaces"_s] = interfaces;
    result[u"processes"_s] = processes;
    return QJsonDocument(result).toJson(QJsonDocument::Compact);
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
            peer->write(mockSnapshot());
            peer->write("\n");
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
