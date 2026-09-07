// Java's "modified UTF-8", as used by NBT strings.
//
// NBT strings are written with Java's DataOutput.writeUTF, which is *not* UTF-8
// despite the name. Two differences, both of which produce corrupt data if
// ignored:
//
//   1. U+0000 is encoded as the two bytes C0 80, never as a single 00 byte, so
//      that a string can contain a null without terminating anything.
//   2. Characters above U+FFFF are encoded as a *surrogate pair*, each surrogate
//      written as its own three-byte sequence — six bytes total, where real
//      UTF-8 uses four. This is CESU-8.
//
// For the overwhelming majority of NBT — resource locations, property names,
// player names — the two encodings are byte-identical, which is exactly why
// this is easy to get wrong and hard to notice. It surfaces on emoji in a sign
// or an item name, and then a chunk fails to round-trip.
#pragma once

#include "ov/base/types.hpp"
#include "ov/io/byte_reader.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace ov::io {

/// Decode modified UTF-8 into standard UTF-8.
[[nodiscard]] ReadResult<std::string> decode_modified_utf8(std::span<const u8> bytes);

/// Encode standard UTF-8 as modified UTF-8.
[[nodiscard]] std::string encode_modified_utf8(std::string_view text);

/// Byte length the string will occupy once encoded, without encoding it.
[[nodiscard]] usize modified_utf8_length(std::string_view text) noexcept;

/// True when the two encodings coincide, which is the common case. Lets callers
/// skip the conversion entirely for plain ASCII and most Latin text.
[[nodiscard]] bool is_modified_utf8_identical(std::string_view text) noexcept;

}  // namespace ov::io
