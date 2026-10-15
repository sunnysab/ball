// ball: a network traffic HUD for KDE Wayland sessions.
// Tests for the per-user autostart helper. Everything happens inside a
// QTemporaryDir, so the real HOME and the real session autostart are never
// touched; no root and no network access are needed.
#include "ui/autostart.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

using namespace Qt::StringLiterals;

namespace {

QString entryPath(const QTemporaryDir& dir) { return dir.filePath(u"autostart/ball.desktop"_s); }

QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

void writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), static_cast<qint64>(content.size()));
}

} // namespace

class TestAutostart : public QObject {
    Q_OBJECT

private slots:
    // A fresh directory has no entry and reports disabled; nothing is created
    // merely by asking.
    void defaultDisabledWithoutFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = entryPath(dir);

        QVERIFY(!ball::ui::autostartEnabled(dir.path()));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(dir.filePath(u"autostart"_s)));
    }

    void enableDisableReEnablePersists() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = entryPath(dir);
        const QString executable = u"/usr/local/bin/ball"_s;
        QString error;

        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), executable, true, error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(ball::ui::autostartEnabled(dir.path()));

        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), executable, false, error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!ball::ui::autostartEnabled(dir.path()));

        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), executable, true, error));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(ball::ui::autostartEnabled(dir.path()));
    }

    // Disabling when nothing was ever enabled is a no-op, not a file creation.
    void disableWithoutEntryIsNoop() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;

        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, false, error));
        QVERIFY(!QFileInfo::exists(entryPath(dir)));
    }

    void enabledEntryIsAValidDesktopFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;
        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, true, error));

        const QByteArray content = readFile(entryPath(dir));
        QVERIFY(content.startsWith("[Desktop Entry]\n"));
        QVERIFY(content.contains("Type=Application\n"));
        QVERIFY(content.contains("Terminal=false\n"));
        QVERIFY(content.contains("OnlyShowIn=KDE;\n"));
        QVERIFY(content.contains("Exec=\"/usr/local/bin/ball\"\n"));
        QVERIFY(content.contains("Name=ball"));
        QVERIFY(content.contains("X-Ball-Managed=true\n"));

        // Optional: the freedesktop validator is not installed everywhere, but
        // when it is, the generated file must be accepted by it.
        const QString validator = QStandardPaths::findExecutable(u"desktop-file-validate"_s);
        if (!validator.isEmpty()) {
            QProcess process;
            process.start(validator, {entryPath(dir)});
            QVERIFY(process.waitForFinished(30000));
            QCOMPARE(process.exitCode(), 0);
        }
    }

    // Space, quote, backslash, dollar and backtick are all legal and must
    // survive both escaping layers of the Exec value.
    void specialCharactersInExecutablePath() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString special = QStringLiteral("/sp ace/quo\"te\\back$d`ollar/ball");
        QString error;

        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), special, true, error));
        QVERIFY2(error.isEmpty(), qPrintable(error));

        // Layer 1 escapes \, " and (for GLib) $ and `; layer 2 escapes the
        // backslashes again for the key-file value. Verified against GLib,
        // which launches this exact argv[0].
        const QString content = QString::fromUtf8(readFile(entryPath(dir)));
        const QString expectedExec =
            QStringLiteral("Exec=\"/sp ace/quo\\\\\"te\\\\\\\\back\\\\$d\\\\`ollar/ball\"");
        QVERIFY2(content.contains(expectedExec), qPrintable(content));
        QVERIFY(content.contains(QStringLiteral("X-Ball-Managed=true\n")));
    }

    // GLib/GIO rejects any Exec containing '%', even escaped as '%%', so such a
    // path must be refused instead of writing an entry that never launches.
    void percentPathIsRefusedWithoutSideEffects() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;

        QVERIFY(!ball::ui::setAutostartEnabled(dir.path(), u"/opt/a%b/ball"_s, true, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(entryPath(dir)));
        QVERIFY(!ball::ui::autostartEnabled(dir.path()));
    }

    // A relative path can never be launched from a desktop file.
    void relativeExecutableIsRefused() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;

        QVERIFY(!ball::ui::setAutostartEnabled(dir.path(), u"ball"_s, true, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(entryPath(dir)));
    }

    // A hand-written entry is not ours: disabling marks it Hidden instead of
    // deleting the user's file.
    void foreignEntryIsHiddenNotDeleted() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = entryPath(dir);
        const QByteArray original = "[Desktop Entry]\n"
                                    "Type=Application\n"
                                    "Name=User's own ball launcher\n"
                                    "Exec=/home/user/bin/ball\n"
                                    "Comment=my entry\n";
        writeFile(path, original);
        QVERIFY(ball::ui::autostartEnabled(dir.path()));

        QString error;
        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, false, error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!ball::ui::autostartEnabled(dir.path()));

        // Every original key survives; only Hidden was added.
        const QByteArray content = readFile(path);
        QVERIFY(content.contains("Comment=my entry\n"));
        QVERIFY(content.contains("Exec=/home/user/bin/ball\n"));
        QVERIFY(content.contains("Hidden=true\n"));
    }

    void foreignEntryWithOtherGroupsKeepsItsCommand() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = entryPath(dir);
        writeFile(path, "[Desktop Entry]\nType=Application\nName=custom\n"
                        "Exec=/custom/ball --socket /custom/gui.sock\n"
                        "Comment=mentions X-Ball-Managed=true\n"
                        "[Desktop Action Inspect]\nName=Inspect\nExec=/custom/inspect\n");
        QString error;
        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, false, error));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!ball::ui::autostartEnabled(dir.path()));
        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, true, error));
        QVERIFY(ball::ui::autostartEnabled(dir.path()));
        const auto content = readFile(path);
        QVERIFY(content.contains("Exec=/custom/ball --socket /custom/gui.sock\n"));
        QVERIFY(content.contains("Exec=/custom/inspect\n"));
    }

    void controlCharactersAreRefused() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString error;
        QVERIFY(
            !ball::ui::setAutostartEnabled(dir.path(), u"/tmp/ball\nHidden=false"_s, true, error));
        QVERIFY(!QFileInfo::exists(entryPath(dir)));
    }

    // An already hidden entry stays disabled, whatever the other keys say.
    void hiddenEntryIsDisabled() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(entryPath(dir), "[Desktop Entry]\n"
                                  "Type=Application\n"
                                  "Name=ball\n"
                                  "Exec=/usr/local/bin/ball\n"
                                  "Hidden=true\n");
        QVERIFY(!ball::ui::autostartEnabled(dir.path()));
    }

    void nonApplicationEntryIsDisabled() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(entryPath(dir), "[Desktop Entry]\n"
                                  "Type=Link\n"
                                  "Name=ball\n"
                                  "Exec=/usr/local/bin/ball\n"
                                  "URL=https://example.invalid/\n");
        QVERIFY(!ball::ui::autostartEnabled(dir.path()));
    }

    // Neither enabling nor disabling may touch the symlink or its target.
    void symlinkIsNeverFollowedOrRemoved() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = dir.filePath(u"user-entry.desktop"_s);
        const QByteArray targetContent = "[Desktop Entry]\n"
                                         "Type=Application\n"
                                         "Name=user entry\n"
                                         "Exec=/home/user/bin/ball\n";
        writeFile(target, targetContent);
        const QString path = entryPath(dir);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QVERIFY(QFile::link(target, path));
        QVERIFY(QFileInfo(path).isSymLink());

        QString error;
        QVERIFY(!ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, true, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, false, error));

        // Only the link itself changed argv; the target bytes are untouched.
        QVERIFY(QFileInfo(path).isSymLink());
        QCOMPARE(readFile(target), targetContent);
    }

    // A failure must be reported, not silently swallowed, and must not leave a
    // half-written file behind.
    void unwritableDirectoryFailsCleanly() {
        if (::geteuid() == 0) {
            QSKIP("root ignores directory permissions");
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString autostartDir = dir.filePath(u"autostart"_s);
        QVERIFY(QDir().mkpath(autostartDir));
        QVERIFY(
            QFile::setPermissions(autostartDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

        QString error;
        QVERIFY(!ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, true, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(entryPath(dir)));
        QVERIFY(!ball::ui::autostartEnabled(dir.path()));
    }

    // Managing our file must not disturb anything else in the config directory.
    void otherConfigFilesAreUntouched() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString other = dir.filePath(u"ball.conf"_s);
        const QByteArray otherContent = "[hud]\nopacity=0.5\n";
        writeFile(other, otherContent);

        QString error;
        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, true, error));
        QVERIFY(ball::ui::setAutostartEnabled(dir.path(), u"/usr/local/bin/ball"_s, false, error));
        QCOMPARE(readFile(other), otherContent);
        QVERIFY(QFileInfo::exists(other));
    }
};

QTEST_GUILESS_MAIN(TestAutostart)
#include "autostart.moc"
