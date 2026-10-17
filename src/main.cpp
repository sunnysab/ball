// ball: a network traffic HUD for KDE Wayland sessions.
// Single entry point: it either runs the read-only GUI or the sampling daemon.
#include "daemon/service.h"
#include "ui/hud.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QSettings>
#include <QSocketNotifier>
#include <QString>
#include <QStringList>

#include <csignal>
#include <cstdio>
#include <pthread.h>
#include <pwd.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace {

// Blocks the termination signals before any Qt object exists, so every Qt
// thread inherits the mask, and turns them into readable signalfd events.
// Restores the previous mask and closes the descriptor on scope exit.
class SignalMask {
public:
    SignalMask() {
        sigemptyset(&m_set);
        sigaddset(&m_set, SIGINT);
        sigaddset(&m_set, SIGTERM);
        sigaddset(&m_set, SIGHUP);
        if (::pthread_sigmask(SIG_BLOCK, &m_set, &m_previous) != 0) {
            return;
        }
        m_blocked = true;
        m_fd = ::signalfd(-1, &m_set, SFD_CLOEXEC | SFD_NONBLOCK);
    }
    ~SignalMask() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        if (m_blocked) {
            ::pthread_sigmask(SIG_SETMASK, &m_previous, nullptr);
        }
    }
    SignalMask(const SignalMask&) = delete;
    SignalMask& operator=(const SignalMask&) = delete;

    int fd() const { return m_fd; }
    void drain() const {
        signalfd_siginfo info{};
        while (m_fd >= 0 && ::read(m_fd, &info, sizeof info) > 0) {
        }
    }

private:
    sigset_t m_set{};
    sigset_t m_previous{};
    int m_fd = -1;
    bool m_blocked = false;
};

// Quits the event loop on SIGINT/SIGTERM/SIGHUP without a signal handler
// calling into Qt. Must run after the QCoreApplication exists. Returns false
// when the signals could not be wired up.
bool installQuitSignals(const SignalMask& mask, QCoreApplication& app) {
    if (mask.fd() < 0) {
        return false;
    }
    auto* notifier = new QSocketNotifier(mask.fd(), QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [&mask, &app] {
        mask.drain();
        app.quit();
    });
    return true;
}

QString defaultGuiSocket() {
    const struct passwd* user = ::getpwuid(::getuid());
    const QString name = user != nullptr && user->pw_name != nullptr
                             ? QString::fromLocal8Bit(user->pw_name)
                             : QString::number(static_cast<qulonglong>(::getuid()));
    return QStringLiteral("/run/ball-%1/gui.sock").arg(name);
}

bool isAbsolutePath(const QString& path) { return QDir::isAbsolutePath(path); }

void printUsage(std::FILE* out) {
    std::fputs("Usage: ball [options]\n"
               "  -h, --help              show this help and exit\n"
               "      --version           show the version and exit\n"
               "      --daemon            run the sampling daemon\n"
               "      --socket <path>     snapshot socket (required with --daemon)\n"
               "      --socktrail-bin <path>  collector binary (daemon mode only)\n"
               "      --collector-socket <path>  subscribe to an already running collector\n"
               "                          instead of spawning one (daemon mode only)\n",
               out);
}

int fail(const QString& message, int code) {
    std::fprintf(stderr, "ball: %s\n", qPrintable(message));
    return code;
}

} // namespace

