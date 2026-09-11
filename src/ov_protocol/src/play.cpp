#include "ov/protocol/play.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/protocol/varint.hpp"
#include "ov/world/heightmap.hpp"

#include <array>
#include <cmath>

namespace ov::net {
namespace {

/// The dimensions the client is told exist. It needs the list to size its own
/// registry; it does not need them to be reachable yet.
constexpr std::array<std::string_view, 3> kWorldNames{
    "minecraft:overworld",
    "minecraft:the_nether",
    "minecraft:the_end",
};

void write_chat_component(io::ByteWriter& writer, std::string_view text) {
    std::string json = R"({"text":")";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            json.push_back('\\');
        }
        json.push_back(c);
    }
    json += R"("})";
    write_string(writer, json);
}

/// A block position packed into a long: 26 bits x, 26 bits z, 12 bits y.
///
/// Note the order — x, then **z**, then y in the low bits. Swapping y and z
/// puts the spawn point somewhere plausible and wrong, and the packing is
/// signed, so the shifts have to be done on unsigned values to avoid relying on
/// implementation-defined behaviour.
void write_position(io::ByteWriter& writer, i32 x, i32 y, i32 z) {
    // Delegates rather than repeating the shifts. Two implementations of one
    // packing is how a reader and a writer end up disagreeing by a bit.
    write_position(writer, WirePosition{x, y, z});
}

/// One paletted container, in the wire's shape.
///
/// The three forms differ in more than size: the width is what tells the reader
/// which one it is looking at, so a container that pads its width into another
/// form's range is decoded as that other form.
void write_paletted_container(io::ByteWriter& writer, const world::PalettedContainer& container) {
    writer.write_u8(container.bits());

    switch (container.kind()) {
        case world::PaletteKind::SingleValue:
            // One value, and no data at all — not an empty array with a length
            // of zero written after a palette.
            write_varint(
                writer, static_cast<i32>(container.palette().empty() ? 0 : container.palette()[0]));
            break;

        case world::PaletteKind::Indirect:
            write_varint(writer, static_cast<i32>(container.palette().size()));
            for (const u16 entry : container.palette()) {
                write_varint(writer, static_cast<i32>(entry));
            }
            break;

        case world::PaletteKind::Direct:
            // No palette section at all.
            break;
    }

    write_varint(writer, static_cast<i32>(container.data().size()));
    for (const u64 word : container.data()) {
        writer.write_u64(word);
    }
}

/// The heightmaps the client is sent, as an NBT compound.
///
/// Only two of the four travel. Sending the others is harmless and sending
/// neither is not: the client uses MOTION_BLOCKING for rain and particles.
[[nodiscard]] std::vector<u8> encode_heightmaps(const world::Chunk& chunk) {
    nbt::Document document;
    document.name = "";
    document.root = nbt::Tag::make_compound();

    for (const world::HeightmapType type :
         {world::HeightmapType::MotionBlocking, world::HeightmapType::WorldSurface}) {
        const auto data = chunk.heightmap(type).data();

        nbt::Tag::LongArray longs;
        longs.reserve(data.size());
        for (const u64 word : data) {
            longs.push_back(static_cast<i64>(word));
        }
        document.root.compound()->push_back(
            nbt::CompoundEntry{std::string{world::to_string(type)}, nbt::Tag{std::move(longs)}});
    }

    return nbt::write(document);
}

}  // namespace

std::vector<u8> encode_login_play(const LoginPlay& login) {
    io::ByteWriter writer;

    writer.write_i32(login.entity_id);
    writer.write_u8(0);  // is hardcore
    writer.write_u8(login.game_mode);
    writer.write_i8(-1);  // previous game mode: none

    write_varint(writer, static_cast<i32>(kWorldNames.size()));
    for (const std::string_view name : kWorldNames) {
        write_string(writer, name);
    }

    writer.write_bytes(login.registry_codec);
    write_string(writer, login.dimension_type);
    write_string(writer, login.dimension_name);

    writer.write_i64(login.hashed_seed);
    write_varint(writer, login.max_players);
    write_varint(writer, login.view_distance);
    write_varint(writer, login.simulation_distance);

    writer.write_u8(0);  // reduced debug info
    writer.write_u8(1);  // enable respawn screen
    writer.write_u8(0);  // is debug
    writer.write_u8(login.is_flat ? 1 : 0);
    writer.write_u8(0);  // has death location

    // Portal cooldown. Present in 763 — confirmed by decoding what a real
    // 1.20.1 server sends, because the schema this was written from also
    // covers later versions and the field could have belonged to one of them.
    write_varint(writer, 0);

    return writer.take();
}

