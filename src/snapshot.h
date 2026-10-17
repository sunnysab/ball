// ball: a network traffic HUD for KDE Wayland sessions.
// Snapshot model and MessagePack codec for the local GUI socket contract in
// docs/protocol.md. The schema is declared once and drives both the daemon's
// encoder and the GUI's decoder, so the two cannot drift apart.
#pragma once

#include "daemon/traffic.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <msgpack/object_fwd.hpp>

#include <optional>

namespace ball {

// Byte counters and rates of one interface or process. The counters are kept as
// 64-bit integers: MessagePack carries them exactly, and the "today"/"session"
// split is what the HUD shows.
struct ByteCounters {
    quint64 rxBytes = 0;
    quint64 txBytes = 0;
};

// One interface row. `kind` is the traffic core's own enum, because the daemon
// already knows it and the wire name is derived from it.
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

// Wire field names, in one place: the decoder matches keys against them and the
// encoder writes exactly these, so a rename cannot silently break one side.
namespace field {
inline constexpr QLatin1StringView kVersion("version");
inline constexpr QLatin1StringView kKind("kind");
inline constexpr QLatin1StringView kStarted("started");
inline constexpr QLatin1StringView kAt("at");
inline constexpr QLatin1StringView kInterfacesAvailable("interfaces_available");
inline constexpr QLatin1StringView kInterfaceError("interface_error");
inline constexpr QLatin1StringView kProcessStatus("process_status");
inline constexpr QLatin1StringView kAvailable("available");
inline constexpr QLatin1StringView kStale("stale");
inline constexpr QLatin1StringView kIncomplete("incomplete");
inline constexpr QLatin1StringView kError("error");
inline constexpr QLatin1StringView kNotes("notes");
inline constexpr QLatin1StringView kInterfaces("interfaces");
inline constexpr QLatin1StringView kProcesses("processes");
inline constexpr QLatin1StringView kIndex("index");
inline constexpr QLatin1StringView kName("name");
inline constexpr QLatin1StringView kUp("up");
inline constexpr QLatin1StringView kToday("today");
inline constexpr QLatin1StringView kSession("session");
inline constexpr QLatin1StringView kRate("rate");
inline constexpr QLatin1StringView kRxBytes("rx_bytes");
inline constexpr QLatin1StringView kTxBytes("tx_bytes");
inline constexpr QLatin1StringView kRxBps("rx_bps");
inline constexpr QLatin1StringView kTxBps("tx_bps");
inline constexpr QLatin1StringView kPid("pid");
inline constexpr QLatin1StringView kStartNs("start_ns");
inline constexpr QLatin1StringView kPresent("present");
inline constexpr QLatin1StringView kSnapshotKind("snapshot");
} // namespace field

// The wire name of an interface kind, or an empty string when it is not one of
// the three the contract defines.
QString interfaceKindName(InterfaceKind kind);
// Inverse of interfaceKindName; false keeps the caller from inventing a kind.
bool interfaceKindFromName(QStringView name, InterfaceKind& kind);

struct SnapshotParseResult {
    std::optional<Snapshot> snapshot;
    QString error; // short ASCII reason; empty when `snapshot` is set
};

// Decodes exactly one snapshot frame, already framed by the caller. The whole
// object is validated before it is returned, so a rejected frame can never
// partially update the UI.
SnapshotParseResult decodeSnapshot(const msgpack::object& frame);

// Encodes one snapshot as one complete frame. `ok` is false when a value could
// not be represented (an unset time), so the daemon never sends a snapshot it
// could not describe.
QByteArray encodeSnapshot(const Snapshot& snapshot, bool& ok);

} // namespace ball
