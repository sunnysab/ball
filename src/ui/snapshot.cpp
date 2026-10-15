// ball: a network traffic HUD for KDE Wayland sessions.
#include "ui/snapshot.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QSet>

#include <cmath>
#include <limits>

namespace ball::ui {
namespace {

constexpr int kIntMax = std::numeric_limits<int>::max();

SnapshotParseResult reject(const char* reason) {
    return SnapshotParseResult{std::nullopt, QLatin1String(reason)};
}

// Reads a non-negative JSON integer exactly, using the JSON literal rather than
// its double view, so counters past 2^53 keep every bit.
bool readJsonCount(const QJsonObject& object, const char* key, quint64& value) {
    const QJsonValue json = object.value(QLatin1String(key));
    if (!json.isDouble()) {
        return false;
    }
    const double asDouble = json.toDouble();
    if (!std::isfinite(asDouble) || asDouble < 0.0 || std::trunc(asDouble) != asDouble) {
        return false;
    }
    const qint64 exact = json.toInteger(-1);
    if (exact < 0) {
        return false;
    }
    value = static_cast<quint64>(exact);
    return true;
}

// Byte counters travel as decimal strings precisely so 64-bit values survive
// JSON; anything else (negative, fractional, exponent, overflow) is a bad frame.
bool readDecimalString(const QJsonObject& object, const char* key, quint64& value) {
    const QJsonValue json = object.value(QLatin1String(key));
    if (!json.isString()) {
        return false;
    }
    const QString text = json.toString();
    if (text.isEmpty()) {
        return false;
    }
    for (const QChar character : text) {
        if (!character.isDigit()) {
            return false;
        }
    }
    bool ok = false;
    const quint64 parsed = text.toULongLong(&ok);
    if (!ok) {
        return false;
    }
    value = parsed;
    return true;
}

bool readCounters(const QJsonObject& parent, const char* key, ByteCounters& counters) {
    const QJsonValue json = parent.value(QLatin1String(key));
    if (!json.isObject()) {
        return false;
    }
    const QJsonObject object = json.toObject();
    return readDecimalString(object, "rx_bytes", counters.rxBytes) &&
           readDecimalString(object, "tx_bytes", counters.txBytes);
}

bool readRate(const QJsonObject& parent, double& rxBps, double& txBps) {
    const QJsonValue json = parent.value(QLatin1String("rate"));
    if (!json.isObject()) {
        return false;
    }
    const QJsonObject object = json.toObject();
    const QJsonValue rx = object.value(QLatin1String("rx_bps"));
    const QJsonValue tx = object.value(QLatin1String("tx_bps"));
    if (!rx.isDouble() || !tx.isDouble()) {
        return false;
    }
    const double rxValue = rx.toDouble();
    const double txValue = tx.toDouble();
    if (!std::isfinite(rxValue) || !std::isfinite(txValue) || rxValue < 0.0 || txValue < 0.0) {
        return false;
    }
    rxBps = rxValue;
    txBps = txValue;
    return true;
}

// An instant must name its offset: Qt would otherwise read a zoned-less string
// as local time and silently move the frame.
bool parseTimestamp(const QJsonValue& value, QDateTime& out) {
    if (!value.isString()) {
        return false;
    }
    const QString text = value.toString();
    const qsizetype timeStart = text.indexOf(QLatin1Char('T'));
    const bool hasZone = text.endsWith(QLatin1Char('Z')) ||
                         (timeStart >= 0 && (text.indexOf(QLatin1Char('+'), timeStart) >= 0 ||
                                             text.indexOf(QLatin1Char('-'), timeStart) >= 0));
    if (!hasZone) {
        return false;
    }
    const QDateTime parsed = QDateTime::fromString(text, Qt::ISODate);
    if (!parsed.isValid()) {
        return false;
    }
    out = parsed;
    return true;
}

bool parseKind(const QJsonValue& value, InterfaceKind& kind) {
    if (!value.isString()) {
        return false;
    }
    const QString text = value.toString();
    if (text == QLatin1String("physical")) {
        kind = InterfaceKind::Physical;
    } else if (text == QLatin1String("virtual")) {
        kind = InterfaceKind::Virtual;
    } else if (text == QLatin1String("loopback")) {
        kind = InterfaceKind::Loopback;
    } else {
        return false;
    }
    return true;
}

bool parseInterfaces(const QJsonArray& array, QList<InterfaceSnapshot>& out) {
    QSet<quint32> indices;
    for (const QJsonValue entry : array) {
        if (!entry.isObject()) {
            return false;
        }
        const QJsonObject object = entry.toObject();
        quint64 index = 0;
        if (!readJsonCount(object, "index", index) || index == 0 || index > kIntMax) {
            return false;
        }
        const QJsonValue nameValue = object.value(QLatin1String("name"));
        if (!nameValue.isString() || nameValue.toString().isEmpty()) {
            return false;
        }
        InterfaceSnapshot row;
        row.index = static_cast<quint32>(index);
        row.name = nameValue.toString();
        if (!parseKind(object.value(QLatin1String("kind")), row.kind)) {
            return false;
        }
        const QJsonValue upValue = object.value(QLatin1String("up"));
        if (!upValue.isBool()) {
            return false;
        }
        row.up = upValue.toBool();
        if (!readCounters(object, "today", row.today) ||
            !readCounters(object, "session", row.session) ||
            !readRate(object, row.rxBps, row.txBps)) {
            return false;
        }
        if (indices.contains(row.index)) {
            return false;
        }
        indices.insert(row.index);
        out.append(row);
    }
    return true;
}

bool parseProcesses(const QJsonArray& array, QList<ProcessSnapshot>& out) {
    QSet<QPair<int, quint64>> identities;
    for (const QJsonValue entry : array) {
        if (!entry.isObject()) {
            return false;
        }
        const QJsonObject object = entry.toObject();
        quint64 pid = 0;
        if (!readJsonCount(object, "pid", pid) || pid == 0 || pid > kIntMax) {
            return false;
        }
        quint64 startNs = 0;
        if (!readDecimalString(object, "start_ns", startNs)) {
            return false;
        }
        const QJsonValue nameValue = object.value(QLatin1String("name"));
        if (!nameValue.isUndefined() && !nameValue.isString()) {
            return false;
        }
        ProcessSnapshot row;
        row.pid = static_cast<int>(pid);
        row.startNs = startNs;
        row.name = nameValue.toString();
        const QJsonValue presentValue = object.value(QLatin1String("present"));
        if (!presentValue.isBool()) {
            return false;
        }
        row.present = presentValue.toBool();
        if (!readCounters(object, "today", row.today) ||
            !readCounters(object, "session", row.session) ||
            !readRate(object, row.rxBps, row.txBps)) {
            return false;
        }
        const QPair<int, quint64> identity{row.pid, row.startNs};
        if (identities.contains(identity)) {
            return false;
        }
        identities.insert(identity);
        out.append(row);
    }
    return true;
}

bool readBool(const QJsonObject& object, const char* key, bool& value) {
    const QJsonValue json = object.value(QLatin1String(key));
    if (!json.isBool()) {
        return false;
    }
    value = json.toBool();
    return true;
}

bool parseProcessStatus(const QJsonObject& object, ProcessStatus& status) {
    if (!readBool(object, "available", status.available) ||
        !readBool(object, "stale", status.stale) ||
        !readBool(object, "incomplete", status.incomplete)) {
        return false;
    }
    const QJsonValue errorValue = object.value(QLatin1String("error"));
    if (!errorValue.isString()) {
        return false;
    }
    status.error = errorValue.toString();
    const QJsonValue notesValue = object.value(QLatin1String("notes"));
    if (!notesValue.isArray()) {
        return false;
    }
    for (const QJsonValue note : notesValue.toArray()) {
        if (!note.isString()) {
            return false;
        }
        status.notes.append(note.toString());
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
    return QStringLiteral("virtual");
}

SnapshotParseResult parseSnapshot(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reject("bad json");
    }
    const QJsonObject object = document.object();

    quint64 version = 0;
    if (!readJsonCount(object, "version", version)) {
        return reject("bad version");
    }
    if (version != 1) {
        return reject("unsupported version");
    }
    const QJsonValue kindValue = object.value(QLatin1String("kind"));
    if (!kindValue.isString() || kindValue.toString() != QLatin1String("snapshot")) {
        return reject("not a snapshot");
    }

    Snapshot snapshot;
    snapshot.version = static_cast<int>(version);
    if (!parseTimestamp(object.value(QLatin1String("started")), snapshot.started)) {
        return reject("bad started");
    }
    if (!parseTimestamp(object.value(QLatin1String("at")), snapshot.at)) {
        return reject("bad at");
    }

    const QJsonValue availableValue = object.value(QLatin1String("interfaces_available"));
    if (!availableValue.isBool()) {
        return reject("bad interfaces_available");
    }
    snapshot.interfacesAvailable = availableValue.toBool();
    const QJsonValue interfaceErrorValue = object.value(QLatin1String("interface_error"));
    if (!interfaceErrorValue.isString()) {
        return reject("bad interface_error");
    }
    snapshot.interfaceError = interfaceErrorValue.toString();

    const QJsonValue statusValue = object.value(QLatin1String("process_status"));
    if (!statusValue.isObject() ||
        !parseProcessStatus(statusValue.toObject(), snapshot.processStatus)) {
        return reject("bad process_status");
    }

    const QJsonValue interfacesValue = object.value(QLatin1String("interfaces"));
    if (!interfacesValue.isArray() ||
        !parseInterfaces(interfacesValue.toArray(), snapshot.interfaces)) {
        return reject("bad interfaces");
    }
    const QJsonValue processesValue = object.value(QLatin1String("processes"));
    if (!processesValue.isArray() ||
        !parseProcesses(processesValue.toArray(), snapshot.processes)) {
        return reject("bad processes");
    }

    return SnapshotParseResult{std::move(snapshot), QString()};
}

} // namespace ball::ui