std::vector<u8> encode_synchronize_position(f64 x, f64 y, f64 z, f32 yaw, f32 pitch,
                                            i32 teleport_id) {
    io::ByteWriter writer;
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    writer.write_f32(yaw);
    writer.write_f32(pitch);
    writer.write_u8(0);  // all values absolute
    write_varint(writer, teleport_id);
    return writer.take();
}

std::vector<u8> encode_set_center_chunk(i32 chunk_x, i32 chunk_z) {
    io::ByteWriter writer;
    write_varint(writer, chunk_x);
    write_varint(writer, chunk_z);
    return writer.take();
}

std::vector<u8> encode_unload_chunk(i32 chunk_x, i32 chunk_z) {
    io::ByteWriter writer;
    writer.write_i32(chunk_x);
    writer.write_i32(chunk_z);
    return writer.take();
}

namespace {

/// Angles travel as a single byte: 256 steps to a full turn.
///
/// The conversion has to wrap rather than clamp — a yaw of 350 degrees and one
/// of -10 are the same direction, and clamping would pin a player looking
/// slightly west to due south.
[[nodiscard]] i8 angle_byte(f32 degrees) noexcept {
    const auto steps = static_cast<i32>(std::lround(static_cast<f64>(degrees) * 256.0 / 360.0));
    return static_cast<i8>(static_cast<u8>(steps & 0xFF));
}

}  // namespace

std::vector<u8> encode_player_info_add(const Uuid& uuid, std::string_view name, i32 game_mode) {
    io::ByteWriter writer;

    // add_player | update_game_mode | update_listed. Sending add_player alone
    // leaves the entry unlisted, and an unlisted player is not rendered.
    writer.write_u8(0x01 | 0x04 | 0x08);
    write_varint(writer, 1);

    write_uuid(writer, uuid);
    write_string(writer, name);
    write_varint(writer, 0);  // no skin properties: offline mode has no session
    write_varint(writer, game_mode);
    writer.write_u8(1);  // listed
    return writer.take();
}

std::vector<u8> encode_player_info_remove(const Uuid& uuid) {
    io::ByteWriter writer;
    write_varint(writer, 1);
    write_uuid(writer, uuid);
    return writer.take();
}

std::vector<u8> encode_spawn_player(i32 entity_id, const Uuid& uuid, f64 x, f64 y, f64 z, f32 yaw,
                                    f32 pitch) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    write_uuid(writer, uuid);
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    writer.write_i8(angle_byte(yaw));
    writer.write_i8(angle_byte(pitch));
    return writer.take();
}

std::vector<u8> encode_entity_teleport(i32 entity_id, f64 x, f64 y, f64 z, f32 yaw, f32 pitch,
                                       bool on_ground) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    writer.write_i8(angle_byte(yaw));
    writer.write_i8(angle_byte(pitch));
    writer.write_u8(on_ground ? 1 : 0);
    return writer.take();
}

std::vector<u8> encode_entity_head_rotation(i32 entity_id, f32 yaw) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_i8(angle_byte(yaw));
    return writer.take();
}

std::vector<u8> encode_remove_entity(i32 entity_id) {
    io::ByteWriter writer;
    write_varint(writer, 1);
    write_varint(writer, entity_id);
    return writer.take();
}

void write_slot(io::ByteWriter& writer, const ItemStack& stack) {
    if (stack.empty()) {
        writer.write_u8(0);
        return;
    }
    writer.write_u8(1);
    write_varint(writer, stack.item_id);
    writer.write_i8(stack.count);
    if (stack.nbt.empty()) {
        writer.write_u8(0);  // TAG_End: no tag
    } else {
        writer.write_bytes(stack.nbt);
    }
}

