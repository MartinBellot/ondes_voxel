#include "ov/protocol/biome_music.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {

std::vector<BiomeMusic> biome_music_from_codec(const nbt::Tag& codec) {
    std::vector<BiomeMusic> out;
    const nbt::Tag* registry = codec.find("minecraft:worldgen/biome");
    const nbt::Tag* values   = registry != nullptr ? registry->find("value") : nullptr;
    const auto*     list     = values != nullptr ? values->list() : nullptr;
    if (list == nullptr) {
        return out;
    }
    for (const nbt::Tag& entry : *list) {
        const nbt::Tag* name    = entry.find("name");
        const nbt::Tag* element = entry.find("element");
        const nbt::Tag* effects = element != nullptr ? element->find("effects") : nullptr;
        const nbt::Tag* music   = effects != nullptr ? effects->find("music") : nullptr;
        const nbt::Tag* sound   = music != nullptr ? music->find("sound") : nullptr;
        if (name == nullptr || sound == nullptr) {
            continue;
        }
        BiomeMusic biome;
        biome.biome = std::string(name->as_string());
        // A sound event is a plain id, or a compound {sound_id, range}.
        if (sound->type() == nbt::TagType::String) {
            biome.sound = std::string(sound->as_string());
        } else if (const nbt::Tag* id = sound->find("sound_id")) {
            biome.sound = std::string(id->as_string());
        }
        if (biome.sound.empty()) {
            continue;  // not shaped like a sound: skipped, not guessed
        }
        if (const nbt::Tag* min = music->find("min_delay")) {
            biome.min_delay = static_cast<i32>(min->as_i64(biome.min_delay));
        }
        if (const nbt::Tag* max = music->find("max_delay")) {
            biome.max_delay = static_cast<i32>(max->as_i64(biome.max_delay));
        }
        if (const nbt::Tag* replace = music->find("replace_current_music")) {
            biome.replace_current = replace->as_bool();
        }
        out.push_back(std::move(biome));
    }
    return out;
}

std::optional<LoginWorld> read_login_world(std::span<const u8> login_play) {
    io::ByteReader reader(login_play);
    // Entity id, hardcore, game mode, previous game mode, the dimension names,
    // the codec, then the dimension type and the dimension name.
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
    const auto type = read_string(reader);
    auto       name = read_string(reader);
    if (!type || !name) {
        return std::nullopt;
    }
    LoginWorld world;
    world.music     = biome_music_from_codec(codec->root);
    world.dimension = std::string(*name);
    return world;
}

std::optional<std::string> read_respawn_dimension(std::span<const u8> respawn) {
    io::ByteReader reader(respawn);
    const auto     type = read_string(reader);
    auto           name = read_string(reader);
    if (!type || !name) {
        return std::nullopt;
    }
    return std::string(*name);
}

world::WorldShape dimension_shape(std::string_view dimension) noexcept {
    if (dimension == "minecraft:the_nether") {
        return world::WorldShape::nether();
    }
    if (dimension == "minecraft:the_end") {
        return world::WorldShape::the_end();
    }
    return world::WorldShape::overworld();
}

}  // namespace ov::net
