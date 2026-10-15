// ball: identity-checked Linux process metadata shared by daemon and GUI.
#pragma once

#include <QString>
#include <QStringList>

namespace ball {

struct ProcessIdentity {
    int pid = 0;
    quint64 startNs = 0;
};

// A collector identity is PID + boot-relative start nanoseconds. Compare its
// clock-tick projection with /proc/<pid>/stat before using metadata from that PID.
// The injected root lets tests use fixtures without inspecting other processes.
QString readProcessName(ProcessIdentity process, const QString& procRoot = QStringLiteral("/proc"));

// Runs with the caller's permissions. Empty means unavailable, empty argv, or
// over the 64 KiB bound; never reads another incarnation of a recycled PID.
QStringList readProcessCommandLine(ProcessIdentity process,
                                   const QString& procRoot = QStringLiteral("/proc"));

} // namespace ball
