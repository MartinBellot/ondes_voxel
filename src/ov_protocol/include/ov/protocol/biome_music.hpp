// What a client needs from Login (play) and Respawn to choose its music.
//
// A biome's background music is not in any packet of its own. It is in the
// registry codec Login (play) carries: each `minecraft:worldgen/biome` entry's
// `element.effects.music` — `sound`, `min_delay`, `max_delay`,
// `replace_current_music` — the same object the data generator writes in the
// biome's JSON. The dimension the player is in is the Login's `dimension name`
// after the codec, and Respawn's second field when it changes.
//
// Read from the wire, as vanilla's client does, rather than from a local copy
// of the data: a server with other biomes gets its own music.
#pragma once

#include "ov/base/types.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/world/chunk.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::net {

struct BiomeMusic {
    /// `minecraft:forest`.
    std::string biome;
    /// `minecraft:music.overworld.forest`.
    std::string sound;
    i32         min_delay{12000};
    i32         max_delay{24000};
    bool        replace_current{false};
};

/// Every biome of a codec that names its music. A biome without a `music`
/// field is absent: the game's music plays there.
[[nodiscard]] std::vector<BiomeMusic> biome_music_from_codec(const nbt::Tag& codec);

struct LoginWorld {
    std::vector<BiomeMusic> music;
    /// `minecraft:overworld`, `minecraft:the_nether`, `minecraft:the_end`.
    std::string dimension;
};

/// The music and the dimension, out of a whole Login (play) body. Nullopt when
/// the packet cannot be read as far as the dimension name.
[[nodiscard]] std::optional<LoginWorld> read_login_world(std::span<const u8> login_play);

/// Respawn's dimension name: its second field, after the dimension type.
[[nodiscard]] std::optional<std::string> read_respawn_dimension(std::span<const u8> respawn);

/// The vertical extent of a dimension, by the name Login (play) and Respawn
/// give it: what a client must parse that dimension's chunk packets with. A
/// Nether chunk carries 16 sections, an Overworld one 24; read with the wrong
/// shape, every one of them fails. Anything unknown is taken for the Overworld.
[[nodiscard]] world::WorldShape dimension_shape(std::string_view dimension) noexcept;

}  // namespace ov::net
