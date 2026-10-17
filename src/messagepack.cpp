// ball: a network traffic HUD for KDE Wayland sessions.
#include "messagepack.h"

#include <QTimeZone>

#include <msgpack.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace ball::mp {
namespace {

// Bound on one frame's nesting: the schema is shallow, and the local peers are
// MessagePack writers of maps and arrays, not an untrusted Internet feed.
// shortcut: fixed nesting limit, raise it if a deeper frame is ever specified.
constexpr std::size_t kMaxDepth = 32;
// Container limits are expressed as element counts, so they double as the
// memory bound: a 16 MiB frame cannot claim 2^32 entries and make the library
// allocate for a header that was never sent.
// shortcut: 65536 rows is far above any real process table, and it caps the
// parser's worst-case allocation at a couple of megabytes per frame.
constexpr std::size_t kMaxEntries = 65536;
constexpr std::size_t kMaxStringBytes = kMaxFrameBytes;
// Reasons are shown in the UI and the CLI next to the other short ASCII errors.
constexpr int kMaxErrorLength = 120;

msgpack::unpack_limit limits() {
    return msgpack::unpack_limit(kMaxEntries, kMaxEntries, kMaxStringBytes, kMaxStringBytes,
                                 kMaxStringBytes, kMaxDepth);
}

} // namespace

QString ascii(const msgpack::object& value, bool& ok) {
    ok = value.type == msgpack::type::STR;
    if (!ok) {
        return {};
    }
    const QString text = QString::fromUtf8(value.via.str.ptr, qsizetype(value.via.str.size));
    for (const QChar character : text) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f) {
            ok = false;
            return {};
        }
    }
    return text;
}

bool readCount(const msgpack::object& value, quint64& out) {
    if (value.type == msgpack::type::POSITIVE_INTEGER) {
        out = value.via.u64;
        return true;
    }
    if (value.type != msgpack::type::FLOAT32 && value.type != msgpack::type::FLOAT64) {
        // Covers a negative integer, a string and nil in one step.
        return false;
    }
    // A float64 keeps every integer up to 2^53 and is exactly representable for
    // every counter a local peer produces; a value outside that range or with a
    // fraction is refused rather than rounded.
    const double asFloat = value.via.f64;
    constexpr double kMaxExactFloat = 9007199254740992.0; // 2^53
    if (!std::isfinite(asFloat) || asFloat < 0.0 || asFloat > kMaxExactFloat ||
        std::trunc(asFloat) != asFloat) {
        return false;
    }
    out = static_cast<quint64>(asFloat);
    return true;
}

bool readBool(const msgpack::object& value, bool& out) {
    if (value.type != msgpack::type::BOOLEAN) {
        return false;
    }
    out = value.via.boolean;
    return true;
}

bool readNumber(const msgpack::object& value, double& out) {
    if (value.type == msgpack::type::FLOAT32 || value.type == msgpack::type::FLOAT64) {
        // The library widens a float32 into the f64 member.
        out = value.via.f64;
    } else if (value.type == msgpack::type::POSITIVE_INTEGER) {
        out = double(value.via.u64);
    } else if (value.type == msgpack::type::NEGATIVE_INTEGER) {
        out = double(value.via.i64);
    } else {
        return false;
    }
    return std::isfinite(out);
}

const msgpack::object* member(const msgpack::object& value, QLatin1StringView key) {
    if (value.type != msgpack::type::MAP) {
        return nullptr;
    }
    // Field names are ASCII in this protocol, so comparing the bytes directly
    // avoids allocating a QString per lookup.
    const QByteArray name = key.toUtf8();
    const quint32 size = value.via.map.size;
    const msgpack::object_kv* entry = value.via.map.ptr;
    for (quint32 i = 0; i < size; ++i, ++entry) {
        if (entry->key.type != msgpack::type::STR ||
            entry->key.via.str.size != uint32_t(name.size())) {
            continue;
        }
        if (std::memcmp(entry->key.via.str.ptr, name.constData(), name.size()) == 0) {
            return &entry->val;
        }
    }
    return nullptr;
}

bool arrayEntries(const msgpack::object& value, const msgpack::object*& entries, quint32& size) {
    if (value.type != msgpack::type::ARRAY) {
        return false;
    }
    entries = value.via.array.ptr;
    size = value.via.array.size;
    return true;
}

bool readInstant(const msgpack::object& value, Instant& out) {
    if (value.type != msgpack::type::EXT || value.via.ext.type() != kTimestampExtType) {
        return false;
    }
    // The library reads the ext payload and converts it into a time point; a
    // payload that is not 4, 8 or 12 bytes long is a type error there.
    try {
        out = value.as<Instant>();
    } catch (const msgpack::type_error&) {
        return false;
    }
    return true;
}

QDateTime timestampToLocalTime(const Instant& instant) {
    const auto sinceEpoch = instant.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
    const auto remainder =
        std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - seconds);
    const qint64 msecs =
        qint64(std::chrono::duration_cast<std::chrono::milliseconds>(seconds).count());
    const QDateTime utc =
        QDateTime::fromMSecsSinceEpoch(msecs + qint64(remainder.count() / 1000000), QTimeZone::UTC);
    if (!utc.isValid()) {
        return {};
    }
    return utc.toLocalTime();
}

// ---------------------------------------------------------------------------
// Frame and Decoder

struct Frame::Zone {
    msgpack::object_handle handle;
};

Frame::Frame(Frame&& other) noexcept
    : m_zone(std::move(other.m_zone)), m_value(std::exchange(other.m_value, nullptr)) {}

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        m_zone = std::move(other.m_zone);
        m_value = std::exchange(other.m_value, nullptr);
    }
    return *this;
}

Frame::~Frame() = default;

struct Decoder::Impl {
    msgpack::unpacker unpacker = msgpack::unpacker(nullptr, nullptr, 0, limits());
};

Decoder::Decoder() : m_impl(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;

void Decoder::feed(const QByteArray& bytes) {
    if (m_failed || bytes.isEmpty()) {
        return;
    }
    m_impl->unpacker.reserve_buffer(std::size_t(bytes.size()));
    std::memcpy(m_impl->unpacker.buffer(), bytes.constData(), std::size_t(bytes.size()));
    m_impl->unpacker.buffer_consumed(std::size_t(bytes.size()));

    // The parser's own limits are checked when a container's body is read, so a
    // header claiming a gigabyte would make it hold every byte that follows
    // while it waits. The pending-bytes bound stops that: one frame is capped,
    // and anything past the cap cannot be part of a legal frame.
    if (m_impl->unpacker.nonparsed_size() > kMaxFrameBytes) {
        m_failed = true;
        m_error = QByteArrayLiteral("frame too large");
    }
}

Frame Decoder::next() {
    if (m_failed) {
        return {};
    }
    Frame frame;
    frame.m_zone = std::make_unique<Frame::Zone>();
    try {
        if (!m_impl->unpacker.next(frame.m_zone->handle)) {
            return {}; // nothing complete yet
        }
        frame.m_value = &frame.m_zone->handle.get();
    } catch (const std::exception& error) {
        // The zoned and handle both belong to the failed parse, so the frame is
        // dropped with them; a sticky error stops the stream for good.
        m_failed = true;
        m_error = QByteArray(error.what()).left(kMaxErrorLength);
    }
    return frame;
}

void Decoder::reset() {
    // A new parser is the only way to drop a half-read frame, and a reconnect
    // happens at most once a second, so the allocation is not worth avoiding.
    m_impl = std::make_unique<Impl>();
    m_error.clear();
    m_failed = false;
}

} // namespace ball::mp