std::optional<ItemStack> read_slot(io::ByteReader& reader) {
    const auto present = reader.read_u8();
    if (!present) {
        return std::nullopt;
    }
    if (*present == 0) {
        return ItemStack{};
    }

    const auto item_id = read_varint(reader);
    const auto count   = reader.read_i8();
    if (!item_id || !count) {
        return std::nullopt;
    }

    ItemStack stack{*item_id, *count, {}};

    // The item's NBT. A single zero byte means there is none; anything else is
    // a tag that has to be consumed, or every field after this slot is read
    // from the middle of it.
    const usize start  = reader.position();
    const auto  marker = reader.read_u8();
    if (!marker) {
        return std::nullopt;
    }
    if (*marker == 0) {
        return stack;
    }

    // Parsed and then kept as the original bytes. The server understands none
    // of what is in here — enchantments, custom names — and re-encoding a tag
    // it does not understand is how data gets lost.
    reader.seek(start);
    if (!nbt::read(reader)) {
        return std::nullopt;
    }
    const usize end = reader.position();
    reader.seek(start);
    const auto bytes = reader.read_bytes(end - start);
    if (!bytes) {
        return std::nullopt;
    }
    stack.nbt.assign(bytes->begin(), bytes->end());
    return stack;
}

std::vector<u8> encode_open_screen(i32 window_id, i32 type, std::string_view title) {
    io::ByteWriter writer;
    write_varint(writer, window_id);
    write_varint(writer, type);
    write_chat_component(writer, title);
    return writer.take();
}

std::vector<u8> encode_container_content(u8 window_id, i32 state_id,
                                         std::span<const ItemStack> slots,
                                         const ItemStack&           carried) {
    io::ByteWriter writer;
    writer.write_u8(window_id);
    write_varint(writer, state_id);
    write_varint(writer, static_cast<i32>(slots.size()));
    for (const ItemStack& slot : slots) {
        write_slot(writer, slot);
    }
    write_slot(writer, carried);
    return writer.take();
}

std::vector<u8> encode_container_slot(i8 window_id, i32 state_id, i16 slot,
                                      const ItemStack& stack) {
    io::ByteWriter writer;
    writer.write_i8(window_id);
    write_varint(writer, state_id);
    writer.write_i16(slot);
    write_slot(writer, stack);
    return writer.take();
}

std::vector<u8> encode_close_container(u8 window_id) {
    io::ByteWriter writer;
    writer.write_u8(window_id);
    return writer.take();
}

std::optional<ContainerClick> parse_container_click(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     window_id = reader.read_u8();
    const auto     state_id  = read_varint(reader);
    const auto     slot      = reader.read_i16();
    const auto     button    = reader.read_i8();
    const auto     mode      = read_varint(reader);
    if (!window_id || !state_id || !slot || !button || !mode) {
        return std::nullopt;
    }

    // The changed-slot array is the client telling us what it *believes*
    // happened. It is read past rather than trusted: a server that applies it
    // lets any client write its own inventory.
    const auto changed = read_varint(reader);
    if (!changed) {
        return std::nullopt;
    }
    for (i32 i = 0; i < *changed; ++i) {
        if (!reader.read_i16() || !read_slot(reader)) {
            return std::nullopt;
        }
    }

    const auto carried = read_slot(reader);
    if (!carried) {
        return std::nullopt;
    }
    return ContainerClick{*window_id, *state_id, *slot, *button, *mode, *carried};
}

std::optional<u8> parse_close_container(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     window_id = reader.read_u8();
    if (!window_id) {
        return std::nullopt;
    }
    return *window_id;
}

std::vector<u8> encode_keep_alive(i64 id) {
    io::ByteWriter writer;
    writer.write_i64(id);
    return writer.take();
}

std::vector<u8> encode_set_default_spawn(i32 x, i32 y, i32 z, f32 angle) {
    io::ByteWriter writer;
    write_position(writer, x, y, z);
    writer.write_f32(angle);
    return writer.take();
}

std::vector<u8> encode_player_abilities(bool invulnerable, bool flying, bool allow_flying,
                                        bool creative, f32 flying_speed, f32 walking_speed) {
    io::ByteWriter writer;

    u8 flags = 0;
    flags |= invulnerable ? 0x01 : 0;
    flags |= flying ? 0x02 : 0;
    flags |= allow_flying ? 0x04 : 0;
    flags |= creative ? 0x08 : 0;
    writer.write_u8(flags);

    writer.write_f32(flying_speed);
    writer.write_f32(walking_speed);
    return writer.take();
}

