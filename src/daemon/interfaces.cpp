// ball: a network traffic HUD for KDE Wayland sessions.
#include "daemon/interfaces.h"

#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace ball {
namespace {

constexpr int receiveBufferSize = 64 * 1024;
// at most through tx_bytes: older kernels may ship a shorter IFLA_STATS64.
constexpr qsizetype minimumStatsBytes = 32;

quint16 attributeType(const rtattr& attribute) {
    return static_cast<quint16>(attribute.rta_type & NLA_TYPE_MASK);
}

// IFLA_IFNAME must be a NUL-terminated string; anything else is rejected so a
// truncated frame cannot smuggle a partial name in.
std::optional<QString> attributeString(const char* payload, qsizetype length) {
    for (qsizetype i = 0; i < length; ++i) {
        if (payload[i] == '\0') {
            return i == 0 ? std::nullopt : std::optional(QString::fromUtf8(payload, i));
        }
    }
    return std::nullopt;
}

// Reads down to tx_bytes. Older kernels may ship a shorter IFLA_STATS64, so
// only the fields actually used are required.
std::optional<std::pair<quint64, quint64>> attributeCounters(const char* payload,
                                                             qsizetype length) {
    if (length < minimumStatsBytes) {
        return std::nullopt;
    }
    quint64 rxBytes = 0;
    quint64 txBytes = 0;
    std::memcpy(&rxBytes, payload + 16, sizeof(rxBytes)); // offsetof(rtnl_link_stats64, rx_bytes)
    std::memcpy(&txBytes, payload + 24, sizeof(txBytes)); // offsetof(rtnl_link_stats64, tx_bytes)
    return std::pair(rxBytes, txBytes);
}

} // namespace

std::optional<InterfaceInfo> decodeLinkMessage(const void* data, qsizetype length) {
    if (data == nullptr || length < static_cast<qsizetype>(NLMSG_HDRLEN)) {
        return std::nullopt;
    }
    const auto* base = static_cast<const char*>(data);

    nlmsghdr header{};
    std::memcpy(&header, base, sizeof(header));
    const qsizetype messageLength = static_cast<qsizetype>(header.nlmsg_len);
    const qsizetype linkLength = static_cast<qsizetype>(NLMSG_LENGTH(sizeof(ifinfomsg)));
    if (header.nlmsg_type != RTM_NEWLINK || messageLength < linkLength || messageLength > length) {
        return std::nullopt;
    }

    ifinfomsg info{};
    std::memcpy(&info, base + NLMSG_HDRLEN, sizeof(info));
    if (info.ifi_index <= 0) {
        return std::nullopt; // an interface always has a positive kernel index
    }

    InterfaceInfo result;
    result.index = static_cast<quint32>(info.ifi_index);
    result.up = (info.ifi_flags & IFF_UP) != 0;

    bool hasName = false;
    bool hasCounters = false;
    bool hasLinkInfo = false;
    const qsizetype attributeOffset = NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(ifinfomsg));
    qsizetype attributeLength = messageLength - attributeOffset;
    if (attributeLength < 0) {
        return std::nullopt;
    }
    qsizetype attributeOffsetCurrent = attributeOffset;
    while (attributeLength >= static_cast<qsizetype>(RTA_LENGTH(0))) {
        rtattr attribute{};
        std::memcpy(&attribute, base + attributeOffsetCurrent, sizeof(attribute));
        const qsizetype attributeSize = static_cast<qsizetype>(attribute.rta_len);
        if (attributeSize < static_cast<qsizetype>(RTA_LENGTH(0)) ||
            attributeSize > attributeLength) {
            return std::nullopt; // a malformed tail must reject the whole link
        }
        const char* payload = base + attributeOffsetCurrent + RTA_LENGTH(0);
        const qsizetype payloadLength = attributeSize - static_cast<qsizetype>(RTA_LENGTH(0));
        switch (attributeType(attribute)) {
        case IFLA_IFNAME:
            if (const auto name = attributeString(payload, payloadLength)) {
                result.name = *name;
                hasName = true;
            }
            break;
        case IFLA_STATS64:
            if (const auto counters = attributeCounters(payload, payloadLength)) {
                result.rxBytes = counters->first;
                result.txBytes = counters->second;
                hasCounters = true;
            }
            break;
        case IFLA_LINKINFO:
            hasLinkInfo = true;
            break;
        default:
            break;
        }
        attributeOffsetCurrent += static_cast<qsizetype>(RTA_ALIGN(attributeSize));
        attributeLength -= static_cast<qsizetype>(RTA_ALIGN(attributeSize));
    }
    if (attributeLength != 0) {
        return std::nullopt; // trailing bytes that are not a whole attribute
    }
    if (!hasName || !hasCounters) {
        return std::nullopt;
    }

    if ((info.ifi_flags & IFF_LOOPBACK) != 0) {
        result.kind = InterfaceKind::Loopback;
    } else {
        result.kind = interfaceIsVirtual(result.name, hasLinkInfo) ? InterfaceKind::Virtual
                                                                   : InterfaceKind::Physical;
    }
    return result;
}

