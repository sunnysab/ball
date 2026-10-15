// ball: a network traffic HUD for KDE Wayland sessions.
// The only state that survives a restart; traffic history is never persisted.
#pragma once

#include <QPoint>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace ball::ui {

// Preferences are limited on purpose: position, screen, opacity and the
// interface selection. Snapshot values are never written to disk.
struct Preferences {
    QPoint position;
    QString screenName;
    double opacity = 0.9;
    QStringList interfaces;
    bool hasInterfaceSelection = false;

    // `hasPosition` distinguishes "never saved" from a saved (0,0).
    bool hasPosition = false;

    static Preferences load(QSettings& settings);
    void save(QSettings& settings) const;
};

} // namespace ball::ui
