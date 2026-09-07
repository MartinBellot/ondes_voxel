#include "ov/protocol/login.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {
namespace {

/// Escape a string for a JSON chat component.
///
/// The disconnect reason is shown to the client verbatim, and it can carry a
/// player name. An unescaped quote produces a component the client cannot
/// parse, and it then shows a generic error instead of the reason — which is
/// the one thing the message existed to convey.
void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<u8>(c) < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(static_cast<u8>(c) >> 4) & 0xF]);
                    out.push_back(kHex[static_cast<u8>(c) & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

}  // namespace

bool is_valid_player_name(std::string_view name) noexcept {
    if (name.size() < 3 || name.size() > kMaxPlayerNameLength) {
        return false;
    }
    for (const char c : name) {
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

std::optional<LoginStart> parse_login_start(std::span<const u8> body) {
    io::ByteReader reader{body};

    auto name = read_string(reader, kMaxPlayerNameLength);
    if (!name) {
        return std::nullopt;
    }

    LoginStart login;
    login.name = std::move(*name);

    // Since 1.19.3 the client may send a uuid it believes is its own. It is
    // read so the stream stays in sync, and then ignored: an offline server
    // assigns the identity, because trusting this field would let anyone claim
    // anyone else's saved data by asking for their uuid.
    const auto has_uuid = reader.read_u8();
    if (has_uuid && *has_uuid != 0) {
        const auto uuid = read_uuid(reader);
        if (!uuid) {
            return std::nullopt;
        }
        login.claimed_uuid = *uuid;
    }

    return login;
}

std::vector<u8> encode_login_success(const Uuid& uuid, std::string_view name) {
    io::ByteWriter writer;
    write_uuid(writer, uuid);
    write_string(writer, name);
    // Property count. Properties carry the skin and cape signed by Mojang; an
    // offline server has none, so the client falls back to the default skin
    // derived from the uuid.
    write_varint(writer, 0);
    return writer.take();
}

std::vector<u8> encode_set_compression(i32 threshold) {
    io::ByteWriter writer;
    write_varint(writer, threshold);
    return writer.take();
}

std::vector<u8> encode_login_disconnect(std::string_view reason) {
    std::string component;
    component += R"({"text":)";
    append_json_string(component, reason);
    component += "}";

    io::ByteWriter writer;
    write_string(writer, component);
    return writer.take();
}

}  // namespace ov::net
