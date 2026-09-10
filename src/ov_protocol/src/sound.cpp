#include "ov/protocol/sound.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {

namespace {

/// The identifier a sound is named by when it is not sent by id. Mojang's
/// resource locations are at most a few hundred characters; this is the
/// protocol's own string cap.
void write_sound(io::ByteWriter& writer, const SoundRef& sound) {
    if (sound.sound_id >= 0) {
        write_varint(writer, sound.sound_id + 1);
        return;
    }
    write_varint(writer, 0);
    write_string(writer, sound.name);
    writer.write_u8(sound.fixed_range ? 1 : 0);
    if (sound.fixed_range) {
        writer.write_f32(*sound.fixed_range);
    }
}

std::optional<SoundRef> read_sound(io::ByteReader& reader) {
    const auto id = read_varint(reader);
    if (!id || *id < 0) {
        return std::nullopt;
    }
    SoundRef sound;
    if (*id > 0) {
        sound.sound_id = *id - 1;
        return sound;
    }
    auto name = read_string(reader);
    const auto has_range = reader.read_u8();
    if (!name || !has_range) {
        return std::nullopt;
    }
    sound.name = std::move(*name);
    if (*has_range != 0) {
        const auto range = reader.read_f32();
        if (!range) {
            return std::nullopt;
        }
        sound.fixed_range = *range;
    }
    return sound;
}

}  // namespace

i32 sound_coordinate(f64 blocks) noexcept {
    return static_cast<i32>(blocks * 8.0);
}

std::vector<u8> encode_sound_effect(const SoundEffect& sound) {
    io::ByteWriter writer;
    write_sound(writer, sound.sound);
    write_varint(writer, sound.category);
    writer.write_i32(sound.x);
    writer.write_i32(sound.y);
    writer.write_i32(sound.z);
    writer.write_f32(sound.volume);
    writer.write_f32(sound.pitch);
    writer.write_i64(sound.seed);
    return writer.take();
}

std::optional<SoundEffect> parse_sound_effect(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    auto           sound = read_sound(reader);
    if (!sound) {
        return std::nullopt;
    }
    const auto category = read_varint(reader);
    const auto x        = reader.read_i32();
    const auto y        = reader.read_i32();
    const auto z        = reader.read_i32();
    const auto volume   = reader.read_f32();
    const auto pitch    = reader.read_f32();
    const auto seed     = reader.read_i64();
    if (!category || !x || !y || !z || !volume || !pitch || !seed) {
        return std::nullopt;
    }
    return SoundEffect{std::move(*sound), *category, *x, *y, *z, *volume, *pitch, *seed};
}

std::vector<u8> encode_entity_sound_effect(const EntitySoundEffect& sound) {
    io::ByteWriter writer;
    write_sound(writer, sound.sound);
    write_varint(writer, sound.category);
    write_varint(writer, sound.entity_id);
    writer.write_f32(sound.volume);
    writer.write_f32(sound.pitch);
    writer.write_i64(sound.seed);
    return writer.take();
}

std::optional<EntitySoundEffect> parse_entity_sound_effect(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    auto           sound = read_sound(reader);
    if (!sound) {
        return std::nullopt;
    }
    const auto category = read_varint(reader);
    const auto entity   = read_varint(reader);
    const auto volume   = reader.read_f32();
    const auto pitch    = reader.read_f32();
    const auto seed     = reader.read_i64();
    if (!category || !entity || !volume || !pitch || !seed) {
        return std::nullopt;
    }
    return EntitySoundEffect{std::move(*sound), *category, *entity, *volume, *pitch, *seed};
}

std::vector<u8> encode_stop_sound(const StopSound& stop) {
    io::ByteWriter writer;
    const u8       flags = static_cast<u8>((stop.category ? 1 : 0) | (stop.sound.empty() ? 0 : 2));
    writer.write_u8(flags);
    if (stop.category) {
        write_varint(writer, *stop.category);
    }
    if (!stop.sound.empty()) {
        write_string(writer, stop.sound);
    }
    return writer.take();
}

std::optional<StopSound> parse_stop_sound(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     flags = reader.read_u8();
    if (!flags || (*flags & ~3U) != 0) {
        return std::nullopt;
    }
    StopSound stop;
    if ((*flags & 1U) != 0) {
        const auto category = read_varint(reader);
        if (!category) {
            return std::nullopt;
        }
        stop.category = *category;
    }
    if ((*flags & 2U) != 0) {
        auto name = read_string(reader);
        if (!name) {
            return std::nullopt;
        }
        stop.sound = std::move(*name);
    }
    return stop;
}

std::optional<WorldEvent> parse_world_event(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     event  = reader.read_i32();
    const auto     packed = read_position_raw(reader);
    const auto     data   = reader.read_i32();
    const auto     global = reader.read_u8();
    if (!event || !packed || !data || !global) {
        return std::nullopt;
    }
    // 26 bits of X, 26 of Z, 12 of Y, each signed — see read_position_raw.
    const i64 value = *packed;
    WorldEvent out;
    out.event  = *event;
    out.x      = static_cast<i32>(value >> 38);
    out.y      = static_cast<i32>((value << 52) >> 52);
    out.z      = static_cast<i32>((value << 26) >> 38);
    out.data   = *data;
    out.global = *global != 0;
    return out;
}

}  // namespace ov::net
