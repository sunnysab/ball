// ball: a network traffic HUD for KDE Wayland sessions.
#include "snapshot.h"

#include "messagepack.h"

#include <msgpack.hpp>

#include <QSet>

#include <chrono>
#include <cmath>
#include <limits>

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
    const msgpack::object* const notes = mp::member(*value, field::kNotes);
    const msgpack::object* entries = nullptr;
    quint32 size = 0;
    if (notes == nullptr || !mp::arrayEntries(*notes, entries, size)) {
        return false;
    }
    for (quint32 i = 0; i < size; ++i) {
        status.notes.append(mp::ascii(entries[i], ok));
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

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

} // namespace

QByteArray encodeSnapshot(const Snapshot& snapshot, bool& ok) {
    SnapshotWriter writer(11);
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
    writer.beginMap(5);
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

    ok = writer.ok();
    return ok ? writer.take() : QByteArray();
}

} // namespace ball
