#include "ov/protocol/chat.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/varint.hpp"

#include <algorithm>

namespace ov::net {
namespace {

/// The most argument signatures a vanilla server accepts on one command, from
/// the protocol page. More is a hostile packet, not a long command.
constexpr i32 kMaxArgumentSignatures = 8;

/// The most previous messages a Player Chat Message may name.
constexpr i32 kMaxPreviousMessages = 20;

/// A generous ceiling on a Commands graph. Vanilla's full tree is a few
/// thousand nodes; a count far beyond that is a packet we must not allocate for.
constexpr i32 kMaxCommandNodes = 1 << 16;

/// Suggestions per response. The client lists them all, so a server that sends
/// a million is a denial of service on its own clients — and a parser that
/// believes the count is one on itself.
constexpr i32 kMaxSuggestions = 1 << 14;

/// The length Java gives a string: UTF-16 code units. `read_string` only
/// bounds the *bytes* (three per unit), so a 257-letter ASCII message passes it;
/// the 256 limit on chat is a character limit and is checked here.
[[nodiscard]] usize utf16_length(std::string_view text) noexcept {
    usize units = 0;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0U) == 0x80U) {
            continue;  // a continuation byte adds nothing
        }
        units += byte >= 0xF0U ? 2 : 1;  // a four-byte sequence is a surrogate pair
    }
    return units;
}

/// A string no longer than `max_units` UTF-16 code units, or nothing.
[[nodiscard]] std::optional<std::string> read_bounded(io::ByteReader& reader, u32 max_units) {
    auto text = read_string(reader, max_units);
    if (!text || utf16_length(*text) > max_units) {
        return std::nullopt;
    }
    return std::move(*text);
}

[[nodiscard]] bool read_signature(io::ByteReader& reader, ChatSignature& into) {
    const auto bytes = reader.read_bytes(into.size());
    if (!bytes) {
        return false;
    }
    std::ranges::copy(*bytes, into.begin());
    return true;
}

[[nodiscard]] bool read_acknowledged(io::ByteReader& reader, AcknowledgedBits& into) {
    const auto bytes = reader.read_bytes(into.size());
    if (!bytes) {
        return false;
    }
    std::ranges::copy(*bytes, into.begin());
    return true;
}

[[nodiscard]] std::optional<bool> read_bool(io::ByteReader& reader) {
    const auto value = reader.read_u8();
    if (!value || *value > 1) {
        // A boolean that is neither 0 nor 1 is a field read out of place, and
        // everything after it would be read out of place too.
        return std::nullopt;
    }
    return *value == 1;
}

[[nodiscard]] std::optional<std::string> read_optional_component(io::ByteReader& reader,
                                                                 bool&           present) {
    const auto flag = read_bool(reader);
    if (!flag) {
        return std::nullopt;
    }
    present = *flag;
    if (!present) {
        return std::string{};
    }
    auto text = read_string(reader, kMaxChatComponentLength);
    if (!text) {
        return std::nullopt;
    }
    return std::move(*text);
}

void write_bool(io::ByteWriter& writer, bool value) { writer.write_u8(value ? 1 : 0); }

