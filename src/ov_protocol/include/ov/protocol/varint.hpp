// VarInt and VarLong, the protocol's variable-length integers.
//
// Nearly every field in the Minecraft protocol is one of these: packet ids,
// lengths, entity ids, block positions in some packets. Getting them wrong does
// not produce a wrong value, it desynchronises the stream — every subsequent
// field is read from the wrong offset and the connection dies somewhere far
// from the cause.
//
// The encoding: seven bits of payload per byte, low bits first, with the high
// bit set on every byte but the last.
//
// Three details that are easy to miss and expensive to find:
//
//   Negative numbers are two's complement, so -1 is five full bytes. An encoder
//   that stops when the value reaches zero emits nothing for a negative number.
//
//   A VarInt is at most 5 bytes and a VarLong at most 10. A stream claiming
//   more is malformed, and reading it is an unbounded loop on attacker-supplied
//   data — vanilla rejects it with "VarInt is too big" and so does this.
//
//   The size of an encoded value has to be computable without encoding it: a
//   length-prefixed packet needs to know its own length before it writes it.
#pragma once

#include "ov/base/types.hpp"
#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"

#include <expected>

namespace ov::net {

enum class VarIntError {
    /// Fewer bytes remain than the value needs.
    UnexpectedEnd,
    /// More continuation bytes than the type allows: 5 for VarInt, 10 for VarLong.
    TooLarge,
};

[[nodiscard]] std::string_view to_string(VarIntError error) noexcept;

template<typename T>
using VarIntResult = std::expected<T, VarIntError>;

inline constexpr u32 kMaxVarIntBytes  = 5;
inline constexpr u32 kMaxVarLongBytes = 10;

/// Bytes this value occupies once encoded, without encoding it.
[[nodiscard]] constexpr u32 varint_size(i32 value) noexcept {
    // Cast through unsigned so that a negative value shifts in zeros rather
    // than sign bits; a negative VarInt is always the full five bytes.
    auto bits = static_cast<u32>(value);
    u32  size = 1;
    while ((bits & ~0x7Fu) != 0) {
        bits >>= 7;
        ++size;
    }
    return size;
}

[[nodiscard]] constexpr u32 varlong_size(i64 value) noexcept {
    auto bits = static_cast<u64>(value);
    u32  size = 1;
    while ((bits & ~static_cast<u64>(0x7F)) != 0) {
        bits >>= 7;
        ++size;
    }
    return size;
}

[[nodiscard]] VarIntResult<i32> read_varint(io::ByteReader& reader) noexcept;
[[nodiscard]] VarIntResult<i64> read_varlong(io::ByteReader& reader) noexcept;

void write_varint(io::ByteWriter& writer, i32 value);
void write_varlong(io::ByteWriter& writer, i64 value);

}  // namespace ov::net
