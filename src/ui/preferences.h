// ball: a network traffic HUD for KDE Wayland sessions.
// The only state that survives a restart; traffic history is never persisted.
#pragma once

#include "ui/appearance.h"

#include <QByteArray>
#include <QPoint>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace ball::ui {

// Only display preferences survive a restart; snapshot values never go to disk.
struct Preferences {
    QPoint position;
    QString screenName;
    double opacity = 0.96;
    Theme theme = Theme::Porcelain;
    BarLayout barLayout = BarLayout::TwoLine;
    QByteArray detailsGeometry;
    QStringList interfaces;
    bool hasInterfaceSelection = false;

    // `hasPosition` distinguishes "never saved" from a saved (0,0).
    bool hasPosition = false;

    static Preferences load(QSettings& settings);
    void save(QSettings& settings) const;
};

} // namespace ball::ui