/// The byte length of a parser's properties, read off the reader.
///
/// Every parser 1.20.1 has, by its id in `minecraft:command_argument_type`
/// (`data/vanilla/1.20.1/generated/reports/registries.json`). Parsers with no
/// properties return an empty span; an id this table does not know makes the
/// packet unreadable — the protocol page says a client must stop there, and so
/// does this.
[[nodiscard]] std::optional<std::vector<u8>> read_properties(io::ByteReader& reader,
                                                             i32             parser) {
    const usize start = reader.position();
    const auto  take  = [&](usize bytes) -> bool { return reader.skip(bytes).has_value(); };
    bool        ok    = true;
    switch (parser) {
    case 1:  // brigadier:float: flags, then min and max if flagged
    case 2:  // brigadier:double
    case 3:  // brigadier:integer
    case 4: {  // brigadier:long
        const auto flags = reader.read_u8();
        if (!flags) {
            return std::nullopt;
        }
        const usize width = parser == 1 || parser == 3 ? 4 : 8;
        if ((*flags & 0x01U) != 0) {
            ok = ok && take(width);
        }
        if ((*flags & 0x02U) != 0) {
            ok = ok && take(width);
        }
        break;
    }
    case 5: {  // brigadier:string: a VarInt enum 0..2
        const auto mode = read_varint(reader);
        ok              = mode && *mode >= 0 && *mode <= 2;
        break;
    }
    case 6:   // minecraft:entity: flags
    case 29:  // minecraft:score_holder: flags
        ok = take(1);
        break;
    case 40:  // minecraft:time: the minimum, an Int
        ok = take(4);
        break;
    case 41:  // resource_or_tag
    case 42:  // resource_or_tag_key
    case 43:  // resource
    case 44: {  // resource_key: the registry, an identifier
        ok = read_string(reader).has_value();
        break;
    }
    default:
        // The parsers without properties. Anything outside 0..48 is refused.
        if (parser < 0 || parser > 48) {
            return std::nullopt;
        }
        break;
    }
    if (!ok) {
        return std::nullopt;
    }
    const usize end = reader.position();
    reader.seek(start);
    const auto bytes = reader.read_bytes(end - start);
    if (!bytes) {
        return std::nullopt;
    }
    return std::vector<u8>(bytes->begin(), bytes->end());
}

}  // namespace

// ── Serverbound ─────────────────────────────────────────────────────────────

std::optional<ChatCommand> parse_chat_command(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    ChatCommand    out;
    auto           command = read_bounded(reader, kMaxChatMessageLength);
    if (!command) {
        return std::nullopt;
    }
    out.command          = std::move(*command);
    const auto timestamp = reader.read_i64();
    const auto salt      = reader.read_i64();
    const auto count     = read_varint(reader);
    if (!timestamp || !salt || !count || *count < 0 || *count > kMaxArgumentSignatures) {
        return std::nullopt;
    }
    out.timestamp = *timestamp;
    out.salt      = *salt;
    out.signatures.resize(static_cast<usize>(*count));
    for (ChatCommand::ArgumentSignature& signature : out.signatures) {
        auto name = read_string(reader, 16);
        if (!name || !read_signature(reader, signature.signature)) {
            return std::nullopt;
        }
        signature.name = std::move(*name);
    }
    const auto message_count = read_varint(reader);
    if (!message_count || !read_acknowledged(reader, out.acknowledged)) {
        return std::nullopt;
    }
    out.message_count = *message_count;
    if (!reader.exhausted()) {
        return std::nullopt;
    }
    return out;
}

std::vector<u8> encode_chat_command(const ChatCommand& command) {
    io::ByteWriter writer;
    write_string(writer, command.command);
    writer.write_i64(command.timestamp);
    writer.write_i64(command.salt);
    write_varint(writer, static_cast<i32>(command.signatures.size()));
    for (const ChatCommand::ArgumentSignature& signature : command.signatures) {
        write_string(writer, signature.name);
        writer.write_bytes(signature.signature);
    }
    write_varint(writer, command.message_count);
    writer.write_bytes(command.acknowledged);
    return writer.take();
}

std::optional<ChatMessage> parse_chat_message(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    ChatMessage    out;
    auto           message = read_bounded(reader, kMaxChatMessageLength);
    if (!message) {
        return std::nullopt;
    }
    out.message          = std::move(*message);
    const auto timestamp = reader.read_i64();
    const auto salt      = reader.read_i64();
    const auto signed_   = read_bool(reader);
    if (!timestamp || !salt || !signed_) {
        return std::nullopt;
    }
    out.timestamp = *timestamp;
    out.salt      = *salt;
    if (*signed_) {
        ChatSignature signature{};
        if (!read_signature(reader, signature)) {
            return std::nullopt;
        }
        out.signature = signature;
    }
    const auto message_count = read_varint(reader);
    if (!message_count || !read_acknowledged(reader, out.acknowledged)) {
        return std::nullopt;
    }
    out.message_count = *message_count;
    if (!reader.exhausted()) {
        return std::nullopt;
    }
    return out;
}

