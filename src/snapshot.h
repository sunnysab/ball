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
// both be checked before process rates may be shown as live. `info` carries
// explanations only (unmatched sockets, expired PIDs, kTLS probes): it never
// drives `incomplete` or the HUD warning.
struct ProcessStatus {
    bool available = false;
    bool stale = true;
    bool incomplete = false;
    QString error;
    QStringList notes;
    QStringList info;
};

// One process taking part in a connection. `client`/`server` are the processes
// that established or accepted it, `io` the ones that actually moved bytes.
// `pid` is 0 when the collector could not tell, -1 only when the collector says
// the process is ambiguous, and a positive PID otherwise; `io` only accepts
// positive PIDs. `startNs` is the same nanosecond identity as
// ProcessSnapshot::startNs and keeps its full 64-bit width.
struct ConnectionProcess {
    int pid = 0;
    quint64 startNs = 0;
    QString name;
    quint64 rxBytes = 0;
    quint64 txBytes = 0;
};

// Bounds for the connection section. A value outside them is refused, never
// truncated, so a reader can trust that every present field is complete. All
// text caps are UTF-8 bytes, which is what the wire carries.
//
// One shared text cap keeps a real SNI name (up to 253 bytes) and a detail
// carrying several DNS explanations legal, and the list caps are the table
// sizes the cache can actually hold. The 16 MiB frame limit is not derived from
// these: encodeSnapshot measures the finished frame and refuses it whole when it
// is too large, so no arithmetic over field caps has to be trusted.
namespace connection {
inline constexpr int kMaxRows = 8192;
// One text field, or one entry of a text list, in UTF-8 bytes.
inline constexpr int kMaxTextBytes = 4096;
inline constexpr int kMaxInterfaces = 64;
inline constexpr int kMaxIo = 256;
// Any explanation list: process_status.notes/info and connection_status.notes.
inline constexpr int kMaxNotes = 32;
} // namespace connection

// One observed connection. `id` is the collector's identity and is never zero;
// `end` is empty while the connection is live and holds the end reason once it
// finished. Byte and packet counters are cumulative since the collector first
// observed the connection, so they are replaced, never added up.
struct ConnectionSnapshot {
    quint64 id = 0;
    QString protocol;
    QString app;
    QString state;
    QString direction;
    QString source;
    QString target;
    QString name;
    QString detail;
    QString end;
    QStringList interfaces;
    QDateTime firstSeen;
    QDateTime lastSeen;
    quint64 rxBytes = 0;
    quint64 txBytes = 0;
    quint64 packets = 0;
    quint64 retransmits = 0;
    double rxBps = 0.0;
    double txBps = 0.0;
    qint64 rttUs = 0;
    QString rttSource;
    ConnectionProcess client;
    ConnectionProcess server;
    QList<ConnectionProcess> io;
};

// Quality of the connection side of one snapshot, kept apart from the process
// and interface status so a missing collector cannot hide live interfaces.
// `truncated` says the bounded cache had to reclaim rows, so the list is not
// the full history.
struct ConnectionStatus {
    bool available = false;
    bool stale = true;
    bool truncated = false;
    QStringList notes;
};

struct Snapshot {
    int version = 0;
    QDateTime started; // backend monitoring start, changes when the backend restarts
    QDateTime at;      // instant this snapshot was generated
    bool interfacesAvailable = false;
    QString interfaceError;
    ProcessStatus processStatus;
    // Collector-run totals, independent of interface and process accumulation.
    quint64 ipPackets = 0;
    quint64 ipBytes = 0;
    QList<InterfaceSnapshot> interfaces;
    QList<ProcessSnapshot> processes;
    QList<ConnectionSnapshot> connections;
    ConnectionStatus connectionStatus;
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
inline constexpr QLatin1StringView kIpPackets("ip_packets");
inline constexpr QLatin1StringView kIpBytes("ip_bytes");
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
inline constexpr QLatin1StringView kConnections("connections");
inline constexpr QLatin1StringView kConnectionStatus("connection_status");
inline constexpr QLatin1StringView kInfo("info");
inline constexpr QLatin1StringView kTruncated("truncated");
inline constexpr QLatin1StringView kId("id");
inline constexpr QLatin1StringView kProtocol("protocol");
inline constexpr QLatin1StringView kApp("app");
inline constexpr QLatin1StringView kState("state");
inline constexpr QLatin1StringView kDirection("direction");
inline constexpr QLatin1StringView kSource("source");
inline constexpr QLatin1StringView kTarget("target");
inline constexpr QLatin1StringView kDetail("detail");
inline constexpr QLatin1StringView kEnd("end");
inline constexpr QLatin1StringView kFirstSeen("first_seen");
inline constexpr QLatin1StringView kLastSeen("last_seen");
inline constexpr QLatin1StringView kPackets("packets");
inline constexpr QLatin1StringView kRetransmits("retransmits");
inline constexpr QLatin1StringView kRttUs("rtt_us");
inline constexpr QLatin1StringView kRttSource("rtt_source");
inline constexpr QLatin1StringView kClient("client");
inline constexpr QLatin1StringView kServer("server");
inline constexpr QLatin1StringView kIo("io");
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
// not be represented (an unset time) or when the finished frame would exceed
// mp::kMaxFrameBytes; a too-large frame is never sent in parts or trimmed. The
// daemon therefore never sends a snapshot it could not describe in full.
QByteArray encodeSnapshot(const Snapshot& snapshot, bool& ok);

} // namespace ball
