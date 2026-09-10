// How vanilla names things in its messages.
//
// A player is not "ovprobe": it is a component with an insertion, a click
// that suggests `/tell ovprobe `, and a hover that shows the entity — and a cow
// is a translatable `entity.minecraft.cow` with its uuid as insertion. Every
// feedback line that names something carries this, and the capture has the
// exact JSON of each.
#pragma once

#include "context.hpp"
#include "env.hpp"
#include "text.hpp"

#include "ov/nbt/tag.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace ov::server::cmd {

/// A player's display name.
[[nodiscard]] Text player_display_name(std::string_view name, const net::Uuid& uuid);

/// Any entity's display name: a player's as above, a mob's as its type.
[[nodiscard]] Text entity_display_name(const EntityInfo& entity, const ParseEnv& env,
                                       const Lang* lang);

/// `entity.minecraft.cow` for "minecraft:cow".
[[nodiscard]] std::string entity_translation_key(std::string_view type);

/// The name an item shows: its custom name when it has one, else
/// `item.minecraft.x` or `block.minecraft.x`. Which of the two a vanilla item
/// uses is Java code (a block item takes its block's key), so it is decided by
/// the language table when one is loaded and by "is there a block of that
/// name" when not — stated, since the two can disagree on a handful of items.
[[nodiscard]] Text item_name(std::string_view id, const nbt::Tag* tag, const ParseEnv& env,
                             const Lang* lang);

/// `[Diamond]` with the item hover, as `give` prints it.
[[nodiscard]] Text item_display(std::string_view id, i32 count, const nbt::Tag* tag,
                                const ParseEnv& env, const Lang* lang);

/// The seed, green, bracketed, copy-on-click.
[[nodiscard]] Text copy_on_click(std::string text);

}  // namespace ov::server::cmd