std::vector<u8> encode_chat_message(const ChatMessage& message) {
    io::ByteWriter writer;
    write_string(writer, message.message);
    writer.write_i64(message.timestamp);
    writer.write_i64(message.salt);
    write_bool(writer, message.signature.has_value());
    if (message.signature) {
        writer.write_bytes(*message.signature);
    }
    write_varint(writer, message.message_count);
    writer.write_bytes(message.acknowledged);
    return writer.take();
}

std::optional<i32> parse_message_acknowledgment(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     count = read_varint(reader);
    if (!count || !reader.exhausted()) {
        return std::nullopt;
    }
    return *count;
}

std::optional<SuggestionsRequest> parse_suggestions_request(std::span<const u8> payload) {
    io::ByteReader     reader{payload};
    SuggestionsRequest out;
    const auto         transaction = read_varint(reader);
    if (!transaction) {
        return std::nullopt;
    }
    auto text = read_bounded(reader, kMaxSuggestionRequestLength);
    if (!text || !reader.exhausted()) {
        return std::nullopt;
    }
    out.transaction = *transaction;
    out.text        = std::move(*text);
    return out;
}

std::vector<u8> encode_suggestions_request(const SuggestionsRequest& request) {
    io::ByteWriter writer;
    write_varint(writer, request.transaction);
    write_string(writer, request.text);
    return writer.take();
}

std::optional<u8> parse_change_difficulty(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     value = reader.read_u8();
    if (!value || *value > 3 || !reader.exhausted()) {
        return std::nullopt;
    }
    return *value;
}

// ── Clientbound ─────────────────────────────────────────────────────────────

std::vector<u8> encode_system_chat(std::string_view component_json, bool overlay) {
    io::ByteWriter writer;
    write_string(writer, component_json);
    write_bool(writer, overlay);
    return writer.take();
}

std::optional<SystemChat> parse_system_chat(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    auto           content = read_string(reader, kMaxChatComponentLength);
    if (!content) {
        return std::nullopt;
    }
    const auto overlay = read_bool(reader);
    if (!overlay || !reader.exhausted()) {
        return std::nullopt;
    }
    return SystemChat{std::move(*content), *overlay};
}

std::vector<u8> encode_player_chat(const PlayerChat& chat) {
    io::ByteWriter writer;
    // Header.
    write_uuid(writer, chat.sender);
    write_varint(writer, chat.index);
    write_bool(writer, chat.signature.has_value());
    if (chat.signature) {
        writer.write_bytes(*chat.signature);
    }
    // Body.
    write_string(writer, chat.body);
    writer.write_i64(chat.timestamp);
    writer.write_i64(chat.salt);
    // Previous messages: none. This server signs nothing and relays no
    // signature, so there is no chain to point back into.
    write_varint(writer, 0);
    // Other.
    write_bool(writer, chat.unsigned_content.has_value());
    if (chat.unsigned_content) {
        write_string(writer, *chat.unsigned_content);
    }
    write_varint(writer, 0);  // filter type: PASS_THROUGH
    // Network target.
    write_varint(writer, chat.chat_type);
    write_string(writer, chat.name_json);
    write_bool(writer, chat.target_json.has_value());
    if (chat.target_json) {
        write_string(writer, *chat.target_json);
    }
    return writer.take();
}

