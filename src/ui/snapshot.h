// ball: a network traffic HUD for KDE Wayland sessions.
// Snapshot model for the local GUI socket contract in docs/protocol.md.
#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace ball::ui {

// Interface kind as defined by the snapshot contract.
enum class InterfaceKind : unsigned char {
    Physical,
    Virtual,
    Loopback,
};

QString interfaceKindName(InterfaceKind kind);

struct ByteCounters {
    quint64 rxBytes = 0;
    quint64 txBytes = 0;
};

// One interface row. Byte counters come from decimal strings, so they keep the
// full 64-bit precision the kernel reports; rates are bytes per second.
struct InterfaceSnapshot {
    quint32 index = 0;
    QString name;
    InterfaceKind kind = InterfaceKind::Physical;
    bool up = false;
    ByteCounters today;
    ByteCounters session;
    double rxBps = 0.0;
    double txBps = 0.0;
};

// One process row. `startNs` is the collector's nanosecond identity and, with
// the PID, tells a recycled PID apart from its previous owner.
struct ProcessSnapshot {
    int pid = 0;
    quint64 startNs = 0;
    QString name;
    bool present = false;
    ByteCounters today;
    ByteCounters session;
    double rxBps = 0.0;
    double txBps = 0.0;
};

// Quality of the process side of one snapshot. `available` and `stale` must
// both be checked before process rates may be shown as live.
struct ProcessStatus {
    bool available = false;
    bool stale = true;
    bool incomplete = false;
    QString error;
    QStringList notes;
};

struct Snapshot {
    int version = 0;
    QDateTime started; // backend monitoring start, changes when the backend restarts
    QDateTime at;      // instant this snapshot was generated
    bool interfacesAvailable = false;
    QString interfaceError;
    ProcessStatus processStatus;
    QList<InterfaceSnapshot> interfaces;
    QList<ProcessSnapshot> processes;
};

struct SnapshotParseResult {
    std::optional<Snapshot> snapshot;
    QString error; // short ASCII reason; empty when `snapshot` is set
};

// Parses exactly one snapshot line, without its newline. The whole object is
// validated before it is returned, so a rejected frame can never partially
// update the UI.
SnapshotParseResult parseSnapshot(const QByteArray& line);

} // namespace ball::ui
