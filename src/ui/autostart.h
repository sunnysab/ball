// ball: a network traffic HUD for KDE Wayland sessions.
// Per-user XDG autostart management for the GUI window only. It starts the
// HUD at login; the daemon keeps counting in the background regardless.
#pragma once

#include <QString>

namespace ball::ui {

// Whether <configDirectory>/autostart/ball.desktop shows the GUI at login.
// The desktop file is the only source of truth: a missing entry, a non-
// Application type, an empty Exec or Hidden=true all mean disabled.
bool autostartEnabled(const QString& configDirectory);

// Enables or disables the login autostart entry. Enabling atomically writes a
// freedesktop entry for `executablePath`; disabling removes the file this
// module wrote, or adds Hidden=true to a hand-written entry. A symlink at the
// entry path is never followed and never replaced. On failure this returns
// false, sets `error` to a short ASCII reason, and leaves no partial file.
bool setAutostartEnabled(const QString& configDirectory, const QString& executablePath,
                         bool enabled, QString& error);

} // namespace ball::ui
