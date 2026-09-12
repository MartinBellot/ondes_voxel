#include "ov/protocol/tab_list.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/varint.hpp"

#include <utility>

namespace ov::net {
namespace {

/// Add Player's name is a String (16); its properties String (32767).
constexpr u32 kMaxNameLength = 16;

/// UTF-16 code units in a UTF-8 string: what a String (n) counts. The string
/// reader bounds the *bytes* at three a unit, so 17 ASCII letters pass a
/// String (16) there, and are refused here.
[[nodiscard]] usize utf16_units(std::string_view text) noexcept {
    usize units = 0;
    for (const char c : text) {
        const auto byte = static_cast<u8>(c);
        if ((byte & 0xC0U) == 0x80U) {
            continue;  // a continuation byte
        }
        units += (byte & 0xF8U) == 0xF0U ? 2 : 1;  // four bytes: a surrogate pair
    }
    return units;
}

[[nodiscard]] std::optional<bool> read_bool(io::ByteReader& reader) {
    const auto value = reader.read_u8();
    if (!value || *value > 1) {
        return std::nullopt;
    }
    return *value == 1;
}

/// A count of elements each at least `min_size` bytes: refused before anything
/// is reserved when the bytes left cannot hold it.
[[nodiscard]] std::optional<usize> read_count(io::ByteReader& reader, usize min_size) {
    const auto count = read_varint(reader);
    if (!count || *count < 0) {
        return std::nullopt;
    }
    const auto wanted = static_cast<usize>(*count);
    if (wanted > reader.remaining() / min_size) {
        return std::nullopt;
    }
    return wanted;
}

[[nodiscard]] std::optional<std::vector<u8>> read_byte_array(io::ByteReader& reader) {
    const auto size = read_count(reader, 1);
    if (!size) {
        return std::nullopt;
    }
    const auto bytes = reader.read_bytes(*size);
    if (!bytes) {
        return std::nullopt;
    }
    return std::vector<u8>(bytes->begin(), bytes->end());
}

[[nodiscard]] bool read_entry(io::ByteReader& reader, u8 actions, PlayerInfoEntry& entry) {
    using namespace player_info;
    const auto uuid = read_uuid(reader);
    if (!uuid) {
        return false;
    }
    entry.uuid = *uuid;
    if ((actions & kAddPlayer) != 0) {
        auto name = read_string(reader, kMaxNameLength);
        if (!name || utf16_units(*name) > kMaxNameLength) {
            return false;
        }
        entry.name = std::move(*name);
        // Name, value, a boolean: at least three bytes a property.
        const auto count = read_count(reader, 3);
        if (!count) {
            return false;
        }
        entry.properties.reserve(*count);
        for (usize i = 0; i < *count; ++i) {
            ProfileProperty property;
            auto            key = read_string(reader);
            if (!key) {
                return false;
            }
            auto value = read_string(reader);
            if (!value) {
                return false;
            }
            const auto signed_ = read_bool(reader);
            if (!signed_) {
                return false;
            }
            property.name  = std::move(*key);
            property.value = std::move(*value);
            if (*signed_) {
                auto signature = read_string(reader);
                if (!signature) {
                    return false;
                }
                property.signature = std::move(*signature);
            }
            entry.properties.push_back(std::move(property));
        }
    }
    if ((actions & kInitializeChat) != 0) {
        const auto has = read_bool(reader);
        if (!has) {
            return false;
        }
        if (*has) {
            ChatSessionData chat;
            const auto      session = read_uuid(reader);
            if (!session) {
                return false;
            }
            const auto expires = reader.read_i64();
            if (!expires) {
                return false;
            }
            chat.session    = *session;
            chat.expires_ms = *expires;
            auto key        = read_byte_array(reader);
            if (!key) {
                return false;
            }
            auto signature = read_byte_array(reader);
            if (!signature) {
                return false;
            }
            chat.public_key    = std::move(*key);
            chat.key_signature = std::move(*signature);
            entry.chat         = std::move(chat);
        }
    }
    if ((actions & kUpdateGameMode) != 0) {
        const auto mode = read_varint(reader);
        if (!mode) {
            return false;
        }
        entry.game_mode = *mode;
    }
    if ((actions & kUpdateListed) != 0) {
        const auto listed = read_bool(reader);
        if (!listed) {
            return false;
        }
        entry.listed = *listed;
    }
    if ((actions & kUpdateLatency) != 0) {
        const auto latency = read_varint(reader);
        if (!latency) {
            return false;
        }
        entry.latency = *latency;
    }
    if ((actions & kUpdateDisplayName) != 0) {
        const auto has = read_bool(reader);
        if (!has) {
            return false;
        }
        if (*has) {
            auto name = read_string(reader, kMaxChatComponentLength);
            if (!name) {
                return false;
            }
            entry.display_name_json = std::move(*name);
        }
    }
    return true;
}

void write_entry(io::ByteWriter& writer, u8 actions, const PlayerInfoEntry& entry) {
    using namespace player_info;
    write_uuid(writer, entry.uuid);
    if ((actions & kAddPlayer) != 0) {
        write_string(writer, entry.name);
        write_varint(writer, static_cast<i32>(entry.properties.size()));
        for (const ProfileProperty& property : entry.properties) {
            write_string(writer, property.name);
            write_string(writer, property.value);
            writer.write_u8(property.signature ? 1 : 0);
            if (property.signature) {
                write_string(writer, *property.signature);
            }
        }
    }
    if ((actions & kInitializeChat) != 0) {
        writer.write_u8(entry.chat ? 1 : 0);
        if (entry.chat) {
            write_uuid(writer, entry.chat->session);
            writer.write_i64(entry.chat->expires_ms);
            write_varint(writer, static_cast<i32>(entry.chat->public_key.size()));
            writer.write_bytes(entry.chat->public_key);
            write_varint(writer, static_cast<i32>(entry.chat->key_signature.size()));
            writer.write_bytes(entry.chat->key_signature);
        }
    }
    if ((actions & kUpdateGameMode) != 0) {
        write_varint(writer, entry.game_mode);
    }
    if ((actions & kUpdateListed) != 0) {
        writer.write_u8(entry.listed ? 1 : 0);
    }
    if ((actions & kUpdateLatency) != 0) {
        write_varint(writer, entry.latency);
    }
    if ((actions & kUpdateDisplayName) != 0) {
        writer.write_u8(entry.display_name_json ? 1 : 0);
        if (entry.display_name_json) {
            write_string(writer, *entry.display_name_json);
        }
    }
}

}  // namespace

std::vector<u8> encode_player_info_update(const PlayerInfoUpdate& update) {
    io::ByteWriter writer;
    writer.write_u8(update.actions);
    write_varint(writer, static_cast<i32>(update.entries.size()));
    for (const PlayerInfoEntry& entry : update.entries) {
        write_entry(writer, update.actions, entry);
    }
    return writer.take();
}

std::optional<PlayerInfoUpdate> parse_player_info_update(std::span<const u8> payload) {
    io::ByteReader   reader{payload};
    PlayerInfoUpdate update;
    const auto       actions = reader.read_u8();
    // The two top bits name no action: a mask with them set was built for
    // another protocol, and its entries have fields this reader cannot size.
    if (!actions || (*actions & ~player_info::kAllActions) != 0) {
        return std::nullopt;
    }
    update.actions = *actions;
    const auto count = read_count(reader, 16);  // a UUID at least
    if (!count) {
        return std::nullopt;
    }
    update.entries.resize(*count);
    for (PlayerInfoEntry& entry : update.entries) {
        if (!read_entry(reader, update.actions, entry)) {
            return std::nullopt;
        }
    }
    if (!reader.exhausted()) {
        return std::nullopt;
    }
    return update;
}

std::vector<u8> encode_player_info_remove_many(std::span<const Uuid> uuids) {
    io::ByteWriter writer;
    write_varint(writer, static_cast<i32>(uuids.size()));
    for (const Uuid& uuid : uuids) {
        write_uuid(writer, uuid);
    }
    return writer.take();
}

std::optional<std::vector<Uuid>> parse_player_info_remove(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     count = read_count(reader, 16);
    if (!count) {
        return std::nullopt;
    }
    std::vector<Uuid> uuids;
    uuids.reserve(*count);
    for (usize i = 0; i < *count; ++i) {
        const auto uuid = read_uuid(reader);
        if (!uuid) {
            return std::nullopt;
        }
        uuids.push_back(*uuid);
    }
    if (!reader.exhausted()) {
        return std::nullopt;
    }
    return uuids;
}

std::vector<u8> encode_tab_list_header_footer(const TabListHeaderFooter& texts) {
    io::ByteWriter writer;
    write_string(writer, texts.header_json);
    write_string(writer, texts.footer_json);
    return writer.take();
}

std::optional<TabListHeaderFooter> parse_tab_list_header_footer(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    auto           header = read_string(reader, kMaxChatComponentLength);
    if (!header) {
        return std::nullopt;
    }
    auto footer = read_string(reader, kMaxChatComponentLength);
    if (!footer || !reader.exhausted()) {
        return std::nullopt;
    }
    return TabListHeaderFooter{std::move(*header), std::move(*footer)};
}

std::vector<u8> encode_open_horse_screen(const OpenHorseScreen& screen) {
    io::ByteWriter writer;
    writer.write_u8(screen.window_id);
    write_varint(writer, screen.slot_count);
    writer.write_i32(screen.entity_id);
    return writer.take();
}

std::optional<OpenHorseScreen> parse_open_horse_screen(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     window = reader.read_u8();
    if (!window) {
        return std::nullopt;
    }
    const auto slots = read_varint(reader);
    if (!slots || *slots < 0) {
        return std::nullopt;
    }
    const auto entity = reader.read_i32();
    if (!entity || !reader.exhausted()) {
        return std::nullopt;
    }
    return OpenHorseScreen{*window, *slots, *entity};
}

}  // namespace ov::net