int main(int argc, char* argv[]) {
    // Takes precedence over the platform plugin: the GUI must answer --help and
    // --version even without a Wayland session.
    const SignalMask signalMask;

    QStringList args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        args.append(QString::fromLocal8Bit(argv[i]));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("ball: network traffic HUD for KDE Wayland sessions"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(QCommandLineOption(
        QStringLiteral("daemon"), QStringLiteral("run the sampling daemon instead of the GUI")));
    parser.addOption(QCommandLineOption(QStringLiteral("socket"),
                                        QStringLiteral("absolute path of the snapshot socket"),
                                        QStringLiteral("path")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("socktrail-bin"),
        QStringLiteral("absolute path of the socktrail collector (daemon mode only)"),
        QStringLiteral("path")));
    parser.addOption(QCommandLineOption(
        QStringLiteral("collector-socket"),
        QStringLiteral("absolute path of an already running collector socket; the daemon "
                       "subscribes to it and never starts one (daemon mode only)"),
        QStringLiteral("path")));

    // The explicit QStringList keeps option values from being mistaken for a
    // mode flag; Qt never treats an argument as both an option and its value.
    if (!parser.parse(args)) {
        std::fprintf(stderr, "ball: %s\n", qPrintable(parser.errorText()));
        printUsage(stderr);
        return 2;
    }
    if (!parser.positionalArguments().isEmpty()) {
        std::fprintf(stderr, "ball: unexpected argument '%s'\n",
                     qPrintable(parser.positionalArguments().constFirst()));
        printUsage(stderr);
        return 2;
    }

    const bool wantsText = parser.isSet(QStringLiteral("help")) ||
                           parser.isSet(QStringLiteral("help-all")) ||
                           parser.isSet(QStringLiteral("version"));
    if (wantsText) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("ball"));
        QCoreApplication::setApplicationVersion(QStringLiteral(BALL_VERSION));
        parser.process(args);
        return 0;
    }

    const bool daemon = parser.isSet(QStringLiteral("daemon"));
    const bool hasSocket = parser.isSet(QStringLiteral("socket"));
    const QString socket = parser.value(QStringLiteral("socket"));
    const bool hasCollector = parser.isSet(QStringLiteral("socktrail-bin"));
    const QString collector = parser.value(QStringLiteral("socktrail-bin"));
    const bool hasCollectorSocket = parser.isSet(QStringLiteral("collector-socket"));
    const QString collectorSocket = parser.value(QStringLiteral("collector-socket"));

    if (hasSocket && !isAbsolutePath(socket)) {
        return fail(QStringLiteral("--socket requires an absolute path"), 2);
    }
    if (!daemon && hasCollector) {
        return fail(QStringLiteral("--socktrail-bin is only valid with --daemon"), 2);
    }
    if (!daemon && hasCollectorSocket) {
        return fail(QStringLiteral("--collector-socket is only valid with --daemon"), 2);
    }
    if (daemon && (!hasSocket || socket.isEmpty())) {
        return fail(QStringLiteral("--daemon requires --socket <absolute-path>"), 2);
    }
    if (hasCollector && !isAbsolutePath(collector)) {
        return fail(QStringLiteral("--socktrail-bin requires an absolute path"), 2);
    }
    if (hasCollectorSocket && !isAbsolutePath(collectorSocket)) {
        return fail(QStringLiteral("--collector-socket requires an absolute path"), 2);
    }
    if (hasCollector && hasCollectorSocket) {
        return fail(QStringLiteral("--socktrail-bin and --collector-socket are mutually "
                                   "exclusive"),
                    2);
    }

    if (daemon) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("ball"));
        QCoreApplication::setApplicationVersion(QStringLiteral(BALL_VERSION));
        if (!installQuitSignals(signalMask, app)) {
            return fail(QStringLiteral("could not set up signal handling"), 1);
        }

        ball::Service service;
        ball::ServiceOptions options;
        options.socketPath = socket;
        if (hasCollectorSocket) {
            // Only the socket is taken: an external unit owns the collector
            // process and its capture scope.
            options.collectorSocketPath = collectorSocket;
        } else {
            options.collectorBinary =
                hasCollector ? collector : QStringLiteral(BALL_DEFAULT_COLLECTOR_PATH);
        }
        if (!service.start(options)) {
            return fail(service.error(), 1);
        }
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &service, &ball::Service::stop);
        return app.exec();
    }

    // GUI: only a KDE Wayland session is supported, and never as root.
    if (::geteuid() == 0) {
        return fail(QStringLiteral("refusing to run the GUI as root"), 1);
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ball"));
    QCoreApplication::setApplicationName(QStringLiteral("ball"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BALL_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("ball"));
    if (app.platformName() != QStringLiteral("wayland")) {
        return fail(QStringLiteral("the GUI requires a Wayland session, got platform '%1'")
                        .arg(app.platformName()),
                    1);
    }
    app.setWindowIcon(ball::ui::Hud::hudIcon());
    // The HUD hides to the tray; the event loop must survive its window closing.
    app.setQuitOnLastWindowClosed(false);
    if (!installQuitSignals(signalMask, app)) {
        return fail(QStringLiteral("could not set up signal handling"), 1);
    }

    QSettings settings;
    ball::ui::Hud hud(socket.isEmpty() ? defaultGuiSocket() : socket, settings);
    hud.setWindowIcon(ball::ui::Hud::hudIcon());
    // Quitting the GUI never asks the backend to stop.
    QObject::connect(&hud, &ball::ui::Hud::exitRequested, &app, &QCoreApplication::quit);
    hud.show();
    return app.exec();
}
