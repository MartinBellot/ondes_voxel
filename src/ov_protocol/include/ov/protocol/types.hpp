// The protocol's composite data types.
//
// Everything here is defined by the wire format, not chosen. Where a limit
// looks arbitrary it is vanilla's, and matching it matters: a client that
// refuses a 40000-character string will not be rescued by a server that sends
// one.
#pragma once

#include "ov/base/types.hpp"
#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/varint.hpp"

#include <array>
#include <expected>
#include <string>
#include <string_view>

namespace ov::net {

enum class TypeError {
    UnexpectedEnd,
    /// A VarInt prefix was malformed.
    BadVarInt,
    /// A string longer than the protocol allows, or than the caller expects.
    StringTooLong,
    /// String bytes are not valid UTF-8.
    BadUtf8,
};

[[nodiscard]] std::string_view to_string(TypeError error) noexcept;

template<typename T>
using TypeResult = std::expected<T, TypeError>;

/// Vanilla's ceiling for a protocol string, in UTF-16 code units. Individual
/// fields declare smaller limits; a chat message is 256, a resource location
/// 32767 like this.
inline constexpr u32 kMaxStringLength = 32767;

/// A UUID, as it appears on the wire: two big-endian 64-bit halves.
struct Uuid {
    u64 most_significant{0};
    u64 least_significant{0};

    friend constexpr bool operator==(const Uuid&, const Uuid&) noexcept = default;

    /// Canonical 8-4-4-4-12 hyphenated form.
    [[nodiscard]] std::string to_string() const;

    /// Parse the hyphenated form, or the 32-character form without hyphens.
    [[nodiscard]] static std::expected<Uuid, TypeError> parse(std::string_view text);

    // Uuid::offline_player arrives with the authentication work: it is a
    // version 3 UUID over "OfflinePlayer:<name>", which needs MD5.
};

// ── Reading ─────────────────────────────────────────────────────────────────

/// A length-prefixed UTF-8 string. The prefix counts *bytes*, while the
/// declared maximum counts UTF-16 code units, so the byte bound is up to three
/// times the character bound.
[[nodiscard]] TypeResult<std::string> read_string(io::ByteReader& reader,
                                                  u32             max_length = kMaxStringLength);

[[nodiscard]] TypeResult<Uuid> read_uuid(io::ByteReader& reader);

/// A block position packed into one 64-bit value: 26 bits of X, 26 of Z, then
/// 12 of Y, each signed. The Y field being last and only 12 bits wide is a
/// 1.14 change; the older layout put Y in the middle.
[[nodiscard]] TypeResult<i64> read_position_raw(io::ByteReader& reader);

/// A rotation as 1/256 of a full turn.
[[nodiscard]] TypeResult<f32> read_angle(io::ByteReader& reader);

// ── Writing ─────────────────────────────────────────────────────────────────

void write_string(io::ByteWriter& writer, std::string_view text);
void write_uuid(io::ByteWriter& writer, const Uuid& uuid);
void write_angle(io::ByteWriter& writer, f32 degrees);

/// Encoded size of a string, for packets that must know their length up front.
[[nodiscard]] u32 string_size(std::string_view text) noexcept;

}  // namespace ov::net