bool interfaceIsVirtual(const QString& name, bool hasLinkInfo,
                        const QStringList& virtualSysfsRoots) {
    if (name.isEmpty() || name.contains(QLatin1Char('/'))) {
        return true; // not a name that can name a sysfs device
    }
    for (const QString& root : virtualSysfsRoots) {
        const QDir netRoot(root);
        if (!netRoot.exists()) {
            continue; // no sysfs tree here, fall back to the linkinfo hint
        }
        // A real device has a `device` symlink next to the netdev entry; veth,
        // bridge, vlan and wireguard do not.
        return !QFileInfo(netRoot.filePath(name + QStringLiteral("/device"))).exists();
    }
    return hasLinkInfo;
}

LinkDump decodeLinkDump(QByteArray& buffer, quint32 sequence) {
    LinkDump result;
    qsizetype offset = 0;
    for (;;) {
        const qsizetype available = buffer.size() - offset;
        if (available < static_cast<qsizetype>(NLMSG_HDRLEN)) {
            break;
        }
        nlmsghdr header{};
        std::memcpy(&header, buffer.constData() + offset, sizeof(header));
        const qsizetype length = static_cast<qsizetype>(header.nlmsg_len);
        if (length < static_cast<qsizetype>(NLMSG_HDRLEN)) {
            buffer.clear();
            result.failed = true;
            return result;
        }
        if (available < length) {
            break; // the rest of this frame is still in flight
        }
        if (header.nlmsg_seq != sequence) {
            offset += NLMSG_ALIGN(length);
            continue;
        }
        // NLMSG_DONE ends the current dump, with an optional status payload.
        const char* frame = buffer.constData() + offset;
        if (header.nlmsg_type == NLMSG_DONE) {
            const qsizetype donePayload = length - static_cast<qsizetype>(NLMSG_HDRLEN);
            offset += NLMSG_ALIGN(length);
            bool statusOk =
                header.nlmsg_seq == sequence && (header.nlmsg_flags & NLM_F_DUMP_INTR) == 0;
            if (statusOk && donePayload != 0) {
                if (donePayload < static_cast<qsizetype>(sizeof(qint32))) {
                    statusOk = false;
                } else {
                    qint32 status = 0;
                    std::memcpy(&status, frame + NLMSG_HDRLEN, sizeof(status));
                    statusOk = status == 0;
                }
            }
            if (statusOk) {
                result.complete = true;
            } else {
                result.failed = true;
            }
            break;
        }
        // The kernel-visible list changed mid-dump; the result is not a coherent
        // inventory and must not replace the caller's baselines.
        if ((header.nlmsg_flags & NLM_F_DUMP_INTR) != 0) {
            result.failed = true;
            break;
        }
        if (header.nlmsg_type == NLMSG_ERROR) {
            if (length < static_cast<qsizetype>(NLMSG_LENGTH(sizeof(nlmsgerr)))) {
                result.failed = true;
                break;
            }
            nlmsgerr error{};
            std::memcpy(&error, buffer.constData() + offset + NLMSG_HDRLEN, sizeof(error));
            if (error.error != 0) {
                result.failed = true;
                break;
            }
        } else if (header.nlmsg_type == RTM_NEWLINK) {
            if (const auto info = decodeLinkMessage(buffer.constData() + offset, length)) {
                result.interfaces.append(*info);
            } else {
                // A malformed link must reject the whole dump; silently dropping
                // it would look like the interface disappeared.
                result.failed = true;
                break;
            }
        }
        offset += NLMSG_ALIGN(length);
    }
    if (result.failed) {
        buffer.clear(); // the frame cannot be parsed, drop it instead of retrying
    } else {
        buffer.remove(0, offset);
    }
    return result;
}

