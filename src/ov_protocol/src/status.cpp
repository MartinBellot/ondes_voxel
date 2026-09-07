#include "ov/protocol/status.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {
namespace {

/// Escape a string for JSON.
///
/// The description and player names can come from a config file or from a
/// player, so a raw quote or backslash in either would produce a document the
/// client cannot parse — and an unparseable status shows the server as
/// unreachable, with no indication why.
void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<u8>(c) < 0x20) {
                    // Control characters must be escaped; anything else passes
                    // through, since the string is already valid UTF-8.
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

std::string ServerStatus::to_json() const {
    std::string json;
    json.reserve(512);

    json += R"({"version":{"name":)";
    append_json_string(json, version_name);
    json += R"(,"protocol":)";
    json += std::to_string(protocol);

    json += R"(},"players":{"max":)";
    json += std::to_string(max_players);
    json += R"(,"online":)";
    json += std::to_string(online_players);
    json += R"(,"sample":[)";
    for (usize i = 0; i < sample.size(); ++i) {
        if (i > 0) {
            json.push_back(',');
        }
        // Each sample entry needs a UUID as well as a name; a nil one is
        // accepted and keeps this from needing a player registry.
        json += R"({"name":)";
        append_json_string(json, sample[i]);
        json += R"(,"id":"00000000-0000-0000-0000-000000000000"})";
    }

    json += R"(]},"description":{"text":)";
    append_json_string(json, description);
    json += "}";

    if (!favicon.empty()) {
        json += R"(,"favicon":)";
        append_json_string(json, favicon);
    }

    json += R"(,"enforcesSecureChat":)";
    json += enforces_secure_chat ? "true" : "false";
    json += "}";

    return json;
}

std::optional<Handshake> parse_handshake(std::span<const u8> body) {
    io::ByteReader reader{body};

    const auto protocol_version = read_varint(reader);
    if (!protocol_version) {
        return std::nullopt;
    }

    // 255 is the protocol's limit for the address field. It is attacker-
    // supplied and goes nowhere useful, but bounding it costs nothing.
    auto address = read_string(reader, 255);
    if (!address) {
        return std::nullopt;
    }

    const auto port = reader.read_u16();
    if (!port) {
        return std::nullopt;
    }

    const auto next = read_varint(reader);
    if (!next) {
        return std::nullopt;
    }
    if (*next != 1 && *next != 2) {
        return std::nullopt;
    }

    return Handshake{*protocol_version, std::move(*address), *port, static_cast<NextState>(*next)};
}

std::vector<u8> encode_status_response(const ServerStatus& status) {
    io::ByteWriter writer;
    write_string(writer, status.to_json());
    return writer.take();
}

std::vector<u8> encode_pong(i64 payload) {
    io::ByteWriter writer;
    writer.write_i64(payload);
    return writer.take();
}

}  // namespace ov::net
