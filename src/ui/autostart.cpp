// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/autostart.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSaveFile>

namespace ball::ui {
namespace {

QString entryPath(const QString& configDirectory) {
    return QDir(configDirectory).filePath(QStringLiteral("autostart/ball.desktop"));
}

struct EntryState {
    bool application = false;
    bool hidden = false;
    bool executable = false;
    bool managed = false;
};

// Minimal desktop-entry reader: only the keys that define the state count, so
// extra or localized keys can never flip the answer.
EntryState parseEntry(const QByteArray& content) {
    EntryState state;
    bool inDesktopEntry = false;
    for (const QByteArray& rawLine : content.split('\n')) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        if (line.startsWith('[')) {
            inDesktopEntry = line == "[Desktop Entry]";
            continue;
        }
        if (!inDesktopEntry) {
            continue;
        }
        const qsizetype separator = line.indexOf('=');
        if (separator <= 0) {
            continue;
        }
        const QByteArray key = line.left(separator).trimmed();
        const QByteArray value = line.mid(separator + 1).trimmed();
        if (key == "Type") {
            state.application = value == "Application";
        } else if (key == "Hidden") {
            state.hidden = value == "true";
        } else if (key == "Exec") {
            state.executable = !value.isEmpty();
        } else if (key == "X-Ball-Managed") {
            state.managed = value == "true";
        }
    }
    return state;
}

QString keyFileEscape(const QString& value) {
    QString escaped;
    escaped.reserve(value.size());
    for (const QChar& character : value) {
        if (character == QLatin1Char('\\')) {
            escaped += QLatin1Char('\\');
        }
        escaped += character;
    }
    return escaped;
}

// Exec has two escaping layers: quoting inside the field, then key-file
// escaping of the whole value. `%` never reaches this point: GIO rejects any
// Exec containing it, even doubled as %%, so such a path is refused instead of
// yielding an entry that silently never launches.
QString storedExec(const QString& path) {
    QString quoted = QStringLiteral("\"");
    quoted.reserve(path.size() + 2);
    for (const QChar& character : path) {
        if (QStringLiteral("\\\"`$").contains(character)) {
            quoted += QLatin1Char('\\');
        }
        quoted += character;
    }
    quoted += QLatin1Char('"');
    return keyFileEscape(quoted);
}

QString enabledEntry(const QString& executablePath) {
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=ball 流量悬浮窗\n"
                          "Exec=%1\n"
                          "Terminal=false\n"
                          "OnlyShowIn=KDE;\n"
                          "X-Ball-Managed=true\n")
        .arg(storedExec(executablePath));
}

// Adds Hidden=true without discarding the other keys of a hand-written entry.
QByteArray setHidden(const QByteArray& content, bool hidden) {
    QByteArray result;
    bool inDesktopEntry = false;
    bool written = false;
    const auto writeHidden = [&] {
        result += hidden ? "Hidden=true\n" : "Hidden=false\n";
        written = true;
    };
    for (const QByteArray& line : content.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith('[')) {
            if (inDesktopEntry && !written) {
                writeHidden();
            }
            inDesktopEntry = trimmed == "[Desktop Entry]";
        }
        const qsizetype separator = trimmed.indexOf('=');
        if (inDesktopEntry && separator > 0 && trimmed.left(separator).trimmed() == "Hidden") {
            continue;
        }
        result += line + '\n';
    }
    if (inDesktopEntry && !written) {
        writeHidden();
    }
    return result;
}

bool writeAtomically(const QString& path, const QByteArray& content) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool readEntry(const QString& path, QByteArray& content) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || QFileInfo(path).size() > 65536 ||
        !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    content = file.readAll();
    return true;
}

} // namespace

bool autostartEnabled(const QString& configDirectory) {
    if (configDirectory.isEmpty()) {
        return false;
    }
    const QString path = entryPath(configDirectory);
    // A symlink is somebody else's entry; reporting it as enabled would make
    // the menu toggle fight the user's own file.
    if (QFileInfo(path).isSymLink()) {
        return false;
    }
    QByteArray content;
    if (!readEntry(path, content)) {
        return false;
    }
    const EntryState state = parseEntry(content);
    return state.application && state.executable && !state.hidden;
}

bool setAutostartEnabled(const QString& configDirectory, const QString& executablePath,
                         bool enabled, QString& error) {
    error.clear();
    if (configDirectory.isEmpty()) {
        error = QStringLiteral("empty config directory");
        return false;
    }
    const QString path = entryPath(configDirectory);

    // Refuse before doing anything: replacing or removing the link itself would
    // drop a user's own entry, and following it would edit data outside our file.
    if (QFileInfo(path).isSymLink()) {
        error = QStringLiteral("autostart path is a symlink");
        return false;
    }

    if (!enabled) {
        if (!QFileInfo::exists(path)) {
            return true; // nothing was enabled, so nothing to create or remove
        }
        QByteArray content;
        if (!readEntry(path, content)) {
            error = QStringLiteral("cannot read autostart file");
            return false;
        }
        if (parseEntry(content).managed) {
            if (!QFile::remove(path)) {
                error = QStringLiteral("cannot remove autostart file");
                return false;
            }
            return true;
        }
        // Somebody else wrote this entry: keep it and only mark it hidden.
        if (!writeAtomically(path, setHidden(content, true))) {
            error = QStringLiteral("cannot write autostart file");
            return false;
        }
        return true;
    }

    if (executablePath.isEmpty() || !executablePath.startsWith(QLatin1Char('/'))) {
        error = QStringLiteral("executable path is not absolute");
        return false;
    }
    if (executablePath.contains(QLatin1Char('%'))) {
        error = QStringLiteral("unsupported executable path");
        return false;
    }
    for (const QChar character : executablePath) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f) {
            error = QStringLiteral("unsupported executable path");
            return false;
        }
    }
    if (QFileInfo::exists(path)) {
        QByteArray content;
        if (!readEntry(path, content)) {
            error = QStringLiteral("cannot read autostart file");
            return false;
        }
        const EntryState state = parseEntry(content);
        if (!state.managed) {
            if (!state.application || !state.executable) {
                error = QStringLiteral("unrecognized autostart entry");
                return false;
            }
            if (!writeAtomically(path, setHidden(content, false))) {
                error = QStringLiteral("cannot write autostart file");
                return false;
            }
            return true;
        }
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = QStringLiteral("cannot create autostart directory");
        return false;
    }
    if (!writeAtomically(path, enabledEntry(executablePath).toUtf8())) {
        error = QStringLiteral("cannot write autostart file");
        return false;
    }
    return true;
}

} // namespace ball::ui
