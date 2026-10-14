// ball: a network traffic HUD for KDE Wayland sessions.
// Minimal socktrail stand-in for the Service tests: listens on --socket and
// writes a hello plus one stats frame per second. Test-only; it never touches
// the network or requires root.
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileDevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <QTimer>

#include <cstdio>

namespace {

QByteArray toLine(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray statsLine(quint64& rx, quint64& tx) {
    // One process row per frame unless a test asks for a bigger table.
    const int count = qEnvironmentVariableIsSet("BALL_FAKE_PROCESS_COUNT")
                          ? qEnvironmentVariableIntValue("BALL_FAKE_PROCESS_COUNT")
                          : 1;
    QJsonArray processes;
    for (int i = 0; i < count; ++i) {
        rx += 1000;
        tx += 500;
        processes.append(QJsonObject{{QStringLiteral("pid"), i + 1},
                                     {QStringLiteral("start_ns"), QJsonValue(9007199254740993LL)},
                                     {QStringLiteral("name"), QStringLiteral("fake-%1").arg(i)},
                                     {QStringLiteral("rx_bytes"), QJsonValue(qint64(rx))},
                                     {QStringLiteral("tx_bytes"), QJsonValue(qint64(tx))}});
    }
    const QJsonObject io{{QStringLiteral("rx_bytes"), 0}, {QStringLiteral("tx_bytes"), 0}};
    const QJsonObject probes{{QStringLiteral("pid"), QJsonObject{{QStringLiteral("kernel_lost"), 0},
                                                                 {QStringLiteral("dropped"), 0},
                                                                 {QStringLiteral("invalid"), 0}}}};
    const QJsonObject stats{
        {QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("process_io"), processes},
        {QStringLiteral("io_unindexed"), io},
        {QStringLiteral("expired_pid_io"), io},
        {QStringLiteral("expired_pid_count"), 0},
        {QStringLiteral("probes"), probes}};
    return toLine(QJsonObject{{QStringLiteral("kind"), QStringLiteral("stats")},
                              {QStringLiteral("stats"), stats}});
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QString socketPath;
    for (int i = 1; i + 1 < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--socket")) {
            socketPath = QString::fromLocal8Bit(argv[i + 1]);
        }
    }
    const QString pidFile = qEnvironmentVariable("BALL_FAKE_PID_FILE");
    if (!pidFile.isEmpty()) {
        QFile file(pidFile);
        // Append, so a test can count how often the collector was started.
        if (file.open(QIODevice::Append)) {
            file.write(QByteArray::number(QCoreApplication::applicationPid()) + '\n');
        }
    }
    if (qEnvironmentVariableIsSet("BALL_FAKE_EXIT")) {
        return 3; // an immediately dying collector, like a broken install
    }
    if (socketPath.isEmpty()) {
        return 2;
    }

    QLocalServer server;
    QFile::remove(socketPath);
    if (!server.listen(socketPath)) {
        return 4;
    }
    QFile::setPermissions(socketPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    QLocalSocket* peer = nullptr;
    quint64 rx = 0;
    quint64 tx = 0;
    const auto sendStats = [&] {
        if (peer != nullptr && peer->state() == QLocalSocket::ConnectedState) {
            peer->write(statsLine(rx, tx));
            peer->flush();
            std::fputs("collector output is not protocol data\n", stdout);
            std::fflush(stdout);
        }
    };
    QTimer timer;
    timer.setInterval(1000);
    QObject::connect(&timer, &QTimer::timeout, &app, sendStats);
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        peer = server.nextPendingConnection();
        if (peer == nullptr) {
            return;
        }
        peer->write(toLine(QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("hello")},
            {QStringLiteral("hello"),
             QJsonObject{{QStringLiteral("version"), 1},
                         {QStringLiteral("started"), QStringLiteral("2026-10-13T14:00:00Z")}}}}));
        sendStats();
        timer.start();
    });
    return app.exec();
}
