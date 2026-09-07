// Binary NBT: reading and writing.
//
// Layout, for reference while reading the implementation:
//
//   file        := tagType(u8) nameLen(u16) name(modified UTF-8) payload
//   compound    := { tagType nameLen name payload } ... TAG_End(0x00)
//   list        := elemType(u8) length(i32) payload*        (no names)
//   byte_array  := length(i32) u8*
//   int_array   := length(i32) i32*
//   long_array  := length(i32) i64*
//   string      := length(u16) modified UTF-8
//
// Everything is big-endian. Lengths are *signed* 32-bit and a negative length
// is a malformed file, not an empty array — a detail that matters because this
// parser is exposed to region files and to the network.
#pragma once

#include <expected>
#include <span>
#include <string>

#include "ov/base/types.hpp"
#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/nbt/tag.hpp"

namespace ov::nbt {

enum class NbtError {
    UnexpectedEnd,
    /// Tag type byte outside 0..12.
    UnknownTagType,
    /// Negative array or list length.
    NegativeLength,
    /// A length that cannot fit in what remains of the buffer.
    LengthTooLarge,
    /// Nesting deeper than the limit. Guards the parser's own stack against a
    /// hostile file that is nothing but ten thousand nested lists.
    TooDeep,
    /// The root tag is not a compound.
    RootNotCompound,
    /// String bytes are not valid modified UTF-8.
    MalformedString,
    /// A list element's type does not match the list's declared element type.
    HeterogeneousList,
};

[[nodiscard]] std::string_view to_string(NbtError error) noexcept;

template <typename T>
using NbtResult = std::expected<T, NbtError>;

/// A parsed NBT document: the root tag plus its name, which vanilla usually
/// leaves empty but which round-tripping has to preserve.
struct Document {
    std::string name;
    Tag         root;

    [[nodiscard]] bool operator==(const Document&) const noexcept = default;
};

/// Matches vanilla's nesting limit. Anything deeper is a malformed or hostile
/// file rather than legitimate data.
inline constexpr u32 kMaxNestingDepth = 512;

/// Parse uncompressed binary NBT.
[[nodiscard]] NbtResult<Document> read(std::span<const u8> data);

/// Parse from a reader positioned at the start of a document, leaving it just
/// past the end. Used by the network path, where NBT is embedded in a packet.
[[nodiscard]] NbtResult<Document> read(io::ByteReader& reader);

/// Parse a headerless network tag: since 1.20.2 some packets carry a tag with
/// no name. Kept here because the region path and the network path must not
/// drift apart.
[[nodiscard]] NbtResult<Tag> read_unnamed(io::ByteReader& reader);

/// Serialize to uncompressed binary NBT.
[[nodiscard]] std::vector<u8> write(const Document& document);
void write(const Document& document, io::ByteWriter& writer);

/// Serialize a payload with no type byte and no name.
void write_payload(const Tag& tag, io::ByteWriter& writer);

}  // namespace ov::nbt
