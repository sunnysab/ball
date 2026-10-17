// ball: a network traffic HUD for KDE Wayland sessions.
#include "snapshot.h"

#include "messagepack.h"

#include <msgpack.hpp>

#include <QSet>

#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace ball {
namespace {

constexpr quint64 kIntMax = quint64(std::numeric_limits<int>::max());

SnapshotParseResult reject(const char* reason) {
    return SnapshotParseResult{std::nullopt, QLatin1String(reason)};
}

bool readCounters(const msgpack::object& parent, QLatin1StringView key, ByteCounters& counters) {
    const msgpack::object* const value = mp::member(parent, key);
    const msgpack::object* const rx =
        value != nullptr ? mp::member(*value, field::kRxBytes) : nullptr;
    const msgpack::object* const tx =
        value != nullptr ? mp::member(*value, field::kTxBytes) : nullptr;
    return rx != nullptr && tx != nullptr && mp::readCount(*rx, counters.rxBytes) &&
           mp::readCount(*tx, counters.txBytes);
}

bool readRate(const msgpack::object& parent, double& rxBps, double& txBps) {
    const msgpack::object* const value = mp::member(parent, field::kRate);
    const msgpack::object* const rx =
        value != nullptr ? mp::member(*value, field::kRxBps) : nullptr;
    const msgpack::object* const tx =
        value != nullptr ? mp::member(*value, field::kTxBps) : nullptr;
    return rx != nullptr && tx != nullptr && mp::readNumber(*rx, rxBps) &&
           mp::readNumber(*tx, txBps) && rxBps >= 0.0 && txBps >= 0.0;
}

bool readTimestamp(const msgpack::object& parent, QLatin1StringView key, QDateTime& out) {
    const msgpack::object* const value = mp::member(parent, key);
    if (value == nullptr) {
        return false;
    }
    mp::Instant instant;
    if (!mp::readInstant(*value, instant)) {
        return false;
    }
    const QDateTime parsed = mp::timestampToLocalTime(instant);
    if (!parsed.isValid()) {
        return false;
    }
    out = parsed;
    return true;
}

// Shared by interfaces and processes: the two rows differ only in the identity
// fields, which the caller reads first.
bool readRowCounters(const msgpack::object& row, ByteCounters& today, ByteCounters& session,
                     double& rxBps, double& txBps) {
    return readCounters(row, field::kToday, today) && readCounters(row, field::kSession, session) &&
           readRate(row, rxBps, txBps);
}

bool readInterfaces(const msgpack::object& parent, QList<InterfaceSnapshot>& out) {
    const msgpack::object* const array = mp::member(parent, field::kInterfaces);
    const msgpack::object* entries = nullptr;
    quint32 size = 0;
    if (array == nullptr || !mp::arrayEntries(*array, entries, size)) {
        return false;
    }
    QSet<quint32> indices;
    for (quint32 i = 0; i < size; ++i) {
        const msgpack::object& row = entries[i];
        quint64 index = 0;
        const msgpack::object* const indexValue = mp::member(row, field::kIndex);
        if (indexValue == nullptr || !mp::readCount(*indexValue, index) || index == 0 ||
            index > kIntMax) {
            return false;
        }
        const msgpack::object* const kindValue = mp::member(row, field::kKind);
        bool named = false;
        InterfaceSnapshot interface;
        interface.index = quint32(index);
        const msgpack::object* const nameValue = mp::member(row, field::kName);
        if (nameValue == nullptr) {
            return false;
        }
        interface.name = mp::ascii(*nameValue, named);
        if (!named || interface.name.isEmpty()) {
            return false;
        }
        bool starts = false;
        const msgpack::object* const upValue = mp::member(row, field::kUp);
        if (kindValue == nullptr || upValue == nullptr) {
            return false;
        }
        const QString kindText = mp::ascii(*kindValue, starts);
        if (!starts || !interfaceKindFromName(kindText, interface.kind) ||
            !mp::readBool(*upValue, interface.up)) {
            return false;
        }
        if (!readRowCounters(row, interface.today, interface.session, interface.rxBps,
                             interface.txBps)) {
            return false;
        }
        if (indices.contains(interface.index)) {
            return false;
        }
        indices.insert(interface.index);
        out.append(interface);
    }
    return true;
}

bool readProcesses(const msgpack::object& parent, QList<ProcessSnapshot>& out) {
    const msgpack::object* const array = mp::member(parent, field::kProcesses);
    const msgpack::object* entries = nullptr;
    quint32 size = 0;
    if (array == nullptr || !mp::arrayEntries(*array, entries, size)) {
        return false;
    }
    QSet<QPair<int, quint64>> identities;
    for (quint32 i = 0; i < size; ++i) {
        const msgpack::object& row = entries[i];
        quint64 pid = 0;
        const msgpack::object* const pidValue = mp::member(row, field::kPid);
        if (pidValue == nullptr || !mp::readCount(*pidValue, pid) || pid == 0 || pid > kIntMax) {
            return false;
        }
        // A zero start_ns has no wire representation because the encoder omits
        // it; absent and zero are then the same identity.
        quint64 startNs = 0;
        const msgpack::object* const startValue = mp::member(row, field::kStartNs);
        if (startValue != nullptr && !mp::readCount(*startValue, startNs)) {
            return false;
        }
        ProcessSnapshot process;
        process.pid = int(pid);
        process.startNs = startNs;
        const msgpack::object* const nameValue = mp::member(row, field::kName);
        if (nameValue != nullptr) {
            bool ok = false;
            process.name = mp::ascii(*nameValue, ok);
            if (!ok) {
                return false;
            }
        }
        const msgpack::object* const presentValue = mp::member(row, field::kPresent);
        if (presentValue == nullptr || !mp::readBool(*presentValue, process.present)) {
            return false;
        }
        if (!readRowCounters(row, process.today, process.session, process.rxBps, process.txBps)) {
            return false;
        }
        const QPair<int, quint64> identity{process.pid, process.startNs};
        if (identities.contains(identity)) {
            return false;
        }
        identities.insert(identity);
        out.append(process);
    }
    return true;
}

// Reads a list of bounded strings. `required` separates the note list the schema
// has always carried from a newer optional one: a missing optional list stays
// empty, while a missing required list is a broken frame. A present list is
// always validated, because these strings are shown verbatim.
bool readTextList(const msgpack::object& parent, QLatin1StringView key, QStringList& out,
                  quint32 maxCount, bool required) {
    const msgpack::object* const value = mp::member(parent, key);
    if (value == nullptr) {
        return !required;
    }
    const msgpack::object* entries = nullptr;
    quint32 size = 0;
    if (!mp::arrayEntries(*value, entries, size) || size > maxCount) {
        return false;
    }
    for (quint32 i = 0; i < size; ++i) {
        bool ok = false;
        const QString text = mp::ascii(entries[i], ok);
        if (!ok || text.toUtf8().size() > connection::kMaxTextBytes) {
            return false;
        }
        out.append(text);
    }
    return true;
}

// One optional text field. A missing key leaves the default (empty) value and a
// present one must be a bounded, control-free string.
bool readText(const msgpack::object& parent, QLatin1StringView key, QString& out) {
    const msgpack::object* const value = mp::member(parent, key);
    if (value == nullptr) {
        return true;
    }
    bool ok = false;
    const QString text = mp::ascii(*value, ok);
    if (!ok || text.toUtf8().size() > connection::kMaxTextBytes) {
        return false;
    }
    out = text;
    return true;
}

// One optional counter. Absent means zero; present must be an exact uint64.
bool readOptionalCount(const msgpack::object& parent, QLatin1StringView key, quint64& out) {
    const msgpack::object* const value = mp::member(parent, key);
    return value == nullptr || mp::readCount(*value, out);
}

// One optional instant. A present value must be a real MessagePack timestamp.
bool readOptionalTimestamp(const msgpack::object& parent, QLatin1StringView key, QDateTime& out) {
    const msgpack::object* const value = mp::member(parent, key);
    if (value == nullptr) {
        return true;
    }
    mp::Instant instant;
    if (!mp::readInstant(*value, instant)) {
        return false;
    }
    out = mp::timestampToLocalTime(instant);
    return out.isValid();
}

// One optional rate map. Absent means zero; a present one must be finite and
// non-negative in both directions, exactly like an interface or process rate.
bool readOptionalRate(const msgpack::object& parent, double& rxBps, double& txBps) {
    return mp::member(parent, field::kRate) == nullptr || readRate(parent, rxBps, txBps);
}

// A PID inside a connection row. `client`/`server` may be 0 (unknown) or -1
// (the collector says the process is ambiguous); `io[]` only names a real
// process, so it accepts positive PIDs only. Anything else, including a
// fractional value, is refused rather than rounded.
bool readPid(const msgpack::object& value, int& out, bool allowUnknown) {
    if (value.type == msgpack::type::NEGATIVE_INTEGER) {
        if (!allowUnknown || value.via.i64 != -1) {
            return false;
        }
        out = -1;
        return true;
    }
    quint64 pid = 0;
    if (!mp::readCount(value, pid) || pid > kIntMax || (!allowUnknown && pid == 0)) {
        return false;
    }
    out = int(pid);
    return true;
}

// RTT is non-negative and must fit the signed field used by the UI.
bool readOptionalSignedCount(const msgpack::object& parent, QLatin1StringView key, qint64& out) {
    const msgpack::object* const value = mp::member(parent, key);
    if (value == nullptr) {
        return true;
    }
    quint64 count = 0;
    if (!mp::readCount(*value, count) || count > quint64(std::numeric_limits<qint64>::max())) {
        return false;
    }
    out = qint64(count);
    return true;
}

bool readConnectionProcess(const msgpack::object& parent, ConnectionProcess& process,
                           bool allowUnknownPid) {
    if (parent.type != msgpack::type::MAP) {
        return false;
    }
    // The PID carries the identity. It may be absent, which is 0 (unknown):
    // `client`/`server` accept that and the -1 "ambiguous" sentinel, `io[]` only
    // a positive PID because an io entry exists to name one real process.
    const msgpack::object* const pid = mp::member(parent, field::kPid);
    if (pid != nullptr && !readPid(*pid, process.pid, allowUnknownPid)) {
        return false;
    }
    if (!allowUnknownPid && process.pid <= 0) {
        return false;
    }
    return readOptionalCount(parent, field::kStartNs, process.startNs) &&
           readText(parent, field::kName, process.name) &&
           readOptionalCount(parent, field::kRxBytes, process.rxBytes) &&
           readOptionalCount(parent, field::kTxBytes, process.txBytes);
}

// One optional process object: `client` and `server` are absent when the
// collector could not name them, which is not the same as a process with PID 0.
bool readConnectionProcessMember(const msgpack::object& parent, QLatin1StringView key,
                                 ConnectionProcess& out, bool allowUnknownPid) {
    const msgpack::object* const value = mp::member(parent, key);
    if (value == nullptr) {
        return true;
    }
    return readConnectionProcess(*value, out, allowUnknownPid);
}

// A member that is an array of connection-process objects, with a bound on both
// the element count and the PID rules of each context. Absent is a legal empty
// list; present must be a well-formed array of usable rows.
bool readConnectionProcessList(const msgpack::object& parent, QLatin1StringView key,
                               QList<ConnectionProcess>& out, bool allowUnknownPid) {
    const msgpack::object* const value = mp::member(parent, key);
    if (value == nullptr) {
        return true;
    }
    const msgpack::object* entries = nullptr;
    quint32 size = 0;
    if (!mp::arrayEntries(*value, entries, size) || size > quint32(connection::kMaxIo)) {
        return false;
    }
    for (quint32 i = 0; i < size; ++i) {
        ConnectionProcess process;
        if (!readConnectionProcess(entries[i], process, allowUnknownPid)) {
            return false;
        }
        out.append(process);
    }
    return true;
}

// One connection row. `id` is the row's identity and must be present and
// nonzero; every other field is optional on the wire but strictly validated
// when it appears, so a partially filled row is still readable while a wrong
// value is refused whole.
bool readConnection(const msgpack::object& row, ConnectionSnapshot& out) {
    const msgpack::object* const id = mp::member(row, field::kId);
    quint64 value = 0;
    if (id == nullptr || !mp::readCount(*id, value) || value == 0) {
        return false;
    }
    out.id = value;
    return readText(row, field::kProtocol, out.protocol) && readText(row, field::kApp, out.app) &&
           readText(row, field::kState, out.state) &&
           readText(row, field::kDirection, out.direction) &&
           readText(row, field::kSource, out.source) && readText(row, field::kTarget, out.target) &&
           readText(row, field::kName, out.name) && readText(row, field::kDetail, out.detail) &&
           readText(row, field::kEnd, out.end) &&
           readTextList(row, field::kInterfaces, out.interfaces,
                        quint32(connection::kMaxInterfaces), false) &&
           readOptionalTimestamp(row, field::kFirstSeen, out.firstSeen) &&
           readOptionalTimestamp(row, field::kLastSeen, out.lastSeen) &&
           readOptionalCount(row, field::kRxBytes, out.rxBytes) &&
           readOptionalCount(row, field::kTxBytes, out.txBytes) &&
           readOptionalCount(row, field::kPackets, out.packets) &&
           readOptionalCount(row, field::kRetransmits, out.retransmits) &&
           readOptionalRate(row, out.rxBps, out.txBps) &&
           readOptionalSignedCount(row, field::kRttUs, out.rttUs) &&
           readText(row, field::kRttSource, out.rttSource) &&
           readConnectionProcessMember(row, field::kClient, out.client, true) &&
           readConnectionProcessMember(row, field::kServer, out.server, true) &&
           readConnectionProcessList(row, field::kIo, out.io, false);
}

// The connection table and its status. Both are absent on an older daemon, and
// an absent table means "connections are not supported", never an empty list
// pretending to be available.
bool readConnections(const msgpack::object& parent, QList<ConnectionSnapshot>& out) {
    const msgpack::object* const value = mp::member(parent, field::kConnections);
    if (value == nullptr) {
        return true;
    }
    const msgpack::object* entries = nullptr;
    quint32 size = 0;
    if (!mp::arrayEntries(*value, entries, size) || size > quint32(connection::kMaxRows)) {
        return false;
    }
    QSet<quint64> identities;
    out.reserve(qsizetype(size));
    for (quint32 i = 0; i < size; ++i) {
        ConnectionSnapshot row;
        if (!readConnection(entries[i], row)) {
            return false;
        }
        // Two rows with one ID are ambiguous: the cache is keyed by it, so a
        // duplicate could hide a row or make an update overwrite the wrong one.
        if (identities.contains(row.id)) {
            return false;
        }
        identities.insert(row.id);
        out.append(row);
    }
    return true;
}

bool readConnectionStatus(const msgpack::object& parent, ConnectionStatus& status) {
    const msgpack::object* const value = mp::member(parent, field::kConnectionStatus);
    if (value == nullptr) {
        return true; // the whole section is newer than the daemon that wrote this
    }
    const msgpack::object* const available = mp::member(*value, field::kAvailable);
    const msgpack::object* const stale = mp::member(*value, field::kStale);
    const msgpack::object* const truncated = mp::member(*value, field::kTruncated);
    if (available == nullptr || stale == nullptr || truncated == nullptr ||
        !mp::readBool(*available, status.available) || !mp::readBool(*stale, status.stale) ||
        !mp::readBool(*truncated, status.truncated)) {
        return false;
    }
    return readTextList(*value, field::kNotes, status.notes, quint32(connection::kMaxNotes),
                        true) &&
           readOptionalCount(*value, field::kGeneration, status.generation);
}

bool readProcessStatus(const msgpack::object& parent, ProcessStatus& status) {
    const msgpack::object* const value = mp::member(parent, field::kProcessStatus);
    if (value == nullptr) {
        return false;
    }
    const msgpack::object* const available = mp::member(*value, field::kAvailable);
    const msgpack::object* const stale = mp::member(*value, field::kStale);
    const msgpack::object* const incomplete = mp::member(*value, field::kIncomplete);
    const msgpack::object* const error = mp::member(*value, field::kError);
    if (available == nullptr || stale == nullptr || incomplete == nullptr || error == nullptr ||
        !mp::readBool(*available, status.available) || !mp::readBool(*stale, status.stale) ||
        !mp::readBool(*incomplete, status.incomplete)) {
        return false;
    }
    bool ok = false;
    status.error = mp::ascii(*error, ok);
    if (!ok) {
        return false;
    }
    // `notes` is required; `info` explains quality problems without touching
    // `incomplete` and is newer, so a frame without it is still valid and simply
    // carries no explanations.
    return readTextList(*value, field::kNotes, status.notes, quint32(connection::kMaxNotes),
                        true) &&
           readTextList(*value, field::kInfo, status.info, quint32(connection::kMaxNotes), false);
}

} // namespace

