#include "ov/protocol/types.hpp"

#include "ov/base/md5.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace ov::net {
namespace {

/// Validate UTF-8 well-formedness.
///
/// A client sends strings before it is authenticated. Passing invalid UTF-8
/// through means it reaches a JSON writer, a log line, or a filename later,
/// somewhere much harder to reason about than here.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept {
    usize i = 0;
    while (i < text.size()) {
        const auto byte       = static_cast<u8>(text[i]);
        usize      extra      = 0;
        u32        code_point = 0;

        if (byte < 0x80) {
            i += 1;
            continue;
        } else if ((byte & 0xE0) == 0xC0) {
            extra      = 1;
            code_point = byte & 0x1Fu;
        } else if ((byte & 0xF0) == 0xE0) {
            extra      = 2;
            code_point = byte & 0x0Fu;
        } else if ((byte & 0xF8) == 0xF0) {
            extra      = 3;
            code_point = byte & 0x07u;
        } else {
            return false;  // continuation byte in a leading position, or 0xF8+
        }

        if (i + extra >= text.size()) {
            return false;
        }
        for (usize k = 1; k <= extra; ++k) {
            const auto continuation = static_cast<u8>(text[i + k]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (continuation & 0x3Fu);
        }

        // Reject overlong forms, surrogates, and anything past U+10FFFF: all
        // three are how a decoder is tricked into disagreeing with an encoder.
        static constexpr u32 kMinimum[] = {0, 0x80, 0x800, 0x10000};
        if (code_point < kMinimum[extra]) {
            return false;
        }
        if (code_point > 0x10FFFF) {
            return false;
        }
        if (code_point >= 0xD800 && code_point <= 0xDFFF) {
            return false;
        }

        i += extra + 1;
    }
    return true;
}

}  // namespace

std::string_view to_string(TypeError error) noexcept {
    switch (error) {
        case TypeError::UnexpectedEnd: return "unexpected end of data";
        case TypeError::BadVarInt: return "malformed VarInt prefix";
        case TypeError::StringTooLong: return "string exceeds its limit";
        case TypeError::BadUtf8: return "string is not valid UTF-8";
    }
    return "unknown type error";
}

std::string Uuid::to_string() const {
    char buffer[37];
    std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(most_significant >> 32),
                  static_cast<unsigned>((most_significant >> 16) & 0xFFFF),
                  static_cast<unsigned>(most_significant & 0xFFFF),
                  static_cast<unsigned>(least_significant >> 48),
                  static_cast<unsigned long long>(least_significant & 0xFFFFFFFFFFFFULL));
    return std::string{buffer};
}

std::expected<Uuid, TypeError> Uuid::parse(std::string_view text) {
    // Both forms are accepted: the hyphenated one appears in JSON, the bare
    // 32-character one in some launcher files.
    std::string hex;
    hex.reserve(32);
    for (const char c : text) {
        if (c == '-') {
            continue;
        }
        hex.push_back(c);
    }
    if (hex.size() != 32) {
        return std::unexpected{TypeError::StringTooLong};
    }

    auto parse_half = [](std::string_view part) -> std::expected<u64, TypeError> {
        u64        value  = 0;
        const auto result = std::from_chars(part.data(), part.data() + part.size(), value, 16);
        if (result.ec != std::errc{} || result.ptr != part.data() + part.size()) {
            return std::unexpected{TypeError::BadUtf8};
        }
        return value;
    };

    const auto high = parse_half(std::string_view{hex}.substr(0, 16));
    const auto low  = parse_half(std::string_view{hex}.substr(16, 16));
    if (!high || !low) {
        return std::unexpected{TypeError::BadUtf8};
    }
    return Uuid{*high, *low};
}

Uuid Uuid::offline_player(std::string_view name) {
    std::string source{"OfflinePlayer:"};
    source += name;

    auto digest = md5(source);

    // Version 3 and the RFC 4122 variant, set in place over the digest. Java's
    // UUID.nameUUIDFromBytes does exactly this, and skipping it produces an id
    // that looks plausible and matches nothing.
    digest[6] = static_cast<u8>((digest[6] & 0x0F) | 0x30);
    digest[8] = static_cast<u8>((digest[8] & 0x3F) | 0x80);

    u64 high = 0;
    u64 low  = 0;
    for (u32 i = 0; i < 8; ++i) {
        high = (high << 8) | digest[i];
        low  = (low << 8) | digest[i + 8];
    }
    return Uuid{high, low};
}

TypeResult<std::string> read_string(io::ByteReader& reader, u32 max_length) {
    const auto length = read_varint(reader);
    if (!length) {
        return std::unexpected{length.error() == VarIntError::UnexpectedEnd
                                   ? TypeError::UnexpectedEnd
                                   : TypeError::BadVarInt};
    }
    if (*length < 0) {
        return std::unexpected{TypeError::BadVarInt};
    }

    // The prefix counts bytes; the declared maximum counts UTF-16 code units,
    // and one of those is at most three UTF-8 bytes. Checking the byte count
    // against the byte bound is what stops a length from becoming an
    // allocation before anything is validated.
    const auto byte_limit = static_cast<usize>(max_length) * 3;
    const auto byte_count = static_cast<usize>(*length);
    if (byte_count > byte_limit || byte_count > reader.remaining()) {
        return std::unexpected{byte_count > byte_limit ? TypeError::StringTooLong
                                                       : TypeError::UnexpectedEnd};
    }

    const auto bytes = reader.read_bytes(byte_count);
    if (!bytes) {
        return std::unexpected{TypeError::UnexpectedEnd};
    }

    std::string text{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    if (!is_valid_utf8(text)) {
        return std::unexpected{TypeError::BadUtf8};
    }
    return text;
}

TypeResult<Uuid> read_uuid(io::ByteReader& reader) {
    const auto high = reader.read_u64();
    const auto low  = reader.read_u64();
    if (!high || !low) {
        return std::unexpected{TypeError::UnexpectedEnd};
    }
    return Uuid{*high, *low};
}

TypeResult<i64> read_position_raw(io::ByteReader& reader) {
    const auto packed = reader.read_i64();
    if (!packed) {
        return std::unexpected{TypeError::UnexpectedEnd};
    }
    return *packed;
}

TypeResult<f32> read_angle(io::ByteReader& reader) {
    const auto raw = reader.read_u8();
    if (!raw) {
        return std::unexpected{TypeError::UnexpectedEnd};
    }
    return static_cast<f32>(*raw) * (360.0f / 256.0f);
}

void write_string(io::ByteWriter& writer, std::string_view text) {
    write_varint(writer, static_cast<i32>(text.size()));
    writer.write_bytes(text);
}

void write_uuid(io::ByteWriter& writer, const Uuid& uuid) {
    writer.write_u64(uuid.most_significant);
    writer.write_u64(uuid.least_significant);
}

void write_angle(io::ByteWriter& writer, f32 degrees) {
    // Wrap into [0, 360) before scaling, so a rotation of 720 does not become
    // an out-of-range byte.
    const f32 wrapped = degrees - 360.0f * std::floor(degrees / 360.0f);
    writer.write_u8(static_cast<u8>(wrapped * (256.0f / 360.0f)));
}

u32 string_size(std::string_view text) noexcept {
    return varint_size(static_cast<i32>(text.size())) + static_cast<u32>(text.size());
}

}  // namespace ov::net
