// ball: a network traffic HUD for KDE Wayland sessions.
#include <LayerShellQt/Window>

#include <QApplication>
#include <QByteArray>
#include <QString>
#include <QWidget>
#include <QWindow>

#include <cstdio>
#include <optional>

namespace {

void printUsage(std::FILE* out) {
    std::fputs("Usage: ball [options]\n"
               "\n"
               "Options:\n"
               "  -h, --help     show this help and exit\n"
               "      --version  show the version and exit\n",
               out);
}

// Qt aborts when no platform plugin can be loaded, so the text-only options are
// answered before QApplication opens a Wayland connection. Returns the process
// exit code, or std::nullopt to continue into the GUI.
std::optional<int> handleTextOption(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            printUsage(stdout);
            return 0;
        }
        if (arg == "--version") {
            std::fprintf(stdout, "ball %s\n", BALL_VERSION);
            return 0;
        }
        if (arg.startsWith('-')) {
            std::fprintf(stderr, "ball: unknown option '%s'\n", arg.constData());
            printUsage(stderr);
            return 2;
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char* argv[]) {
    if (const std::optional<int> code = handleTextOption(argc, argv)) {
        return *code;
    }

    QApplication app(argc, argv);
    if (app.platformName() != QStringLiteral("wayland")) {
        std::fprintf(stderr, "ball: this build only supports Wayland, got platform '%s'\n",
                     qPrintable(app.platformName()));
        return 1;
    }

    QWidget window;
    window.setWindowFlags(Qt::FramelessWindowHint);
    window.setAttribute(Qt::WA_TranslucentBackground);
    window.resize(320, 120);
    window.show();

    QWindow* handle = window.windowHandle();
    if (handle == nullptr) {
        std::fprintf(stderr, "ball: no window handle, cannot attach a layer surface\n");
        return 1;
    }
    LayerShellQt::Window* shell = LayerShellQt::Window::get(handle);
    if (shell == nullptr) {
        std::fprintf(stderr, "ball: the compositor does not support layer surfaces\n");
        return 1;
    }

    shell->setScope(QStringLiteral("ball"));
    shell->setLayer(LayerShellQt::Window::LayerTop);
    shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    shell->setExclusiveZone(0);
    // The header does not declare operator| for this enum, so compose the flags manually.
    LayerShellQt::Window::Anchors anchors = LayerShellQt::Window::AnchorTop;
    anchors |= LayerShellQt::Window::AnchorLeft;
    shell->setAnchors(anchors);
    shell->setActivateOnShow(false);

    return app.exec();
}