bool decodeConnection(const msgpack::object& row, ConnectionSnapshot& out) {
    // Decode into a temporary so a row that fails half way leaves the caller's
    // value exactly as it was.
    ConnectionSnapshot candidate;
    if (!readConnection(row, candidate)) {
        return false;
    }
    out = std::move(candidate);
    return true;
}

QString interfaceKindName(InterfaceKind kind) {
    switch (kind) {
    case InterfaceKind::Physical:
        return QStringLiteral("physical");
    case InterfaceKind::Virtual:
        return QStringLiteral("virtual");
    case InterfaceKind::Loopback:
        return QStringLiteral("loopback");
    }
    return {};
}

bool interfaceKindFromName(QStringView name, InterfaceKind& kind) {
    if (name == u"physical") {
        kind = InterfaceKind::Physical;
    } else if (name == u"virtual") {
        kind = InterfaceKind::Virtual;
    } else if (name == u"loopback") {
        kind = InterfaceKind::Loopback;
    } else {
        return false;
    }
    return true;
}

SnapshotParseResult decodeSnapshot(const msgpack::object& frame) {
    quint64 version = 0;
    const msgpack::object* const versionValue = mp::member(frame, field::kVersion);
    if (versionValue == nullptr || !mp::readCount(*versionValue, version)) {
        return reject("bad version");
    }
    if (version != 1) {
        return reject("unsupported version");
    }
    const msgpack::object* const kindValue = mp::member(frame, field::kKind);
    bool ok = false;
    const QString kind = kindValue != nullptr ? mp::ascii(*kindValue, ok) : QString();
    if (!ok || kind != QLatin1String("snapshot")) {
        return reject("not a snapshot");
    }

    Snapshot snapshot;
    snapshot.version = int(version);
    if (!readTimestamp(frame, field::kStarted, snapshot.started)) {
        return reject("bad started");
    }
    if (!readTimestamp(frame, field::kAt, snapshot.at)) {
        return reject("bad at");
    }

    const msgpack::object* const available = mp::member(frame, field::kInterfacesAvailable);
    if (available == nullptr || !mp::readBool(*available, snapshot.interfacesAvailable)) {
        return reject("bad interfaces_available");
    }
    const msgpack::object* const interfaceError = mp::member(frame, field::kInterfaceError);
    if (interfaceError == nullptr) {
        return reject("bad interface_error");
    }
    snapshot.interfaceError = mp::ascii(*interfaceError, ok);
    if (!ok) {
        return reject("bad interface_error");
    }

    const msgpack::object* const ipPackets = mp::member(frame, field::kIpPackets);
    const msgpack::object* const ipBytes = mp::member(frame, field::kIpBytes);
    if (ipPackets != nullptr && !mp::readCount(*ipPackets, snapshot.ipPackets)) {
        return reject("bad ip_packets");
    }
    if (ipBytes != nullptr && !mp::readCount(*ipBytes, snapshot.ipBytes)) {
        return reject("bad ip_bytes");
    }

    if (!readProcessStatus(frame, snapshot.processStatus)) {
        return reject("bad process_status");
    }
    if (!readInterfaces(frame, snapshot.interfaces)) {
        return reject("bad interfaces");
    }
    if (!readProcesses(frame, snapshot.processes)) {
        return reject("bad processes");
    }
    // Both keys form one optional section; half a section cannot claim an
    // available empty table when the connection rows are actually missing.
    if ((mp::member(frame, field::kConnections) == nullptr) !=
        (mp::member(frame, field::kConnectionStatus) == nullptr)) {
        return reject("incomplete connection section");
    }
    if (!readConnections(frame, snapshot.connections)) {
        return reject("bad connections");
    }
    if (!readConnectionStatus(frame, snapshot.connectionStatus)) {
        return reject("bad connection_status");
    }

    return SnapshotParseResult{std::move(snapshot), QString()};
}

