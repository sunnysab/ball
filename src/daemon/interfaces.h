// ball: a network traffic HUD for KDE Wayland sessions.
// Network interface inventory and cumulative byte counters, read from rtnetlink.
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace ball {

// How an interface is attached to the machine. The GUI will later pick the
// physical uplink itself; callers must not sum every kind into one number.
enum class InterfaceKind : std::uint8_t {
    Physical,
    Virtual,
    Loopback,
};

struct InterfaceInfo {
    quint32 index = 0; // kernel index, stable across a rename
    QString name;
    InterfaceKind kind = InterfaceKind::Physical;
    bool up = false;
    quint64 rxBytes = 0; // cumulative since the interface came up
    quint64 txBytes = 0;
};

// Decodes one RTM_NEWLINK message starting at its nlmsghdr. Returns nullopt on
// any framing or attribute problem (short or oversized frame, negative index,
// missing or non-NUL name, missing/short counters, trailing attributes); a link
// without usable name and counters is rejected so no fake zero reaches the
// accumulator. IFLA_LINKINFO is kept only as a virtual-device hint.
std::optional<InterfaceInfo> decodeLinkMessage(const void* data, qsizetype length);

// Decides whether a non-loopback interface is virtual. A real device has a
// /sys/class/net/<name>/device symlink; anything else (veth, bridge, vlan,
// wireguard, ...) is virtual. `virtualSysfsRoots` lets tests point at a fake
// tree. Falls back to the IFLA_LINKINFO hint when the sysfs entry is absent.
bool interfaceIsVirtual(const QString& name, bool hasLinkInfo,
                        const QStringList& virtualSysfsRoots = {QStringLiteral("/sys/class/net")});

struct LinkDump {
    QList<InterfaceInfo> interfaces;
    // A valid DONE without a payload was seen.
    bool complete = false;
    // Framing error, unrelated error reply, truncation or NLM_F_DUMP_INTR:
    // the caller must reject the whole dump, not treat it as an inventory.
    bool failed = false;
};

// Drains complete messages from a raw netlink receive buffer and appends their
// interfaces; leftover bytes stay in `buffer` until the rest of the frame
// arrives. Expected sequence numbers only. Any failure sets `failed`.
LinkDump decodeLinkDump(QByteArray& buffer, quint32 sequence);

// Reads the interface list with a non-blocking RTM_GETLINK dump. poll() blocks
// the calling thread for at most timeoutMs, so a daemon should use a small
// timeout or schedule it from a worker thread.
class Interfaces {
public:
    Interfaces() = default;
    Interfaces(const Interfaces&) = delete;
    Interfaces& operator=(const Interfaces&) = delete;
    ~Interfaces();

    // nullopt means the dump was not completed successfully (timeout, error
    // reply, truncation or malformed frame); it must never be read as an empty
    // interface list, which would make a caller drop its baselines.
    std::optional<QList<InterfaceInfo>> poll(int timeoutMs);

private:
    int socketHandle();
    void resetSocket();

    int m_fd = -1;
    quint32 m_sequence = 0;
};

} // namespace ball