std::optional<PlayerChat> parse_player_chat(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    PlayerChat     out;
    const auto     sender = read_uuid(reader);
    const auto     index  = read_varint(reader);
    const auto     signed_ = read_bool(reader);
    if (!sender || !index || !signed_) {
        return std::nullopt;
    }
    out.sender = *sender;
    out.index  = *index;
    if (*signed_) {
        ChatSignature signature{};
        if (!read_signature(reader, signature)) {
            return std::nullopt;
        }
        out.signature = signature;
    }
    auto body = read_bounded(reader, kMaxChatMessageLength);
    if (!body) {
        return std::nullopt;
    }
    out.body             = std::move(*body);
    const auto timestamp = reader.read_i64();
    const auto salt      = reader.read_i64();
    const auto previous  = read_varint(reader);
    if (!timestamp || !salt || !previous || *previous < 0 || *previous > kMaxPreviousMessages) {
        return std::nullopt;
    }
    out.timestamp = *timestamp;
    out.salt      = *salt;
    for (i32 i = 0; i < *previous; ++i) {
        const auto id = read_varint(reader);
        if (!id) {
            return std::nullopt;
        }
        if (*id == 0) {
            ChatSignature ignored{};
            if (!read_signature(reader, ignored)) {
                return std::nullopt;
            }
        }
    }
    bool present  = false;
    auto unsigned_content = read_optional_component(reader, present);
    if (!unsigned_content) {
        return std::nullopt;
    }
    if (present) {
        out.unsigned_content = std::move(*unsigned_content);
    }
    const auto filter = read_varint(reader);
    if (!filter || *filter < 0 || *filter > 2) {
        return std::nullopt;
    }
    if (*filter == 2) {
        const auto longs = read_varint(reader);
        if (!longs || *longs < 0 || !reader.skip(static_cast<usize>(*longs) * 8)) {
            return std::nullopt;
        }
    }
    const auto chat_type = read_varint(reader);
    auto       name      = read_string(reader, kMaxChatComponentLength);
    if (!chat_type || !name) {
        return std::nullopt;
    }
    out.chat_type = *chat_type;
    out.name_json = std::move(*name);
    auto target   = read_optional_component(reader, present);
    if (!target) {
        return std::nullopt;
    }
    if (present) {
        out.target_json = std::move(*target);
    }
    if (!reader.exhausted()) {
        return std::nullopt;
    }
    return out;
}

std::vector<u8> encode_disguised_chat(const DisguisedChat& chat) {
    io::ByteWriter writer;
    write_string(writer, chat.message_json);
    write_varint(writer, chat.chat_type);
    write_string(writer, chat.name_json);
    write_bool(writer, chat.target_json.has_value());
    if (chat.target_json) {
        write_string(writer, *chat.target_json);
    }
    return writer.take();
}

std::optional<DisguisedChat> parse_disguised_chat(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    DisguisedChat  out;
    auto           message   = read_string(reader, kMaxChatComponentLength);
    const auto     chat_type = message ? read_varint(reader) : VarIntResult<i32>{};
    if (!message || !chat_type) {
        return std::nullopt;
    }
    auto name = read_string(reader, kMaxChatComponentLength);
    if (!name) {
        return std::nullopt;
    }
    bool present = false;
    auto target  = read_optional_component(reader, present);
    if (!target || !reader.exhausted()) {
        return std::nullopt;
    }
    out.message_json = std::move(*message);
    out.chat_type    = *chat_type;
    out.name_json    = std::move(*name);
    if (present) {
        out.target_json = std::move(*target);
    }
    return out;
}

std::vector<u8> encode_suggestions_response(const SuggestionsResponse& response) {
    io::ByteWriter writer;
    write_varint(writer, response.transaction);
    write_varint(writer, response.start);
    write_varint(writer, response.length);
    write_varint(writer, static_cast<i32>(response.matches.size()));
    for (const Suggestion& match : response.matches) {
        write_string(writer, match.text);
        write_bool(writer, match.tooltip_json.has_value());
        if (match.tooltip_json) {
            write_string(writer, *match.tooltip_json);
        }
    }
    return writer.take();
}

std::optional<SuggestionsResponse> parse_suggestions_response(std::span<const u8> payload) {
    io::ByteReader      reader{payload};
    SuggestionsResponse out;
    const auto          transaction = read_varint(reader);
    const auto          start       = read_varint(reader);
    const auto          length      = read_varint(reader);
    const auto          count       = read_varint(reader);
    if (!transaction || !start || !length || !count || *count < 0 || *count > kMaxSuggestions) {
        return std::nullopt;
    }
    out.transaction = *transaction;
    out.start       = *start;
    out.length      = *length;
    out.matches.reserve(static_cast<usize>(*count));
    for (i32 i = 0; i < *count; ++i) {
        Suggestion match;
        auto       text = read_string(reader);
        if (!text) {
            return std::nullopt;
        }
        match.text   = std::move(*text);
        bool present = false;
        auto tooltip = read_optional_component(reader, present);
        if (!tooltip) {
            return std::nullopt;
        }
        if (present) {
            match.tooltip_json = std::move(*tooltip);
        }
        out.matches.push_back(std::move(match));
    }
    if (!reader.exhausted()) {
        return std::nullopt;
    }
    return out;
}