namespace {

// Writes the schema. Every field is written explicitly, including the empty
// strings and lists the decoder requires, so a reader never has to guess a
// default and a missing key is always a bug on the writer's side.
class SnapshotWriter {
public:
    explicit SnapshotWriter(quint32 mapSize) : m_packer(m_buffer) { m_packer.pack_map(mapSize); }

    void key(QLatin1StringView name) {
        m_packer.pack_str(uint32_t(name.size()));
        m_packer.pack_str_body(name.data(), uint32_t(name.size()));
    }
    void string(const QString& text) {
        const QByteArray utf8 = text.toUtf8();
        m_packer.pack_str(uint32_t(utf8.size()));
        m_packer.pack_str_body(utf8.constData(), uint32_t(utf8.size()));
    }
    void count(quint64 value) { m_packer.pack(value); }
    // Counters are unsigned, but a sentinel like pid -1 must keep its sign.
    void signedCount(qint64 value) { m_packer.pack(value); }
    void boolean(bool value) { m_packer.pack(value); }
    void number(double value) { m_packer.pack(finite(value)); }
    void timestamp(const QDateTime& value) {
        if (!value.isValid()) {
            m_ok = false;
            m_packer.pack(0);
            return;
        }
        // MessagePack's own timestamp extension keeps the instant exact and
        // avoids inventing a string format for it.
        m_packer.pack(std::chrono::system_clock::time_point{
            std::chrono::milliseconds{value.toUTC().toMSecsSinceEpoch()}});
    }
    void beginMap(quint32 size) { m_packer.pack_map(size); }
    void beginArray(quint32 size) { m_packer.pack_array(size); }
    void raw(const QByteArray& text) {
        m_packer.pack_str(uint32_t(text.size()));
        m_packer.pack_str_body(text.constData(), uint32_t(text.size()));
    }

