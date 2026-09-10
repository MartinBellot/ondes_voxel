// The `minecraft:chat_type` registry, as a client reads it from Login (play).
//
// A Player Chat Message does not carry its own text layout. It carries an
// index into this registry, and the entry says how to decorate the message:
// `chat.type.text` with parameters [sender, content] draws "<name> message";
// `commands.message.display.incoming` with a grey italic style draws a
// whisper. The entries arrive **in the registry codec** at login, and the index
// is the entry's `id` there — whatever server sent it. A client that hard-coded
// vanilla's order would be right on the vanilla server and on ours, and wrong
// the day either changed; reading the codec is what vanilla does.
#pragma once

#include "ov/base/types.hpp"
#include "ov/nbt/tag.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::net {

struct ChatDecoration {
    /// The index Player/Disguised Chat Message name.
    i32         id{0};
    /// `minecraft:chat`, `minecraft:msg_command_incoming`…
    std::string name;
    /// The translation key of the `chat` decoration.
    std::string translation_key;
    /// Which of "sender", "target", "content" fill the placeholders, in order.
    std::vector<std::string> parameters;
    /// The decoration's style as JSON members without braces — e.g.
    /// `"italic":true,"color":"gray"` — or empty.
    std::string style_json;
};

/// The chat types of a registry codec (its root compound). An entry that is
/// not shaped like one is skipped, not guessed.
[[nodiscard]] std::vector<ChatDecoration> chat_types_from_codec(const nbt::Tag& codec);

/// The same, out of a whole Login (play) body. Nullopt when the packet cannot
/// be read as far as the codec.
[[nodiscard]] std::optional<std::vector<ChatDecoration>> read_login_chat_types(
    std::span<const u8> login_play);

}  // namespace ov::net