std::vector<u8> encode_game_event(u8 reason, f32 value) {
    io::ByteWriter writer;
    writer.write_u8(reason);
    writer.write_f32(value);
    return writer.take();
}

std::vector<u8> encode_play_disconnect(std::string_view reason) {
    io::ByteWriter writer;
    write_chat_component(writer, reason);
    return writer.take();
}

namespace {

/// Read one paletted container back, the exact mirror of the writer above.
///
/// The width decides the form, and nothing else does: a container whose width
/// has been padded into another form's range is read as that other form. That
/// is a property of the format, not a quirk of this reader.
[[nodiscard]] bool read_paletted_container(io::ByteReader& reader, u8 max_indirect_bits,
                                           u8& bits, std::vector<u16>& palette,
                                           std::vector<u64>& data) {
    const auto width = reader.read_u8();
    if (!width) {
        return false;
    }
    bits = *width;
    palette.clear();
    data.clear();

    if (bits == 0) {
        const auto value = read_varint(reader);
        if (!value) {
            return false;
        }
        palette.push_back(static_cast<u16>(*value));
    } else if (bits <= max_indirect_bits) {
        const auto count = read_varint(reader);
        if (!count || *count < 0) {
            return false;
        }
        // Bounded before it becomes an allocation: the count comes off a
        // socket, and a palette cannot legitimately exceed what its width
        // addresses.
        if (*count > (1 << max_indirect_bits)) {
            return false;
        }
        palette.reserve(static_cast<usize>(*count));
        for (i32 i = 0; i < *count; ++i) {
            const auto entry = read_varint(reader);
            if (!entry || *entry < 0) {
                return false;
            }
            palette.push_back(static_cast<u16>(*entry));
        }
    }

    const auto words = read_varint(reader);
    if (!words || *words < 0) {
        return false;
    }
    // 4096 cells at one bit each is 64 longs; a biome container is smaller
    // still. Anything larger is a malformed length, not a big chunk.
    if (*words > 4096) {
        return false;
    }
    data.reserve(static_cast<usize>(*words));
    for (i32 i = 0; i < *words; ++i) {
        const auto word = reader.read_u64();
        if (!word) {
            return false;
        }
        data.push_back(*word);
    }
    return true;
}

/// A BitSet on the wire: a length-prefixed array of longs. Only the first is
/// ever needed here — a world is at most 64 light sections tall — but the
/// length still has to be consumed or everything after it shifts.
[[nodiscard]] bool read_bitset(io::ByteReader& reader, u64& first) {
    const auto count = read_varint(reader);
    if (!count || *count < 0 || *count > 64) {
        return false;
    }
    first = 0;
    for (i32 i = 0; i < *count; ++i) {
        const auto word = reader.read_u64();
        if (!word) {
            return false;
        }
        if (i == 0) {
            first = *word;
        }
    }
    return true;
}

}  // namespace

i64 pack_position(WirePosition position) noexcept {
    return static_cast<i64>(((static_cast<u64>(position.x) & 0x3FFFFFF) << 38) |
                            ((static_cast<u64>(position.z) & 0x3FFFFFF) << 12) |
                            (static_cast<u64>(position.y) & 0xFFF));
}

WirePosition unpack_position(i64 packed) noexcept {
    const auto value = static_cast<u64>(packed);
    // Each field is signed and narrower than the type it lands in, so each is
    // shifted up to the top and back down arithmetically to carry the sign.
    WirePosition position;
    position.x = static_cast<i32>(static_cast<i64>(value << 0) >> 38);
    position.y = static_cast<i32>(static_cast<i64>(value << 52) >> 52);
    position.z = static_cast<i32>(static_cast<i64>(value << 26) >> 38);
    return position;
}

void write_position(io::ByteWriter& writer, WirePosition position) {
    writer.write_i64(pack_position(position));
}