    // False when a value could not be represented; the frame is then discarded
    // rather than sent without it.
    bool ok() const { return m_ok; }
    qsizetype size() const { return qsizetype(m_buffer.size()); }
    QByteArray take() { return QByteArray(m_buffer.data(), qsizetype(m_buffer.size())); }

private:
    // A rate must stay comparable in the UI, so an implausible value becomes
    // zero instead of an infinity the reader would reject.
    static double finite(double value) {
        return std::isfinite(value) && value >= 0.0 ? value : 0.0;
    }

    msgpack::sbuffer m_buffer;
    msgpack::packer<msgpack::sbuffer> m_packer;
    bool m_ok = true;
};

void writeCounters(SnapshotWriter& writer, const ByteCounters& counters) {
    writer.beginMap(2);
    writer.key(field::kRxBytes);
    writer.count(counters.rxBytes);
    writer.key(field::kTxBytes);
    writer.count(counters.txBytes);
}

// One connection-process object. A name is always written (possibly empty) so a
// reader needs no default; the PID keeps its sign so -1 stays -1.
void writeConnectionProcess(SnapshotWriter& writer, const ConnectionProcess& process) {
    writer.beginMap(5);
    writer.key(field::kPid);
    writer.signedCount(qint64(process.pid));
    writer.key(field::kStartNs);
    writer.count(process.startNs);
    writer.key(field::kName);
    writer.string(process.name);
    writer.key(field::kRxBytes);
    writer.count(process.rxBytes);
    writer.key(field::kTxBytes);
    writer.count(process.txBytes);
}

// One connection row. Every field is written explicitly, including the empty
// strings and lists, so the decoder's optional-field rules are only needed for a
// frame from an older daemon. The two instants are the exception: an unset time
// has no wire form, so it is omitted rather than failing the whole snapshot.
void writeConnection(SnapshotWriter& writer, const ConnectionSnapshot& row) {
    quint32 fields = 21;
    if (row.firstSeen.isValid()) {
        ++fields;
    }
    if (row.lastSeen.isValid()) {
        ++fields;
    }
    writer.beginMap(fields);
    writer.key(field::kId);
    writer.count(row.id);
    writer.key(field::kProtocol);
    writer.string(row.protocol);
    writer.key(field::kApp);
    writer.string(row.app);
    writer.key(field::kState);
    writer.string(row.state);
    writer.key(field::kDirection);
    writer.string(row.direction);
    writer.key(field::kSource);
    writer.string(row.source);
    writer.key(field::kTarget);
    writer.string(row.target);
    writer.key(field::kName);
    writer.string(row.name);
    writer.key(field::kDetail);
    writer.string(row.detail);
    writer.key(field::kEnd);
    writer.string(row.end);
    writer.key(field::kInterfaces);
    writer.beginArray(quint32(row.interfaces.size()));
    for (const QString& name : row.interfaces) {
        writer.string(name);
    }
    if (row.firstSeen.isValid()) {
        writer.key(field::kFirstSeen);
        writer.timestamp(row.firstSeen);
    }
    if (row.lastSeen.isValid()) {
        writer.key(field::kLastSeen);
        writer.timestamp(row.lastSeen);
    }
    writer.key(field::kRxBytes);
    writer.count(row.rxBytes);
    writer.key(field::kTxBytes);
    writer.count(row.txBytes);
    writer.key(field::kPackets);
    writer.count(row.packets);
    writer.key(field::kRetransmits);
    writer.count(row.retransmits);
    writer.key(field::kRate);
    writer.beginMap(2);
    writer.key(field::kRxBps);
    writer.number(row.rxBps);
    writer.key(field::kTxBps);
    writer.number(row.txBps);
    writer.key(field::kRttUs);
    writer.signedCount(row.rttUs);
    writer.key(field::kRttSource);
    writer.string(row.rttSource);
    writer.key(field::kClient);
    writeConnectionProcess(writer, row.client);
    writer.key(field::kServer);
    writeConnectionProcess(writer, row.server);
    writer.key(field::kIo);
    writer.beginArray(quint32(row.io.size()));
    for (const ConnectionProcess& process : row.io) {
        writeConnectionProcess(writer, process);
    }
}

} // namespace