Interfaces::~Interfaces() { resetSocket(); }

int Interfaces::socketHandle() {
    if (m_fd < 0) {
        m_fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
        if (m_fd < 0) {
            qWarning("ball: cannot open a NETLINK_ROUTE socket: %s", std::strerror(errno));
        }
    }
    return m_fd;
}

void Interfaces::resetSocket() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

std::optional<QList<InterfaceInfo>> Interfaces::poll(int timeoutMs) {
    const int fd = socketHandle();
    if (fd < 0) {
        return std::nullopt;
    }
    if (timeoutMs < 0) {
        timeoutMs = 0;
    }
    const quint32 sequence = ++m_sequence;

    struct {
        nlmsghdr header;
        ifinfomsg info;
    } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
    request.header.nlmsg_type = RTM_GETLINK;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = sequence;
    request.info.ifi_family = AF_UNSPEC;

    if (::send(fd, &request, request.header.nlmsg_len, 0) < 0) {
        qWarning("ball: rtnetlink RTM_GETLINK send failed: %s", std::strerror(errno));
        resetSocket();
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    QByteArray buffer;
    QList<InterfaceInfo> interfaces;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            resetSocket();
            return std::nullopt;
        }
        const auto remainingMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, static_cast<int>(remainingMs));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            qWarning("ball: rtnetlink poll failed: %s", std::strerror(errno));
            resetSocket();
            return std::nullopt;
        }
        if (ready == 0) {
            resetSocket();
            return std::nullopt;
        }
        if ((static_cast<unsigned>(descriptor.revents) &
             static_cast<unsigned>(POLLERR | POLLHUP)) != 0) {
            qWarning("ball: rtnetlink socket reported POLLERR/POLLHUP");
            resetSocket();
            return std::nullopt;
        }

        char received[receiveBufferSize];
        iovec chunk{};
        chunk.iov_base = received;
        chunk.iov_len = sizeof(received);
        sockaddr_nl sender{};
        msghdr message{};
        message.msg_name = &sender;
        message.msg_namelen = sizeof(sender);
        message.msg_iov = &chunk;
        message.msg_iovlen = 1;
        const ssize_t bytes = ::recvmsg(fd, &message, 0);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            // ENOBUFS means frames were dropped: the dump is incomplete.
            qWarning("ball: rtnetlink recvmsg failed: %s", std::strerror(errno));
            resetSocket();
            return std::nullopt;
        }
        if ((static_cast<unsigned>(message.msg_flags) & static_cast<unsigned>(MSG_TRUNC)) != 0) {
            qWarning("ball: rtnetlink frame truncated, rejecting the dump");
            resetSocket();
            return std::nullopt;
        }
        if (sender.nl_pid != 0) {
            qWarning("ball: rtnetlink reply from pid %u, rejecting the dump", sender.nl_pid);
            resetSocket();
            return std::nullopt;
        }
        if (bytes == 0) {
            resetSocket();
            return std::nullopt;
        }
        buffer.append(received, static_cast<qsizetype>(bytes));

        const LinkDump dump = decodeLinkDump(buffer, sequence);
        if (dump.failed) {
            resetSocket();
            return std::nullopt;
        }
        interfaces += dump.interfaces; // frames arrive over several reads
        if (dump.complete) {
            return interfaces;
        }
    }
}

} // namespace ball
