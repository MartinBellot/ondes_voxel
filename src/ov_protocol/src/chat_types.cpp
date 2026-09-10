#include "ov/protocol/chat_types.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {

namespace {

void append_quoted(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
}

/// A decoration's style compound as JSON members. Only the members a chat
/// type's style has in 1.20.1 (colour and the five formats); anything else in
/// the compound is ignored rather than mis-translated.
std::string style_members(const nbt::Tag& style) {
    std::string out;
    const auto  member = [&out](std::string_view key) {
        if (!out.empty()) {
            out += ',';
        }
        append_quoted(out, key);
        out += ':';
    };
    for (const std::string_view key : {"bold", "italic", "underlined", "strikethrough", "obfuscated"}) {
        if (const nbt::Tag* flag = style.find(key)) {
            member(key);
            out += flag->as_bool() ? "true" : "false";
        }
    }
    if (const nbt::Tag* colour = style.find("color"); colour != nullptr &&
                                                      colour->type() == nbt::TagType::String) {
        member("color");
        append_quoted(out, colour->as_string());
    }
    return out;
}

}  // namespace

std::vector<ChatDecoration> chat_types_from_codec(const nbt::Tag& codec) {
    std::vector<ChatDecoration> out;
    const nbt::Tag* registry = codec.find("minecraft:chat_type");
    const nbt::Tag* values   = registry != nullptr ? registry->find("value") : nullptr;
    const auto*     list     = values != nullptr ? values->list() : nullptr;
    if (list == nullptr) {
        return out;
    }
    for (const nbt::Tag& entry : *list) {
        const nbt::Tag* name    = entry.find("name");
        const nbt::Tag* id      = entry.find("id");
        const nbt::Tag* element = entry.find("element");
        const nbt::Tag* chat    = element != nullptr ? element->find("chat") : nullptr;
        const nbt::Tag* key     = chat != nullptr ? chat->find("translation_key") : nullptr;
        if (name == nullptr || id == nullptr || key == nullptr) {
            continue;
        }
        ChatDecoration decoration;
        decoration.id              = static_cast<i32>(id->as_i64(-1));
        decoration.name            = std::string(name->as_string());
        decoration.translation_key = std::string(key->as_string());
        if (const nbt::Tag* parameters = chat->find("parameters")) {
            if (const auto* items = parameters->list()) {
                for (const nbt::Tag& parameter : *items) {
                    decoration.parameters.emplace_back(parameter.as_string());
                }
            }
        }
        if (const nbt::Tag* style = chat->find("style")) {
            decoration.style_json = style_members(*style);
        }
        out.push_back(std::move(decoration));
    }
    return out;
}

std::optional<std::vector<ChatDecoration>> read_login_chat_types(std::span<const u8> login_play) {
    io::ByteReader reader(login_play);
    // Entity id, hardcore, game mode, previous game mode, then the dimension
    // names — the fields encode_login_play writes before the codec.
    if (!reader.read_i32() || !reader.read_u8() || !reader.read_u8() || !reader.read_u8()) {
        return std::nullopt;
    }
    const auto count = read_varint(reader);
    if (!count || *count < 0 || *count > 1024) {
        return std::nullopt;
    }
    for (i32 i = 0; i < *count; ++i) {
        if (!read_string(reader)) {
            return std::nullopt;
        }
    }
    auto codec = nbt::read(reader);
    if (!codec) {
        return std::nullopt;
    }
    return chat_types_from_codec(codec->root);
}

}  // namespace ov::net