QByteArray encodeSnapshot(const Snapshot& snapshot, bool& ok) {
    // The connection section is one unit, so its two keys are always written
    // together; an older reader ignores both, and a current one never sees a
    // half-written pair.
    SnapshotWriter writer(13);
    writer.key(field::kVersion);
    writer.count(uint64_t(snapshot.version));
    writer.key(field::kKind);
    writer.string(field::kSnapshotKind);
    writer.key(field::kStarted);
    writer.timestamp(snapshot.started);
    writer.key(field::kAt);
    writer.timestamp(snapshot.at);
    writer.key(field::kInterfacesAvailable);
    writer.boolean(snapshot.interfacesAvailable);
    writer.key(field::kInterfaceError);
    writer.string(snapshot.interfaceError);

    writer.key(field::kIpPackets);
    writer.count(snapshot.ipPackets);
    writer.key(field::kIpBytes);
    writer.count(snapshot.ipBytes);

    writer.key(field::kProcessStatus);
    writer.beginMap(6);
    writer.key(field::kAvailable);
    writer.boolean(snapshot.processStatus.available);
    writer.key(field::kStale);
    writer.boolean(snapshot.processStatus.stale);
    writer.key(field::kIncomplete);
    writer.boolean(snapshot.processStatus.incomplete);
    writer.key(field::kError);
    writer.string(snapshot.processStatus.error);
    writer.key(field::kNotes);
    writer.beginArray(quint32(snapshot.processStatus.notes.size()));
    for (const QString& note : snapshot.processStatus.notes) {
        writer.string(note);
    }
    // Explanations are written even when empty, so the decoder's optional rule
    // is only needed for a frame from an older daemon.
    writer.key(field::kInfo);
    writer.beginArray(quint32(snapshot.processStatus.info.size()));
    for (const QString& note : snapshot.processStatus.info) {
        writer.string(note);
    }

    writer.key(field::kInterfaces);
    writer.beginArray(quint32(snapshot.interfaces.size()));
    for (const InterfaceSnapshot& row : snapshot.interfaces) {
        writer.beginMap(7);
        writer.key(field::kIndex);
        writer.count(row.index);
        writer.key(field::kName);
        writer.string(row.name);
        writer.key(field::kKind);
        writer.string(interfaceKindName(row.kind));
        writer.key(field::kUp);
        writer.boolean(row.up);
        writer.key(field::kToday);
        writeCounters(writer, row.today);
        writer.key(field::kSession);
        writeCounters(writer, row.session);
        writer.key(field::kRate);
        writer.beginMap(2);
        writer.key(field::kRxBps);
        writer.number(row.rxBps);
        writer.key(field::kTxBps);
        writer.number(row.txBps);
    }

    writer.key(field::kProcesses);
    writer.beginArray(quint32(snapshot.processes.size()));
    for (const ProcessSnapshot& row : snapshot.processes) {
        writer.beginMap(7);
        writer.key(field::kPid);
        writer.count(quint64(row.pid));
        writer.key(field::kStartNs);
        writer.count(row.startNs);
        writer.key(field::kName);
        writer.string(row.name);
        writer.key(field::kPresent);
        writer.boolean(row.present);
        writer.key(field::kToday);
        writeCounters(writer, row.today);
        writer.key(field::kSession);
        writeCounters(writer, row.session);
        writer.key(field::kRate);
        writer.beginMap(2);
        writer.key(field::kRxBps);
        writer.number(row.rxBps);
        writer.key(field::kTxBps);
        writer.number(row.txBps);
    }

    writer.key(field::kConnections);
    writer.beginArray(quint32(snapshot.connections.size()));
    for (const ConnectionSnapshot& row : snapshot.connections) {
        writeConnection(writer, row);
    }
    writer.key(field::kConnectionStatus);
    writer.beginMap(5);
    writer.key(field::kGeneration);
    writer.count(snapshot.connectionStatus.generation);
    writer.key(field::kAvailable);
    writer.boolean(snapshot.connectionStatus.available);
    writer.key(field::kStale);
    writer.boolean(snapshot.connectionStatus.stale);
    writer.key(field::kTruncated);
    writer.boolean(snapshot.connectionStatus.truncated);
    writer.key(field::kNotes);
    writer.beginArray(quint32(snapshot.connectionStatus.notes.size()));
    for (const QString& note : snapshot.connectionStatus.notes) {
        writer.string(note);
    }

    ok = writer.ok();
    // The finished size is what decides: the field caps are a sanity bound, not
    // a proof, so an oversized frame is refused whole rather than trimmed or
    // sent in pieces. A receiver would reject it anyway at its own 16 MiB cap.
    if (ok && writer.size() > qsizetype(mp::kMaxFrameBytes)) {
        ok = false;
    }
    return ok ? writer.take() : QByteArray();
}

} // namespace ball