std::optional<world::Chunk> parse_chunk_data(std::span<const u8> payload,
                                             const world::WorldShape& shape, world::AirStates air,
                                             const registry::BlockRegistry* blocks) {
    io::ByteReader reader(payload);

    const auto x = reader.read_i32();
    const auto z = reader.read_i32();
    if (!x || !z) {
        return std::nullopt;
    }

    // The heightmaps are read and dropped. They are derived from the blocks
    // that follow in the same packet, and this chunk recomputes them from
    // those — which is the stronger of the two, and already checked against
    // 4577024 real columns. Reading it is still necessary: the NBT has to be
    // consumed or every field after it is misaligned.
    if (!nbt::read(reader)) {
        return std::nullopt;
    }

    world::Chunk chunk{ChunkPos{*x, *z}, shape, air, blocks};

    const auto section_bytes = read_varint(reader);
    if (!section_bytes || *section_bytes < 0 ||
        static_cast<usize>(*section_bytes) > reader.remaining()) {
        return std::nullopt;
    }
    const usize sections_end = reader.position() + static_cast<usize>(*section_bytes);

    std::vector<u16> palette;
    std::vector<u64> data;
    for (usize index = 0; index < shape.section_count(); ++index) {
        world::ChunkSection* section =
            chunk.section_for_y(shape.min_y + static_cast<i32>(index) * 16);
        if (section == nullptr) {
            return std::nullopt;
        }
        // The block count travels but is not trusted: load_blocks recounts,
        // and a peer that lies about it would otherwise leave every section's
        // emptiness test wrong.
        if (!reader.read_i16()) {
            return std::nullopt;
        }

        u8 bits = 0;
        if (!read_paletted_container(reader, 8, bits, palette, data) ||
            !section->load_blocks(bits, palette, data)) {
            return std::nullopt;
        }
        if (!read_paletted_container(reader, 3, bits, palette, data) ||
            !section->load_biomes(bits, palette, data)) {
            return std::nullopt;
        }
    }
    if (reader.position() > sections_end) {
        // More was read than was declared: continuing would read the block
        // entities out of the middle of a palette.
        return std::nullopt;
    }
    // ── nether ── Fewer bytes read than declared is the real server's own
    // shape: it declares its section buffer by an estimate larger than what it
    // writes — 14 859 declared, 14 850 written, in a Nether chunk captured from
    // 1.20.1 (scripts/measure_nether_portal.py) — and its client reads inside
    // the declared buffer and skips the rest. So does this; refusing made every
    // chunk from a vanilla server unreadable.
    if (!reader.skip(sections_end - reader.position())) {
        return std::nullopt;
    }

    const auto entity_count = read_varint(reader);
    if (!entity_count || *entity_count < 0) {
        return std::nullopt;
    }
    for (i32 i = 0; i < *entity_count; ++i) {
        const auto packed = reader.read_u8();
        const auto y      = reader.read_i16();
        const auto type   = read_varint(reader);
        if (!packed || !y || !type) {
            return std::nullopt;
        }
        auto document = nbt::read(reader);
        if (!document) {
            return std::nullopt;
        }
        world::BlockEntity entity;
        entity.x       = static_cast<u8>(*packed >> 4);
        entity.z       = static_cast<u8>(*packed & 15);
        entity.y       = *y;
        entity.type_id = *type;
        entity.data    = std::move(document->root);
        chunk.set_block_entity(std::move(entity));
    }

    u64 sky_mask   = 0;
    u64 block_mask = 0;
    u64 ignored    = 0;
    if (!read_bitset(reader, sky_mask) || !read_bitset(reader, block_mask) ||
        !read_bitset(reader, ignored) || !read_bitset(reader, ignored)) {
        return std::nullopt;
    }

    // The masks cover section_count + 2 entries: one below the world and one
    // above. Those two have no storage here, so bit 0 and the top bit are read
    // past and discarded rather than mapped to a section that does not exist.
    const usize light_sections = shape.section_count() + 2;
    for (const bool sky : {true, false}) {
        const auto count = read_varint(reader);
        if (!count || *count < 0) {
            return std::nullopt;
        }
        i32 seen = 0;
        for (usize i = 0; i < light_sections && seen < *count; ++i) {
            const u64 mask = sky ? sky_mask : block_mask;
            if ((mask >> i & 1U) == 0) {
                continue;
            }
            ++seen;
            const auto length = read_varint(reader);
            if (!length || *length != 2048 ||
                static_cast<usize>(*length) > reader.remaining()) {
                return std::nullopt;
            }
            const auto bytes = payload.subspan(reader.position(), 2048);
            if (!reader.skip(2048)) {
                return std::nullopt;
            }
            if (i == 0 || i > shape.section_count()) {
                continue;
            }
            world::ChunkSection* section =
                chunk.section_for_y(shape.min_y + static_cast<i32>(i - 1) * 16);
            if (section == nullptr) {
                return std::nullopt;
            }
            auto& array = sky ? section->sky_light() : section->block_light();
            if (!array.load(bytes)) {
                return std::nullopt;
            }
            array.compact();
        }
        if (seen != *count) {
            return std::nullopt;
        }
    }

    chunk.recompute_heightmaps();
    return chunk;
}

