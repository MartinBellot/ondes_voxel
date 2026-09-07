#include "ov/nbt/binary.hpp"

#include "ov/base/platform.hpp"
#include "ov/io/modified_utf8.hpp"

#include <algorithm>
#include <vector>

namespace ov::nbt {
namespace {

using io::ByteReader;
using io::ByteWriter;

/// Translate an io-level read failure into an NBT-level one.
constexpr NbtError from_io(io::ReadError error) noexcept {
    switch (error) {
        case io::ReadError::OutOfBounds: return NbtError::UnexpectedEnd;
        case io::ReadError::InvalidLength: return NbtError::LengthTooLarge;
        case io::ReadError::MalformedEncoding: return NbtError::MalformedString;
    }
    return NbtError::UnexpectedEnd;
}

/// Read a length prefix, rejecting negatives and anything that cannot fit.
///
/// `element_size` lets the bound be checked before allocating: a hostile file
/// declaring an array of two billion longs must fail here, not in the
/// allocator. This is the single most important check in the parser.
NbtResult<usize> read_length(ByteReader& reader, usize element_size) {
    const auto raw = reader.read_i32();
    if (!raw) {
        return std::unexpected{from_io(raw.error())};
    }
    if (*raw < 0) {
        return std::unexpected{NbtError::NegativeLength};
    }
    const auto count = static_cast<usize>(*raw);
    if (element_size != 0 && count > reader.remaining() / element_size) {
        return std::unexpected{NbtError::LengthTooLarge};
    }
    return count;
}

NbtResult<std::string> read_string(ByteReader& reader) {
    const auto length = reader.read_u16();
    if (!length) {
        return std::unexpected{from_io(length.error())};
    }
    const auto bytes = reader.read_bytes(*length);
    if (!bytes) {
        return std::unexpected{from_io(bytes.error())};
    }
    auto decoded = io::decode_modified_utf8(*bytes);
    if (!decoded) {
        return std::unexpected{NbtError::MalformedString};
    }
    return std::move(*decoded);
}

/// Read a payload that cannot itself contain other tags.
///
/// Split out from the nesting machinery below so that the loop only ever has to
/// deal with lists and compounds.
NbtResult<Tag> read_scalar_payload(ByteReader& reader, TagType type) {
    switch (type) {
        case TagType::End: return Tag{};

        case TagType::Byte:
            return reader.read_i8().transform([](i8 v) { return Tag{v}; }).transform_error(from_io);
        case TagType::Short:
            return reader.read_i16()
                .transform([](i16 v) { return Tag{v}; })
                .transform_error(from_io);
        case TagType::Int:
            return reader.read_i32()
                .transform([](i32 v) { return Tag{v}; })
                .transform_error(from_io);
        case TagType::Long:
            return reader.read_i64()
                .transform([](i64 v) { return Tag{v}; })
                .transform_error(from_io);
        case TagType::Float:
            return reader.read_f32()
                .transform([](f32 v) { return Tag{v}; })
                .transform_error(from_io);
        case TagType::Double:
            return reader.read_f64()
                .transform([](f64 v) { return Tag{v}; })
                .transform_error(from_io);

        case TagType::ByteArray: {
            const auto count = read_length(reader, 1);
            if (!count)
                return std::unexpected{count.error()};
            const auto bytes = reader.read_bytes(*count);
            if (!bytes)
                return std::unexpected{from_io(bytes.error())};
            return Tag{Tag::ByteArray{bytes->begin(), bytes->end()}};
        }

        case TagType::String: {
            auto text = read_string(reader);
            if (!text)
                return std::unexpected{text.error()};
            return Tag{std::move(*text)};
        }

        case TagType::IntArray: {
            const auto count = read_length(reader, 4);
            if (!count)
                return std::unexpected{count.error()};
            Tag::IntArray values;
            values.reserve(*count);
            for (usize i = 0; i < *count; ++i) {
                const auto value = reader.read_i32();
                if (!value)
                    return std::unexpected{from_io(value.error())};
                values.push_back(*value);
            }
            return Tag{std::move(values)};
        }

        case TagType::LongArray: {
            const auto count = read_length(reader, 8);
            if (!count)
                return std::unexpected{count.error()};
            Tag::LongArray values;
            values.reserve(*count);
            for (usize i = 0; i < *count; ++i) {
                const auto value = reader.read_i64();
                if (!value)
                    return std::unexpected{from_io(value.error())};
                values.push_back(*value);
            }
            return Tag{std::move(values)};
        }

        case TagType::List:
        case TagType::Compound:
            // Handled by the loop; reaching here would be a programming error.
            return std::unexpected{NbtError::UnknownTagType};
    }
    return std::unexpected{NbtError::UnknownTagType};
}

/// Read the element type and count of a list, with the bounds checks.
struct ListHeader {
    TagType element_type{TagType::End};
    usize   count{0};
};

NbtResult<ListHeader> read_list_header(ByteReader& reader) {
    const auto raw_type = reader.read_u8();
    if (!raw_type) {
        return std::unexpected{from_io(raw_type.error())};
    }
    if (!is_valid_tag_type(*raw_type)) {
        return std::unexpected{NbtError::UnknownTagType};
    }
    const auto element_type = static_cast<TagType>(*raw_type);

    // Element size is known for the fixed-width types, which lets a hostile
    // count be rejected before anything is reserved.
    const usize element_hint = [&]() -> usize {
        switch (element_type) {
            case TagType::Byte: return 1;
            case TagType::Short: return 2;
            case TagType::Int:
            case TagType::Float: return 4;
            case TagType::Long:
            case TagType::Double: return 8;
            default: return 0;
        }
    }();

    const auto count = read_length(reader, element_hint);
    if (!count) {
        return std::unexpected{count.error()};
    }

    // TAG_End has no payload, so a list claiming to hold some is lying about
    // its own size.
    if (element_type == TagType::End && *count > 0) {
        return std::unexpected{NbtError::UnknownTagType};
    }
    return ListHeader{element_type, *count};
}

/// One level of nesting, held on an explicit stack.
struct Frame {
    Tag         tag;   // the list or compound being filled
    std::string name;  // what it will be called in its parent
    bool        in_list{false};
    TagType     element_type{TagType::End};
    usize       remaining{0};  // list elements still to read
};

/// Parse a list or compound payload without recursing.
///
/// This used to recurse, mirroring the file's own nesting, and a file 508 levels
/// deep overflowed the 1 MB stack Windows gives a thread — the exact crash the
/// depth limit exists to prevent, just at a depth the limit still allowed.
/// Lowering the limit would have meant rejecting files vanilla accepts, so the
/// nesting lives on the heap instead and the limit stays at vanilla's 512.
NbtResult<Tag> read_container(ByteReader& reader, TagType type) {
    std::vector<Frame> stack;
    stack.reserve(16);

    auto push = [&](TagType container, std::string name) -> NbtResult<void> {
        if (stack.size() >= kMaxNestingDepth) {
            return std::unexpected{NbtError::TooDeep};
        }
        if (container == TagType::Compound) {
            stack.push_back(Frame{Tag::make_compound(), std::move(name), false, TagType::End, 0});
            return {};
        }
        auto header = read_list_header(reader);
        if (!header) {
            return std::unexpected{header.error()};
        }
        stack.push_back(Frame{Tag::make_list(header->element_type), std::move(name), true,
                              header->element_type, header->count});
        return {};
    };

    if (const auto pushed = push(type, {}); !pushed) {
        return std::unexpected{pushed.error()};
    }

    while (true) {
        Frame& frame    = stack.back();
        bool   finished = false;

        if (frame.in_list) {
            if (frame.remaining == 0) {
                finished = true;
            } else {
                --frame.remaining;
                const TagType element = frame.element_type;
                if (element == TagType::List || element == TagType::Compound) {
                    if (const auto pushed = push(element, {}); !pushed) {
                        return std::unexpected{pushed.error()};
                    }
                    continue;
                }
                auto value = read_scalar_payload(reader, element);
                if (!value) {
                    return std::unexpected{value.error()};
                }
                frame.tag.list()->push_back(std::move(*value));
            }
        } else {
            const auto raw_type = reader.read_u8();
            if (!raw_type) {
                return std::unexpected{from_io(raw_type.error())};
            }
            if (!is_valid_tag_type(*raw_type)) {
                return std::unexpected{NbtError::UnknownTagType};
            }
            const auto entry_type = static_cast<TagType>(*raw_type);

            if (entry_type == TagType::End) {
                finished = true;
            } else {
                auto name = read_string(reader);
                if (!name) {
                    return std::unexpected{name.error()};
                }
                if (entry_type == TagType::List || entry_type == TagType::Compound) {
                    if (const auto pushed = push(entry_type, std::move(*name)); !pushed) {
                        return std::unexpected{pushed.error()};
                    }
                    continue;
                }
                auto value = read_scalar_payload(reader, entry_type);
                if (!value) {
                    return std::unexpected{value.error()};
                }
                frame.tag.compound()->push_back(CompoundEntry{std::move(*name), std::move(*value)});
            }
        }

        if (!finished) {
            continue;
        }

        // This level is complete: hand it to its parent, or return it if it is
        // the root.
        Frame done = std::move(stack.back());
        stack.pop_back();
        if (stack.empty()) {
            return std::move(done.tag);
        }

        Frame& parent = stack.back();
        if (parent.in_list) {
            parent.tag.list()->push_back(std::move(done.tag));
        } else {
            parent.tag.compound()->push_back(
                CompoundEntry{std::move(done.name), std::move(done.tag)});
        }
    }
}

NbtResult<Tag> read_payload(ByteReader& reader, TagType type, u32 depth) {
    OV_UNUSED(depth);
    if (type == TagType::List || type == TagType::Compound) {
        return read_container(reader, type);
    }
    return read_scalar_payload(reader, type);
}

void write_string(ByteWriter& writer, std::string_view text) {
    // Length first, so it has to agree with what the encoder produces. The
    // helper computes it without encoding rather than encoding twice.
    if (io::is_modified_utf8_identical(text)) {
        writer.write_u16(static_cast<u16>(text.size()));
        writer.write_bytes(text);
    } else {
        const std::string encoded = io::encode_modified_utf8(text);
        writer.write_u16(static_cast<u16>(encoded.size()));
        writer.write_bytes(encoded);
    }
}

}  // namespace

std::string_view to_string(NbtError error) noexcept {
    switch (error) {
        case NbtError::UnexpectedEnd: return "unexpected end of data";
        case NbtError::UnknownTagType: return "unknown tag type";
        case NbtError::NegativeLength: return "negative length";
        case NbtError::LengthTooLarge: return "length exceeds available data";
        case NbtError::TooDeep: return "nesting too deep";
        case NbtError::RootNotCompound: return "root tag is not a compound";
        case NbtError::MalformedString: return "malformed modified UTF-8 string";
        case NbtError::HeterogeneousList: return "list elements have mixed types";
    }
    return "unknown NBT error";
}

NbtResult<Document> read(ByteReader& reader) {
    const auto raw_type = reader.read_u8();
    if (!raw_type) {
        return std::unexpected{from_io(raw_type.error())};
    }
    if (!is_valid_tag_type(*raw_type)) {
        return std::unexpected{NbtError::UnknownTagType};
    }
    const auto type = static_cast<TagType>(*raw_type);

    // A file whose first byte is TAG_End is an empty document, which vanilla
    // does write in some places.
    if (type == TagType::End) {
        return Document{"", Tag::make_compound()};
    }
    if (type != TagType::Compound) {
        return std::unexpected{NbtError::RootNotCompound};
    }

    auto name = read_string(reader);
    if (!name) {
        return std::unexpected{name.error()};
    }
    auto root = read_payload(reader, type, 0);
    if (!root) {
        return std::unexpected{root.error()};
    }
    return Document{std::move(*name), std::move(*root)};
}

NbtResult<Document> read(std::span<const u8> data) {
    ByteReader reader{data};
    return read(reader);
}

NbtResult<Tag> read_unnamed(ByteReader& reader) {
    const auto raw_type = reader.read_u8();
    if (!raw_type) {
        return std::unexpected{from_io(raw_type.error())};
    }
    if (!is_valid_tag_type(*raw_type)) {
        return std::unexpected{NbtError::UnknownTagType};
    }
    return read_payload(reader, static_cast<TagType>(*raw_type), 0);
}

void write_payload(const Tag& tag, ByteWriter& writer) {
    switch (tag.type()) {
        case TagType::End: break;
        case TagType::Byte: writer.write_i8(*tag.get_if<i8>()); break;
        case TagType::Short: writer.write_i16(*tag.get_if<i16>()); break;
        case TagType::Int: writer.write_i32(*tag.get_if<i32>()); break;
        case TagType::Long: writer.write_i64(*tag.get_if<i64>()); break;
        case TagType::Float: writer.write_f32(*tag.get_if<f32>()); break;
        case TagType::Double: writer.write_f64(*tag.get_if<f64>()); break;

        case TagType::ByteArray: {
            const auto& bytes = *tag.get_if<Tag::ByteArray>();
            writer.write_i32(static_cast<i32>(bytes.size()));
            writer.write_bytes(std::span<const u8>{bytes});
            break;
        }

        case TagType::String: write_string(writer, *tag.get_if<std::string>()); break;

        case TagType::List: {
            const auto& items = *tag.list();
            writer.write_u8(static_cast<u8>(tag.list_element_type()));
            writer.write_i32(static_cast<i32>(items.size()));
            for (const auto& item : items) {
                write_payload(item, writer);
            }
            break;
        }

        case TagType::Compound: {
            for (const auto& entry : *tag.compound()) {
                writer.write_u8(static_cast<u8>(entry.value.type()));
                write_string(writer, entry.name);
                write_payload(entry.value, writer);
            }
            writer.write_u8(static_cast<u8>(TagType::End));
            break;
        }

        case TagType::IntArray: {
            const auto& values = *tag.get_if<Tag::IntArray>();
            writer.write_i32(static_cast<i32>(values.size()));
            for (const i32 value : values) {
                writer.write_i32(value);
            }
            break;
        }

        case TagType::LongArray: {
            const auto& values = *tag.get_if<Tag::LongArray>();
            writer.write_i32(static_cast<i32>(values.size()));
            for (const i64 value : values) {
                writer.write_i64(value);
            }
            break;
        }
    }
}

void write(const Document& document, ByteWriter& writer) {
    writer.write_u8(static_cast<u8>(document.root.type()));
    write_string(writer, document.name);
    write_payload(document.root, writer);
}

std::vector<u8> write(const Document& document) {
    ByteWriter writer{4096};
    write(document, writer);
    return writer.take();
}

}  // namespace ov::nbt
