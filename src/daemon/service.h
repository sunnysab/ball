// ball: a network traffic HUD for KDE Wayland sessions.
// Daemon-side service: samples interfaces, supervises the external collector
// and serves the read-only NDJSON snapshot stream to local GUI clients.
#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

class QLocalServer;
class QLocalSocket;
class QLockFile;
class QProcess;
class QTemporaryDir;
class QTimer;

namespace ball {

class Interfaces;
class Socktrail;
class Traffic;

// Where the daemon listens and which socktrail binary to supervise. The socket
// directory is created by the deployment layer (systemd), never here.
struct ServiceOptions {
    QString socketPath;
    QString collectorBinary;
};

// Owns the whole daemon side of the protocol: a 1 s interface sampler, one
// supervised socktrail child plus its subscriber, and a QLocalServer that
// broadcasts a full snapshot after every connection and once per second.
class Service : public QObject {
    Q_OBJECT

public:
    explicit Service(QObject* parent = nullptr);
    ~Service() override;

    // Takes the public socket. Returns false and sets error() when the path is
    // unusable (a live instance, a symlink or a regular file, a listen failure).
    // A collector that cannot start only shows up in the snapshot: interface
    // sampling keeps working without it, so start() still succeeds.
    bool start(const ServiceOptions& options);
    // Stops sampling, drops every client and terminates the collector child.
    void stop();

    // Current protocol snapshot; safe to call at any time, even before start().
    QJsonObject snapshot() const;
    // A socket-level failure from start() while empty; afterwards the current
    // collector/socktrail problem, so a running daemon can still report why
    // process data is missing. Empty when everything the service needs is in
    // place.
    QString error() const;

Q_SIGNALS:
    // A new snapshot is ready or the service state changed.
    void updated();

private:
    void shutdown();
    void onNewConnection();
    void onTick();
    void pollInterfaces();
    void broadcastSnapshot();
    QByteArray snapshotLine() const;
    // Writes one snapshot line; false asks the caller to drop the socket (gone
    // or too far behind).
    bool sendSnapshot(QLocalSocket* socket, const QByteArray& line);
    void removeClient(QLocalSocket* socket);
    void startCollector();
    void stopCollector();
    void onCollectorFinished();
    void drainCollectorStderr();
    bool collectorRunning() const;
    QString processError() const;

    ServiceOptions m_options;
    std::unique_ptr<Interfaces> m_interfaces;
    std::unique_ptr<Traffic> m_traffic;
    std::unique_ptr<QLockFile> m_lock;
    std::unique_ptr<QTemporaryDir> m_rawDir;
    QLocalServer* m_server = nullptr;
    QProcess* m_collector = nullptr;
    Socktrail* m_socktrail = nullptr;
    QTimer* m_tickTimer = nullptr;
    QTimer* m_restartTimer = nullptr;
    QList<QLocalSocket*> m_clients;
    QElapsedTimer m_clock; // monotonic timeline for the current service session
    QDateTime m_started;   // protocol `started`, fixed for this service session
    QString m_collectorStderr;
    QString m_error;
    QString m_interfaceError;
    QString m_collectorError;
    QString m_rawSocketPath;
    bool m_running = false;
    bool m_interfacesAvailable = false;

    static constexpr int kMaxClients = 16;
    static constexpr int kPollIntervalMs = 1000;
    static constexpr int kPollTimeoutMs = 50;
    static constexpr int kCollectorRestartMs = 1000;
    // A GUI that fell this far behind is dropped rather than allowed to grow an
    // unbounded write buffer.
    static constexpr qint64 kMaxBacklogBytes = 64LL * 1024;
    static constexpr int kMaxErrorLength = 160;
    static constexpr int kStderrTailLength = 120;
    static constexpr int kTerminateWaitMs = 2000;
};

} // namespace ball