std::vector<u8> encode_chunk_data(const world::Chunk& chunk) {
    io::ByteWriter writer;

    writer.write_i32(chunk.position().x);
    writer.write_i32(chunk.position().z);
    writer.write_bytes(encode_heightmaps(chunk));

    // The sections, concatenated with no per-section length. One miscounted
    // byte shifts everything after it, and the client reports a decode error
    // that names no field.
    io::ByteWriter sections;
    for (const world::ChunkSection& section : chunk.sections()) {
        sections.write_i16(static_cast<i16>(section.non_air_count()));
        write_paletted_container(sections, section.blocks());
        write_paletted_container(sections, section.biomes());
    }
    write_varint(writer, static_cast<i32>(sections.size()));
    writer.write_bytes(sections.data());

    // Block entities. The x and z are packed into one byte, four bits each,
    // and they are chunk-local — writing world coordinates here puts every one
    // of them in the wrong place without any error.
    write_varint(writer, static_cast<i32>(chunk.block_entities().size()));
    for (const world::BlockEntity& entity : chunk.block_entities()) {
        writer.write_u8(static_cast<u8>(((entity.x & 15) << 4) | (entity.z & 15)));
        writer.write_i16(static_cast<i16>(entity.y));
        write_varint(writer, entity.type_id);
        nbt::Document document;
        document.name = "";
        document.root = entity.data;
        writer.write_bytes(nbt::write(document));
    }

    // Light. The masks cover section_count + 2 entries: one below the world and
    // one above, because light spills past the build limits in both directions.
    const usize light_sections = chunk.shape().section_count() + 2;

    io::ByteWriter sky_data;
    io::ByteWriter block_data;
    u64            sky_mask         = 0;
    u64            block_mask       = 0;
    u64            empty_sky_mask   = 0;
    u64            empty_block_mask = 0;
    usize          sky_count        = 0;
    usize          block_count      = 0;

    for (usize i = 0; i < light_sections; ++i) {
        // The extra section at each end has no storage of its own.
        const world::ChunkSection* section =
            (i == 0 || i > chunk.sections().size()) ? nullptr : &chunk.sections()[i - 1];

        // "Empty" on the wire means *all zero*, not "not stored". A section
        // that is uniformly lit still has to be written out; only a uniformly
        // dark one can be elided. Reading that the other way round renders the
        // whole world black, and the client reports nothing at all.
        const bool has_sky   = section != nullptr && !section->sky_light().is_absent();
        const bool has_block = section != nullptr && !section->block_light().is_absent();

        if (has_sky) {
            sky_mask |= u64{1} << i;
            ++sky_count;
            const auto bytes = section->sky_light().to_bytes();
            write_varint(sky_data, static_cast<i32>(bytes.size()));
            sky_data.write_bytes(bytes);
        } else {
            empty_sky_mask |= u64{1} << i;
        }

        if (has_block) {
            block_mask |= u64{1} << i;
            ++block_count;
            const auto bytes = section->block_light().to_bytes();
            write_varint(block_data, static_cast<i32>(bytes.size()));
            block_data.write_bytes(bytes);
        } else {
            empty_block_mask |= u64{1} << i;
        }
    }

    // Each mask is a BitSet: a length-prefixed array of longs.
    for (const u64 mask : {sky_mask, block_mask, empty_sky_mask, empty_block_mask}) {
        write_varint(writer, 1);
        writer.write_u64(mask);
    }

    write_varint(writer, static_cast<i32>(sky_count));
    writer.write_bytes(sky_data.data());
    write_varint(writer, static_cast<i32>(block_count));
    writer.write_bytes(block_data.data());

    return writer.take();
}

// ── Serverbound ─────────────────────────────────────────────────────────────

