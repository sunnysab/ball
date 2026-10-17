// ball: a network traffic HUD for KDE Wayland sessions.
// Minimal socktrail stand-in for the Service tests: listens on --socket and
// writes a hello plus one stats frame per second, in the same MessagePack
// framing the real collector uses. Test-only; it never touches the network or
// requires root.
#include "mp_frames.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileDevice>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <cstdio>

using namespace Qt::StringLiterals;

namespace {

// The instant socktrail's hello always reports for this fixture.
const QDateTime kStarted = QDateTime::fromString(u"2026-10-13T14:00:00Z"_s, Qt::ISODate);

QByteArray helloFrame() {
    return ball::test::envelope(u"hello"_s, QLatin1StringView("hello"),
                                [](ball::test::Writer& writer) {
                                    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                                    packer.pack_map(2);
                                    writer.key(QLatin1StringView("version"));
                                    packer.pack(1);
                                    writer.key(QLatin1StringView("started"));
                                    writer.timestamp(kStarted);
                                });
}

// One stats frame with `count` process rows. The counters grow on every call,
// so the second frame produces the first nonzero delta.
QByteArray statsFrame(quint64& rx, quint64& tx) {
    // One process row per frame unless a test asks for a bigger table.
    const int count = qEnvironmentVariableIsSet("BALL_FAKE_PROCESS_COUNT")
                          ? qEnvironmentVariableIntValue("BALL_FAKE_PROCESS_COUNT")
                          : 1;
    return ball::test::envelope(u"stats"_s, QLatin1StringView("stats"),
                                [&](ball::test::Writer& writer) {
                                    msgpack::packer<msgpack::sbuffer>& packer = writer.packer();
                                    packer.pack_map(7);
                                    writer.key(QLatin1StringView("at"));
                                    writer.timestamp(QDateTime::currentDateTimeUtc());
                                    writer.key(QLatin1StringView("process_io"));
                                    packer.pack_array(uint32_t(count));
                                    for (int i = 0; i < count; ++i) {
                                        rx += 1000;
                                        tx += 500;
                                        packer.pack_map(5);
                                        writer.key(QLatin1StringView("pid"));
                                        packer.pack(i + 1);
                                        // 2^53 + 1: it only survives if the wire
                                        // format keeps every bit.
                                        writer.key(QLatin1StringView("start_ns"));
                                        packer.pack(uint64_t(9007199254740993ULL));
                                        writer.key(QLatin1StringView("name"));
                                        writer.text(u"fake-%1"_s.arg(i));
                                        writer.key(QLatin1StringView("rx_bytes"));
                                        packer.pack(rx);
                                        writer.key(QLatin1StringView("tx_bytes"));
                                        packer.pack(tx);
                                    }
                                    writer.key(QLatin1StringView("io_unindexed"));
                                    packer.pack_map(2);
                                    writer.key(QLatin1StringView("rx_bytes"));
                                    packer.pack(uint64_t(0));
                                    writer.key(QLatin1StringView("tx_bytes"));
                                    packer.pack(uint64_t(0));
                                    writer.key(QLatin1StringView("expired_pid_io"));
                                    packer.pack_map(2);
                                    writer.key(QLatin1StringView("rx_bytes"));
                                    packer.pack(uint64_t(0));
                                    writer.key(QLatin1StringView("tx_bytes"));
                                    packer.pack(uint64_t(0));
                                    writer.key(QLatin1StringView("expired_pid_count"));
                                    packer.pack(uint64_t(0));
                                    // Socket I/O the collector could not attach
                                    // to a connection; the daemon reports it.
                                    writer.key(QLatin1StringView("io_unmatched_bytes"));
                                    packer.pack(uint64_t(0));
                                    writer.key(QLatin1StringView("probes"));
                                    packer.pack_map(1);
                                    writer.key(QLatin1StringView("pid"));
                                    packer.pack_map(3);
                                    writer.key(QLatin1StringView("kernel_lost"));
                                    packer.pack(uint64_t(0));
                                    writer.key(QLatin1StringView("dropped"));
                                    packer.pack(uint64_t(0));
                                    writer.key(QLatin1StringView("invalid"));
                                    packer.pack(uint64_t(0));
                                });
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
            peer->write(statsFrame(rx, tx));
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
        peer->write(helloFrame());
        sendStats();
        timer.start();
    });
    return app.exec();
}
