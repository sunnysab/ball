// ball: a network traffic HUD for KDE Wayland sessions.
// Strict MessagePack reading, shared by the collector subscriber and the GUI
// snapshot client. MessagePack is an encoding, not a transport: framing stays
// with the caller, and the helpers below only inspect one decoded value.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <msgpack/object_fwd.hpp>

#include <chrono>
#include <memory>

namespace ball::mp {

// MessagePack timestamp extension type from the spec.
inline constexpr qint8 kTimestampExtType = -1;

// Largest single frame either socket accepts, so a broken peer cannot make the
// daemon or the GUI buffer without bound.
inline constexpr quint32 kMaxFrameBytes = 16U * 1024U * 1024U;

// The collector encodes each instant as a MessagePack timestamp, which names an
// instant exactly: this many nanoseconds per second, no leap-second ambiguity.
using Instant = std::chrono::system_clock::time_point;

// One decoded frame and the storage its strings point into. Move-only; every
// pointer obtained from value() dies with the frame.
class Frame {
public:
    Frame() = default;
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&& other) noexcept;
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    // False for "no frame available", which is also the state after a move.
    bool valid() const { return m_value != nullptr; }
    const msgpack::object& value() const { return *m_value; }

private:
    friend class Decoder;
    struct Zone; // owns the decoded values; defined in the implementation
    std::unique_ptr<Zone> m_zone;
    const msgpack::object* m_value = nullptr;
};

// Incremental MessagePack reader: bytes go in, whole frames come out. A frame
// boundary is the end of one self-delimiting object, so no separator is needed
// and a frame split across reads is held until it is complete.
//
// A parse error poisons the stream for good: MessagePack has no resynchronizing
// marker, so the reader refuses to go on rather than decode garbage.
class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Appends bytes received from the socket.
    void feed(const QByteArray& bytes);
    // Returns the next complete frame; an invalid frame means either "incomplete,
    // try again later" or failure(), which error() explains.
    Frame next();
    // Drops every buffered byte and the sticky error, for a reconnect.
    void reset();

    bool failed() const { return m_failed; }
    // Empty while healthy, otherwise a short ASCII reason.
    QByteArray error() const { return m_error; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    QByteArray m_error;
    bool m_failed = false;
};

// Field helpers. Each one checks the MessagePack type before using the value,
// so a peer cannot smuggle a value of the wrong type past a decoder.

// Value of `key` when `value` is a map containing it, otherwise nullptr.
const msgpack::object* member(const msgpack::object& value, QLatin1StringView key);

// Elements of an array. False when `value` is not an array; an empty array is
// true with size 0.
bool arrayEntries(const msgpack::object& value, const msgpack::object*& entries, quint32& size);

// Text of `value` when it is a string with no control characters, so a name or
// an error reason cannot inject terminal escapes into the UI.
QString ascii(const msgpack::object& value, bool& ok);

// A non-negative integer of any width, exactly: a negative, fractional or
// non-finite value is refused so no counter is ever rounded.
bool readCount(const msgpack::object& value, quint64& out);
bool readBool(const msgpack::object& value, bool& out);
// A finite number; NaN and the infinities are refused rather than displayed.
bool readNumber(const msgpack::object& value, double& out);

// Reads a MessagePack timestamp (spec extension type -1) into an instant.
bool readInstant(const msgpack::object& value, Instant& out);

// Local wall-clock form of a timestamp, which is what the accumulators and the
// UI already use. Invalid when the instant is outside Qt's range.
QDateTime timestampToLocalTime(const Instant& instant);

} // namespace ball::mp