std::optional<ClientInformation> parse_client_information(std::span<const u8> payload) {
    io::ByteReader reader{payload};

    const auto locale = read_string(reader, 16);
    if (!locale) {
        return std::nullopt;
    }
    const auto view_distance = reader.read_u8();
    if (!view_distance) {
        return std::nullopt;
    }

    // The remaining fields exist and the server does not act on any of them
    // yet. Reading them would be honest padding; not reading them costs
    // nothing, because the frame length already told us where the packet ends.
    return ClientInformation{std::string{*locale}, *view_distance};
}

std::optional<PlayerMovement> parse_movement(i32 packet_id, std::span<const u8> payload) {
    io::ByteReader reader{payload};
    PlayerMovement movement;

    const bool has_position = packet_id == serverbound::kSetPlayerPosition ||
                              packet_id == serverbound::kSetPlayerPositionRot;
    const bool has_rotation = packet_id == serverbound::kSetPlayerRotation ||
                              packet_id == serverbound::kSetPlayerPositionRot;

    if (!has_position && !has_rotation && packet_id != serverbound::kSetPlayerOnGround) {
        return std::nullopt;
    }

    if (has_position) {
        const auto x = reader.read_f64();
        const auto y = reader.read_f64();
        const auto z = reader.read_f64();
        if (!x || !y || !z) {
            return std::nullopt;
        }
        movement.x = *x;
        movement.y = *y;
        movement.z = *z;
    }
    if (has_rotation) {
        const auto yaw   = reader.read_f32();
        const auto pitch = reader.read_f32();
        if (!yaw || !pitch) {
            return std::nullopt;
        }
        movement.yaw   = *yaw;
        movement.pitch = *pitch;
    }

    const auto on_ground = reader.read_u8();
    if (!on_ground) {
        return std::nullopt;
    }
    movement.on_ground = *on_ground != 0;
    return movement;
}

std::optional<i32> parse_confirm_teleport(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     id = read_varint(reader);
    if (!id) {
        return std::nullopt;
    }
    return *id;
}

namespace {

/// Unpack the 26/26/12 block position.
///
/// All three fields are signed, so each is shifted left to put its sign bit at
/// the top and then arithmetically right — masking instead would make every
/// negative coordinate a large positive one, and half the world is at negative
/// coordinates.
[[nodiscard]] WirePosition unpack_position(u64 packed) noexcept {
    const auto value = static_cast<i64>(packed);
    return WirePosition{
        static_cast<i32>(value >> 38),
        static_cast<i32>(value << 52 >> 52),
        static_cast<i32>(value << 26 >> 38),
    };
}

}  // namespace

std::vector<u8> encode_block_entity_data(WirePosition position, i32 type, const nbt::Tag& data) {
    io::ByteWriter writer;
    write_position(writer, position.x, position.y, position.z);
    write_varint(writer, type);

    nbt::Document document;
    document.name = "";
    document.root = data;
    writer.write_bytes(nbt::write(document));
    return writer.take();
}

std::vector<u8> encode_open_sign_editor(WirePosition position, bool front) {
    io::ByteWriter writer;
    write_position(writer, position.x, position.y, position.z);
    writer.write_u8(front ? 1 : 0);
    return writer.take();
}

std::optional<SignUpdate> parse_update_sign(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     packed = reader.read_u64();
    const auto     front  = reader.read_u8();
    if (!packed || !front) {
        return std::nullopt;
    }

    SignUpdate update;
    update.position = unpack_position(*packed);
    update.front    = *front != 0;
    for (std::string& line : update.lines) {
        // 384 is the cap the protocol puts on a sign line. A client is free to
        // send whatever it likes, so the limit is enforced here rather than
        // trusted.
        const auto text = read_string(reader, 384);
        if (!text) {
            return std::nullopt;
        }
        line = *text;
    }
    return update;
}

WirePosition offset_by_face(WirePosition position, i32 face) noexcept {
    switch (face) {
        case 0: return {position.x, position.y - 1, position.z};
        case 1: return {position.x, position.y + 1, position.z};
        case 2: return {position.x, position.y, position.z - 1};
        case 3: return {position.x, position.y, position.z + 1};
        case 4: return {position.x - 1, position.y, position.z};
        case 5: return {position.x + 1, position.y, position.z};
        default: return position;
    }
}