std::vector<u8> encode_commands(const CommandGraphWire& graph) {
    io::ByteWriter writer;
    write_varint(writer, static_cast<i32>(graph.nodes.size()));
    for (const CommandNodeWire& node : graph.nodes) {
        writer.write_u8(node.flags);
        write_varint(writer, static_cast<i32>(node.children.size()));
        for (const i32 child : node.children) {
            write_varint(writer, child);
        }
        if ((node.flags & command_flags::kHasRedirect) != 0) {
            write_varint(writer, node.redirect);
        }
        const u8 type = node.flags & command_flags::kTypeMask;
        if (type == command_flags::kLiteral || type == command_flags::kArgument) {
            write_string(writer, node.name);
        }
        if (type == command_flags::kArgument) {
            write_varint(writer, node.parser);
            writer.write_bytes(node.properties);
            if ((node.flags & command_flags::kHasSuggestions) != 0) {
                write_string(writer, node.suggestions);
            }
        }
    }
    write_varint(writer, graph.root);
    return writer.take();
}

std::optional<CommandGraphWire> parse_commands(std::span<const u8> payload) {
    io::ByteReader   reader{payload};
    CommandGraphWire out;
    const auto       count = read_varint(reader);
    if (!count || *count <= 0 || *count > kMaxCommandNodes) {
        return std::nullopt;
    }
    out.nodes.resize(static_cast<usize>(*count));
    const auto in_range = [&](i32 index) { return index >= 0 && index < *count; };
    for (CommandNodeWire& node : out.nodes) {
        const auto flags    = reader.read_u8();
        const auto children = flags ? read_varint(reader) : VarIntResult<i32>{};
        if (!flags || !children || *children < 0 || *children > *count) {
            return std::nullopt;
        }
        node.flags     = *flags;
        const u8 type  = node.flags & command_flags::kTypeMask;
        if (type == 3 || (node.flags & 0xE0U) != 0) {
            return std::nullopt;
        }
        node.children.reserve(static_cast<usize>(*children));
        for (i32 i = 0; i < *children; ++i) {
            const auto child = read_varint(reader);
            if (!child || !in_range(*child)) {
                return std::nullopt;
            }
            node.children.push_back(*child);
        }
        if ((node.flags & command_flags::kHasRedirect) != 0) {
            const auto redirect = read_varint(reader);
            if (!redirect || !in_range(*redirect)) {
                return std::nullopt;
            }
            node.redirect = *redirect;
        }
        if (type == command_flags::kLiteral || type == command_flags::kArgument) {
            auto name = read_string(reader);
            if (!name) {
                return std::nullopt;
            }
            node.name = std::move(*name);
        }
        if (type == command_flags::kArgument) {
            const auto parser = read_varint(reader);
            if (!parser) {
                return std::nullopt;
            }
            node.parser     = *parser;
            auto properties = read_properties(reader, node.parser);
            if (!properties) {
                return std::nullopt;
            }
            node.properties = std::move(*properties);
            if ((node.flags & command_flags::kHasSuggestions) != 0) {
                auto suggestions = read_string(reader);
                if (!suggestions) {
                    return std::nullopt;
                }
                node.suggestions = std::move(*suggestions);
            }
        } else if ((node.flags & command_flags::kHasSuggestions) != 0) {
            // Suggestions only exist on arguments.
            return std::nullopt;
        }
    }
    const auto root = read_varint(reader);
    if (!root || !in_range(*root) || !reader.exhausted()) {
        return std::nullopt;
    }
    out.root = *root;
    return out;
}

std::vector<u8> encode_server_data(std::string_view motd_json, std::span<const u8> icon_png,
                                   bool enforces_secure_chat) {
    io::ByteWriter writer;
    write_string(writer, motd_json);
    write_bool(writer, !icon_png.empty());
    if (!icon_png.empty()) {
        write_varint(writer, static_cast<i32>(icon_png.size()));
        writer.write_bytes(icon_png);
    }
    write_bool(writer, enforces_secure_chat);
    return writer.take();
}

std::vector<u8> encode_change_difficulty(u8 difficulty, bool locked) {
    io::ByteWriter writer;
    writer.write_u8(difficulty);
    write_bool(writer, locked);
    return writer.take();
}

