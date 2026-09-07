#include "ov/protocol/varint.hpp"

namespace ov::net {

std::string_view to_string(VarIntError error) noexcept {
    switch (error) {
        case VarIntError::UnexpectedEnd: return "unexpected end of data";
        case VarIntError::TooLarge: return "VarInt is too big";
    }
    return "unknown VarInt error";
}

VarIntResult<i32> read_varint(io::ByteReader& reader) noexcept {
    u32 result = 0;
    u32 shift  = 0;

    for (u32 i = 0; i < kMaxVarIntBytes; ++i) {
        const auto byte = reader.read_u8();
        if (!byte) {
            return std::unexpected{VarIntError::UnexpectedEnd};
        }

        result |= static_cast<u32>(*byte & 0x7F) << shift;
        if ((*byte & 0x80) == 0) {
            return static_cast<i32>(result);
        }
        shift += 7;
    }

    // Five bytes consumed and the continuation bit still set. Vanilla calls
    // this "VarInt is too big"; without the bound this loop runs on whatever
    // the peer keeps sending.
    return std::unexpected{VarIntError::TooLarge};
}

VarIntResult<i64> read_varlong(io::ByteReader& reader) noexcept {
    u64 result = 0;
    u32 shift  = 0;

    for (u32 i = 0; i < kMaxVarLongBytes; ++i) {
        const auto byte = reader.read_u8();
        if (!byte) {
            return std::unexpected{VarIntError::UnexpectedEnd};
        }

        result |= static_cast<u64>(*byte & 0x7F) << shift;
        if ((*byte & 0x80) == 0) {
            return static_cast<i64>(result);
        }
        shift += 7;
    }

    return std::unexpected{VarIntError::TooLarge};
}

void write_varint(io::ByteWriter& writer, i32 value) {
    // Unsigned so the shift brings in zeros: a negative value must produce its
    // full two's complement form, not stop early.
    auto bits = static_cast<u32>(value);
    while (true) {
        if ((bits & ~0x7Fu) == 0) {
            writer.write_u8(static_cast<u8>(bits));
            return;
        }
        writer.write_u8(static_cast<u8>((bits & 0x7F) | 0x80));
        bits >>= 7;
    }
}

void write_varlong(io::ByteWriter& writer, i64 value) {
    auto bits = static_cast<u64>(value);
    while (true) {
        if ((bits & ~static_cast<u64>(0x7F)) == 0) {
            writer.write_u8(static_cast<u8>(bits));
            return;
        }
        writer.write_u8(static_cast<u8>((bits & 0x7F) | 0x80));
        bits >>= 7;
    }
}

}  // namespace ov::net