std::optional<PlayerAction> parse_player_action(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     status = read_varint(reader);
    const auto     packed = reader.read_u64();
    const auto     face   = reader.read_i8();
    if (!status || !packed || !face) {
        return std::nullopt;
    }
    const auto sequence = read_varint(reader);
    if (!sequence) {
        return std::nullopt;
    }
    return PlayerAction{*status, unpack_position(*packed), *face, *sequence};
}

std::optional<UseItemOn> parse_use_item_on(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     hand   = read_varint(reader);
    const auto     packed = reader.read_u64();
    const auto     face   = read_varint(reader);
    if (!hand || !packed || !face) {
        return std::nullopt;
    }
    const auto cursor_x = reader.read_f32();
    const auto cursor_y = reader.read_f32();
    const auto cursor_z = reader.read_f32();
    // The inside-block flag is read past rather than used: it matters for
    // placing against a block the player is standing in, which needs collision.
    if (!cursor_x || !cursor_y || !cursor_z || !reader.read_u8()) {
        return std::nullopt;
    }
    const auto sequence = read_varint(reader);
    if (!sequence) {
        return std::nullopt;
    }
    return UseItemOn{unpack_position(*packed), *face, *cursor_x, *cursor_y, *cursor_z, *sequence};
}

std::optional<CreativeSlot> parse_set_creative_slot(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     slot = reader.read_i16();
    if (!slot) {
        return std::nullopt;
    }
    // ── enchanting ── the slot is read whole, tag included: read_slot keeps
    // the NBT as its original bytes, the form the rest of the server stores.
    auto stack = read_slot(reader);
    if (!stack) {
        return std::nullopt;
    }
    if (stack->empty() && stack->item_id == 0) {
        return CreativeSlot{*slot, std::nullopt, 0, {}};
    }
    return CreativeSlot{*slot, stack->item_id, stack->count, std::move(stack->nbt)};
}

std::optional<i16> parse_set_held_item(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     slot = reader.read_i16();
    if (!slot) {
        return std::nullopt;
    }
    return *slot;
}

std::vector<u8> encode_block_update(WirePosition position, i32 state) {
    io::ByteWriter writer;
    write_position(writer, position.x, position.y, position.z);
    write_varint(writer, state);
    return writer.take();
}

std::vector<u8> encode_spawn_entity(i32 entity_id, const Uuid& uuid, i32 type, f64 x, f64 y,
                                    f64 z) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_u64(uuid.most_significant);
    writer.write_u64(uuid.least_significant);
    write_varint(writer, type);
    writer.write_f64(x);
    writer.write_f64(y);
    writer.write_f64(z);
    writer.write_u8(0);  // pitch
    writer.write_u8(0);  // yaw
    writer.write_u8(0);  // head yaw
    write_varint(writer, 0);
    writer.write_i16(0);  // velocity, in units of 1/8000 of a block per tick
    writer.write_i16(0);
    writer.write_i16(0);
    return writer.take();
}

std::vector<u8> encode_item_metadata(i32 entity_id, i32 item_id, i8 count) {
    io::ByteWriter writer;
    write_varint(writer, entity_id);
    writer.write_u8(8);  // index 8 is the stack an item entity carries
    write_varint(writer, 7);
    writer.write_u8(1);  // the slot is present
    write_varint(writer, item_id);
    writer.write_i8(count);
    writer.write_u8(0);     // no NBT
    writer.write_u8(0xFF);  // end of metadata
    return writer.take();
}

std::vector<u8> encode_take_item(i32 collected, i32 collector, i32 count) {
    io::ByteWriter writer;
    write_varint(writer, collected);
    write_varint(writer, collector);
    write_varint(writer, count);
    return writer.take();
}

std::vector<u8> encode_update_time(i64 world_age, i64 time_of_day) {
    io::ByteWriter writer;
    writer.write_i64(world_age);
    writer.write_i64(time_of_day);
    return writer.take();
}

std::vector<u8> encode_acknowledge_dig(i32 sequence) {
    io::ByteWriter writer;
    write_varint(writer, sequence);
    return writer.take();
}

std::optional<i64> parse_keep_alive(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     id = reader.read_i64();
    if (!id) {
        return std::nullopt;
    }
    return *id;
}

}  // namespace ov::net
