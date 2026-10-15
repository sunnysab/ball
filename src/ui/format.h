// ball: a network traffic HUD for KDE Wayland sessions.
// Fixed-width number formatting so a changing value never resizes the HUD.
#pragma once

#include <QFont>
#include <QString>
#include <QStringList>

namespace ball::ui {

// Monospace font for every numeric label, so digits never shift the layout.
QFont numericFont();

// Formats a byte rate (bytes per second) with an automatic unit, e.g.
// "  12.34 MiB/s". Number and unit are padded to a fixed width on purpose.
QString formatRate(double bytesPerSecond);

// Formats a byte total, e.g. "   1.50 MiB". Fixed width for the same reason.
QString formatBytes(quint64 bytes);

// Placeholder shown when no fresh value exists. It uses the same fixed width as
// a real value so the HUD does not jump when data appears or expires.
QString unavailableRate();

// A readable POSIX-shell representation that preserves argument boundaries.
QString formatCommandLine(const QStringList& arguments);

} // namespace ball::ui
