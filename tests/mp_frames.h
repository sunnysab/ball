// ball: a network traffic HUD for KDE Wayland sessions.
// Test-only MessagePack frame writer. Both suites drive real sockets, so the
// fixtures write what a peer writes: complete frames with the protocol's field
// names and integer widths. A malformed case builds its own map through raw(),
// which is the only way to test a decoder's rejections.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <msgpack.hpp>

#include <chrono>
#include <cstring>
#include <functional>
#include <utility>

namespace ball::test {

class Writer {
public:
    using Packer = msgpack::packer<msgpack::sbuffer>;

    Packer& packer() { return m_packer; }
    QByteArray take() { return QByteArray(m_buffer.data(), qsizetype(m_buffer.size())); }

    void key(QLatin1StringView name) {
        m_packer.pack_str(uint32_t(name.size()));
        m_packer.pack_str_body(name.data(), uint32_t(name.size()));
    }
    void text(const QString& value) {
        const QByteArray utf8 = value.toUtf8();
        m_packer.pack_str(uint32_t(utf8.size()));
        m_packer.pack_str_body(utf8.constData(), uint32_t(utf8.size()));
    }
    // The protocol's only encoding for an instant.
    void timestamp(const QDateTime& value) {
        m_packer.pack(std::chrono::system_clock::time_point{
            std::chrono::milliseconds{value.toUTC().toMSecsSinceEpoch()}});
    }

    // A timestamp extension of a different type id, for rejection tests.
    void foreignExt(const char* body, uint32_t size) {
        m_packer.pack_ext(size, 7);
        m_packer.pack_ext_body(body, size);
    }

    // The two collector-run IP totals a stats frame carries, as the two map
    // entries the decoder reads. The caller counts both in its map size.
    void ipCounters(quint64 packets, quint64 bytes) {
        key(QLatin1StringView("ip_packets"));
        m_packer.pack(packets);
        key(QLatin1StringView("ip_bytes"));
        m_packer.pack(bytes);
    }

private:
    msgpack::sbuffer m_buffer;
    Packer m_packer{m_buffer};
};

// One complete frame, written by `fill`.
inline QByteArray frame(const std::function<void(Writer&)>& fill) {
    Writer writer;
    fill(writer);
    return writer.take();
}

// Position of the map entry named `key`, or -1 when `value` is not a map or has
// no such key.
inline int memberIndex(const msgpack::object& value, QLatin1StringView key) {
    if (value.type != msgpack::type::MAP) {
        return -1;
    }
    for (uint32_t i = 0; i < value.via.map.size; ++i) {
        const msgpack::object_kv& entry = value.via.map.ptr[i];
        if (entry.key.type == msgpack::type::STR &&
            entry.key.via.str.size == uint32_t(key.size()) &&
            std::memcmp(entry.key.via.str.ptr, key.data(), key.size()) == 0) {
            return int(i);
        }
    }
    return -1;
}

// The map entry named `key`, or nullptr when `value` is not a map or has no
// such key. Used to break exactly one field of a real frame.
inline msgpack::object* member(msgpack::object& value, QLatin1StringView key) {
    const int index = memberIndex(value, key);
    return index < 0 ? nullptr : &value.via.map.ptr[index].val;
}

// Erases the map entry named `key`, so a test can make an optional field
// genuinely absent: a nil value is a different (and separately refused) case,
// not a missing key. False when there is no such entry.
inline bool removeMember(msgpack::object& value, QLatin1StringView key) {
    const int index = memberIndex(value, key);
    if (index < 0) {
        return false;
    }
    msgpack::object_kv* entries = value.via.map.ptr;
    for (uint32_t i = uint32_t(index) + 1; i < value.via.map.size; ++i) {
        entries[i - 1] = entries[i];
    }
    --value.via.map.size;
    return true;
}

// Re-encodes `frame` after `edit` changed its decoded tree, so a test can make
// one field wrong while every other byte stays exactly as the daemon wrote it.
// Replacement values must live in `zone`, which the caller receives for that.
inline QByteArray patchFrame(const QByteArray& frame,
                             const std::function<void(msgpack::object&, msgpack::zone&)>& edit) {
    std::size_t offset = 0;
    msgpack::object_handle handle =
        msgpack::unpack(frame.constData(), std::size_t(frame.size()), offset);
    msgpack::object root = handle.get();
    msgpack::zone zone;
    edit(root, zone);
    msgpack::sbuffer out;
    msgpack::pack(out, root);
    return QByteArray(out.data(), qsizetype(out.size()));
}

// A frame whose map starts with a "kind" and then the payload under `key`.
inline QByteArray envelope(const QString& kind, QLatin1StringView key,
                           const std::function<void(Writer&)>& fill) {
    return frame([&](Writer& writer) {
        writer.packer().pack_map(2);
        writer.key(QLatin1StringView("kind"));
        writer.text(kind);
        writer.key(key);
        fill(writer);
    });
}

} // namespace ball::test
