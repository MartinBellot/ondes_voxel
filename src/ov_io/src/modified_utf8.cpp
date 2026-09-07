#include "ov/io/modified_utf8.hpp"

namespace ov::io {
namespace {

constexpr u32 kReplacementChar = 0xFFFD;

/// Append a code point as standard UTF-8.
void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// Append a code point as modified UTF-8: null becomes C0 80, and anything
/// above the BMP becomes a surrogate pair of three-byte sequences.
void append_modified(std::string& out, u32 cp) {
    if (cp == 0) {
        out.push_back(static_cast<char>(0xC0));
        out.push_back(static_cast<char>(0x80));
    } else if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        const u32 adjusted = cp - 0x10000;
        const u32 high     = 0xD800 + (adjusted >> 10);
        const u32 low      = 0xDC00 + (adjusted & 0x3FF);
        append_modified(out, high);
        append_modified(out, low);
    }
}

/// Decode one standard-UTF-8 code point, advancing `index`.
u32 next_utf8(std::string_view text, usize& index) noexcept {
    const auto  byte = static_cast<u8>(text[index]);
    const usize left = text.size() - index;

    auto continuation = [&](usize offset) -> u32 {
        return static_cast<u32>(static_cast<u8>(text[index + offset]) & 0x3F);
    };

    if (byte < 0x80) {
        index += 1;
        return byte;
    }
    if ((byte & 0xE0) == 0xC0 && left >= 2) {
        const u32 cp = (static_cast<u32>(byte & 0x1F) << 6) | continuation(1);
        index += 2;
        return cp;
    }
    if ((byte & 0xF0) == 0xE0 && left >= 3) {
        const u32 cp =
            (static_cast<u32>(byte & 0x0F) << 12) | (continuation(1) << 6) | continuation(2);
        index += 3;
        return cp;
    }
    if ((byte & 0xF8) == 0xF0 && left >= 4) {
        const u32 cp = (static_cast<u32>(byte & 0x07) << 18) | (continuation(1) << 12) |
                       (continuation(2) << 6) | continuation(3);
        index += 4;
        return cp;
    }

    index += 1;
    return kReplacementChar;
}

}  // namespace

ReadResult<std::string> decode_modified_utf8(std::span<const u8> bytes) {
    std::string out;
    out.reserve(bytes.size());

    usize i = 0;
    while (i < bytes.size()) {
        const u8 byte = bytes[i];

        if (byte == 0) {
            // A bare null is illegal in modified UTF-8; it is what C0 80 exists
            // to avoid. Reject rather than silently accept a truncated string.
            return std::unexpected{ReadError::MalformedEncoding};
        }

        if (byte < 0x80) {
            out.push_back(static_cast<char>(byte));
            i += 1;
            continue;
        }

        if ((byte & 0xE0) == 0xC0) {
            if (i + 1 >= bytes.size() || (bytes[i + 1] & 0xC0) != 0x80) {
                return std::unexpected{ReadError::MalformedEncoding};
            }
            const u32 cp =
                (static_cast<u32>(byte & 0x1F) << 6) | static_cast<u32>(bytes[i + 1] & 0x3F);
            append_utf8(out, cp);
            i += 2;
            continue;
        }

        if ((byte & 0xF0) == 0xE0) {
            if (i + 2 >= bytes.size() || (bytes[i + 1] & 0xC0) != 0x80 ||
                (bytes[i + 2] & 0xC0) != 0x80) {
                return std::unexpected{ReadError::MalformedEncoding};
            }
            const u32 cp = (static_cast<u32>(byte & 0x0F) << 12) |
                           (static_cast<u32>(bytes[i + 1] & 0x3F) << 6) |
                           static_cast<u32>(bytes[i + 2] & 0x3F);

            // A high surrogate should be followed by a low surrogate, each in
            // its own three-byte sequence. Recombine them into one code point.
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 5 < bytes.size() &&
                (bytes[i + 3] & 0xF0) == 0xE0 && (bytes[i + 4] & 0xC0) == 0x80 &&
                (bytes[i + 5] & 0xC0) == 0x80) {
                const u32 low = (static_cast<u32>(bytes[i + 3] & 0x0F) << 12) |
                                (static_cast<u32>(bytes[i + 4] & 0x3F) << 6) |
                                static_cast<u32>(bytes[i + 5] & 0x3F);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    append_utf8(out, 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00));
                    i += 6;
                    continue;
                }
            }

            // An unpaired surrogate is not representable in standard UTF-8.
            // Substituting keeps a malformed sign from failing a whole chunk.
            append_utf8(out, (cp >= 0xD800 && cp <= 0xDFFF) ? kReplacementChar : cp);
            i += 3;
            continue;
        }

        // Four-byte sequences do not occur in modified UTF-8: astral characters
        // are written as surrogate pairs instead.
        return std::unexpected{ReadError::MalformedEncoding};
    }

    return out;
}

std::string encode_modified_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);

    usize i = 0;
    while (i < text.size()) {
        append_modified(out, next_utf8(text, i));
    }
    return out;
}

usize modified_utf8_length(std::string_view text) noexcept {
    usize length = 0;
    usize i      = 0;
    while (i < text.size()) {
        const u32 cp = next_utf8(text, i);
        if (cp == 0) {
            length += 2;
        } else if (cp < 0x80) {
            length += 1;
        } else if (cp < 0x800) {
            length += 2;
        } else if (cp < 0x10000) {
            length += 3;
        } else {
            length += 6;  // surrogate pair, three bytes each
        }
    }
    return length;
}

bool is_modified_utf8_identical(std::string_view text) noexcept {
    for (const char c : text) {
        const auto byte = static_cast<u8>(c);
        // A null needs the C0 80 form, and a four-byte sequence needs splitting
        // into surrogates. Everything else encodes the same either way.
        if (byte == 0 || (byte & 0xF8) == 0xF0) {
            return false;
        }
    }
    return true;
}

}  // namespace ov::io