std::vector<u8> encode_component_packet(std::string_view component_json) {
    io::ByteWriter writer;
    write_string(writer, component_json);
    return writer.take();
}

std::vector<u8> encode_title_animation_times(i32 fade_in, i32 stay, i32 fade_out) {
    io::ByteWriter writer;
    writer.write_i32(fade_in);
    writer.write_i32(stay);
    writer.write_i32(fade_out);
    return writer.take();
}

std::vector<u8> encode_clear_titles(bool reset) {
    io::ByteWriter writer;
    write_bool(writer, reset);
    return writer.take();
}

std::vector<u8> encode_update_section_blocks(i32 section_x, i32 section_y, i32 section_z,
                                             std::span<const SectionBlock> blocks) {
    io::ByteWriter writer;
    const u64 packed = ((static_cast<u64>(static_cast<u32>(section_x)) & 0x3FFFFFULL) << 42U) |
                       ((static_cast<u64>(static_cast<u32>(section_z)) & 0x3FFFFFULL) << 20U) |
                       (static_cast<u64>(static_cast<u32>(section_y)) & 0xFFFFFULL);
    writer.write_u64(packed);
    write_varint(writer, static_cast<i32>(blocks.size()));
    for (const SectionBlock& block : blocks) {
        const i64 entry = (static_cast<i64>(block.state) << 12) |
                          (static_cast<i64>(block.x & 15U) << 8) |
                          (static_cast<i64>(block.z & 15U) << 4) | static_cast<i64>(block.y & 15U);
        write_varlong(writer, entry);
    }
    return writer.take();
}

std::optional<SectionBlocks> parse_update_section_blocks(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     packed = reader.read_i64();
    const auto     count  = packed ? read_varint(reader) : VarIntResult<i32>{};
    if (!packed || !count || *count < 0 || *count > 4096) {
        return std::nullopt;
    }
    SectionBlocks out;
    out.section_x = static_cast<i32>(*packed >> 42);
    out.section_y = static_cast<i32>(static_cast<i64>(static_cast<u64>(*packed) << 44U) >> 44);
    out.section_z = static_cast<i32>(static_cast<i64>(static_cast<u64>(*packed) << 22U) >> 42);
    out.blocks.reserve(static_cast<usize>(*count));
    for (i32 i = 0; i < *count; ++i) {
        const auto entry = read_varlong(reader);
        if (!entry) {
            return std::nullopt;
        }
        SectionBlock block;
        block.state = static_cast<i32>(*entry >> 12);
        block.x     = static_cast<u8>((*entry >> 8) & 15);
        block.z     = static_cast<u8>((*entry >> 4) & 15);
        block.y     = static_cast<u8>(*entry & 15);
        out.blocks.push_back(block);
    }
    if (!reader.exhausted()) {
        return std::nullopt;
    }
    return out;
}

std::vector<u8> encode_world_event(i32 event, WirePosition position, i32 data, bool global) {
    io::ByteWriter writer;
    writer.write_i32(event);
    writer.write_i64(pack_position(position));
    writer.write_i32(data);
    write_bool(writer, global);
    return writer.take();
}

std::vector<u8> encode_look_at(bool eyes, f64 x, f64 y, f64 z) {
    io::ByteWriter writer;
    write_varint(writer, eyes ? 1 : 0);
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    write_bool(writer, false);  // not an entity
    return writer.take();
}

std::vector<u8> encode_synchronize_position_relative(f64 x, f64 y, f64 z, f32 yaw, f32 pitch,
                                                     u8 flags, i32 teleport_id) {
    io::ByteWriter writer;
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    writer.write_f32(yaw);
    writer.write_f32(pitch);
    writer.write_u8(flags);
    write_varint(writer, teleport_id);
    return writer.take();
}

std::vector<u8> encode_player_info_game_mode(const Uuid& uuid, i32 game_mode) {
    io::ByteWriter writer;
    writer.write_u8(0x04);  // actions: Update Game Mode only
    write_varint(writer, 1);
    write_uuid(writer, uuid);
    write_varint(writer, game_mode);
    return writer.take();
}

}  // namespace ov::net
