#include "ov/world/chunk_storage.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace ov::world {
namespace {

using registry::BlockStateId;

[[nodiscard]] nbt::Tag long_array_of(std::span<const u64> words) {
    nbt::Tag::LongArray longs;
    longs.reserve(words.size());
    for (const u64 word : words) {
        longs.push_back(static_cast<i64>(word));
    }
    return nbt::Tag{std::move(longs)};
}

[[nodiscard]] nbt::Tag byte_array_of(std::span<const u8> bytes) {
    nbt::Tag::ByteArray array;
    array.reserve(bytes.size());
    for (const u8 byte : bytes) {
        array.push_back(byte);
    }
    return nbt::Tag{std::move(array)};
}

/// One palette entry as the disk format spells it: a name, and the properties
/// that distinguish this state from its block's others.
[[nodiscard]] nbt::Tag block_state_tag(const registry::BlockRegistry& blocks, BlockStateId state) {
    nbt::Tag entry = nbt::Tag::make_compound();

    const auto block = blocks.block_of(state);
    entry.compound()->push_back(
        nbt::CompoundEntry{"Name", nbt::Tag{std::string{blocks.block_name(block)}}});

    const auto properties = blocks.properties(block);
    if (properties.empty()) {
        return entry;
    }

    nbt::Tag values = nbt::Tag::make_compound();
    for (const auto& property : properties) {
        values.compound()->push_back(
            nbt::CompoundEntry{std::string{property.name},
                               nbt::Tag{std::string{blocks.property_value(state, property)}}});
    }
    entry.compound()->push_back(nbt::CompoundEntry{"Properties", std::move(values)});
    return entry;
}

/// Resolve a disk palette entry back to a state id.
[[nodiscard]] std::optional<BlockStateId> state_from_tag(const registry::BlockRegistry& blocks,
                                                         const nbt::Tag&                entry) {
    const nbt::Tag* name = entry.find("Name");
    if (name == nullptr) {
        return std::nullopt;
    }
    const auto block = blocks.find_block(name->as_string());
    if (!block) {
        // A block this version does not have. Guessing would put something
        // plausible and wrong in the world.
        return std::nullopt;
    }

    const nbt::Tag* properties = entry.find("Properties");
    if (properties == nullptr || properties->compound() == nullptr) {
        return blocks.default_state(*block);
    }

    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    pairs.reserve(properties->compound()->size());
    for (const auto& property : *properties->compound()) {
        pairs.emplace_back(property.name, property.value.as_string());
    }
    return blocks.state_for(*block, pairs);
}

/// The shape a stored container declares, ready for ChunkSection::load_*.
struct PackedContainer {
    u8               bits{0};
    std::vector<u64> words;
};

/// Read a packed container out of a section, whatever form it is in.
///
/// The data tag is absent when every entry is the same, which is the common
/// case and the one an implementation forgets — treating a missing `data` as an
/// error refuses most of a real world.
///
/// The width is not stored anywhere: it is implied by the palette's size and by
/// which container this is, and blocks and biomes do not share the rule.
/// Deriving it wrongly reads the data as a different encoding entirely.
[[nodiscard]] PackedContainer packed_from(const nbt::Tag& container, std::span<const u16> palette,
                                          bool is_blocks) {
    const nbt::Tag* data  = container.find("data");
    const auto*     longs = data == nullptr ? nullptr : data->get_if<nbt::Tag::LongArray>();

    if (longs == nullptr || longs->empty()) {
        return PackedContainer{0, {}};
    }
    return PackedContainer{
        is_blocks ? bits_for_palette(palette.size()) : bits_for_biome_palette(palette.size()),
        std::vector<u64>(longs->begin(), longs->end())};
}

}  // namespace

nbt::Document to_nbt(const Chunk& chunk, const ChunkCodecContext& context) {
    nbt::Document document;
    document.name = "";
    document.root = nbt::Tag::make_compound();
    auto& root    = *document.root.compound();

    root.push_back(nbt::CompoundEntry{"DataVersion", nbt::Tag{kDataVersion1201}});
    root.push_back(nbt::CompoundEntry{"xPos", nbt::Tag{chunk.position().x}});
    root.push_back(nbt::CompoundEntry{"zPos", nbt::Tag{chunk.position().z}});
    root.push_back(nbt::CompoundEntry{"yPos", nbt::Tag{chunk.shape().min_section()}});
    root.push_back(nbt::CompoundEntry{"Status", nbt::Tag{std::string{"minecraft:full"}}});
    root.push_back(nbt::CompoundEntry{"LastUpdate", nbt::Tag{i64{0}}});
    root.push_back(nbt::CompoundEntry{"InhabitedTime", nbt::Tag{i64{0}}});

    nbt::Tag sections = nbt::Tag::make_list(nbt::TagType::Compound);
    for (usize i = 0; i < chunk.sections().size(); ++i) {
        const ChunkSection& section = chunk.sections()[i];
        const auto section_y = static_cast<i8>(chunk.shape().min_section() + static_cast<i32>(i));

        nbt::Tag entry = nbt::Tag::make_compound();
        entry.compound()->push_back(nbt::CompoundEntry{"Y", nbt::Tag{section_y}});

        // ── Blocks ──────────────────────────────────────────────────────────
        nbt::Tag blocks_tag = nbt::Tag::make_compound();
        nbt::Tag palette    = nbt::Tag::make_list(nbt::TagType::Compound);

        if (context.blocks != nullptr) {
            if (section.blocks().kind() == PaletteKind::Direct) {
                // A direct container has no palette to write, so one is built
                // from the contents. Disk has no direct form: the format is
                // name-based, and 24135 names would be absurd.
                std::vector<u16> distinct;
                std::vector<u16> indices(4096);
                for (usize cell = 0; cell < 4096; ++cell) {
                    const u16  value = section.blocks().get(cell);
                    const auto found = std::ranges::find(distinct, value);
                    if (found == distinct.end()) {
                        indices[cell] = static_cast<u16>(distinct.size());
                        distinct.push_back(value);
                    } else {
                        indices[cell] = static_cast<u16>(std::distance(distinct.begin(), found));
                    }
                }
                for (const u16 value : distinct) {
                    palette.list()->push_back(
                        block_state_tag(*context.blocks, BlockStateId{value}));
                }
                PalettedContainer rebuilt = PalettedContainer::blocks(0);
                rebuilt.assign(indices);
                blocks_tag.compound()->push_back(
                    nbt::CompoundEntry{"data", long_array_of(rebuilt.data())});
            } else {
                for (const u16 value : section.blocks().palette()) {
                    palette.list()->push_back(
                        block_state_tag(*context.blocks, BlockStateId{value}));
                }
                if (!section.blocks().data().empty()) {
                    blocks_tag.compound()->push_back(
                        nbt::CompoundEntry{"data", long_array_of(section.blocks().data())});
                }
            }
        }
        blocks_tag.compound()->insert(blocks_tag.compound()->begin(),
                                      nbt::CompoundEntry{"palette", std::move(palette)});
        entry.compound()->push_back(nbt::CompoundEntry{"block_states", std::move(blocks_tag)});

        // ── Biomes ──────────────────────────────────────────────────────────
        nbt::Tag   biomes_tag    = nbt::Tag::make_compound();
        nbt::Tag   biome_palette = nbt::Tag::make_list(nbt::TagType::String);
        const auto biome_name_of = [&](u16 id) {
            return id < context.biome_names.size() ? context.biome_names[id]
                                                   : std::string_view{"minecraft:plains"};
        };
        for (const u16 value : section.biomes().palette()) {
            biome_palette.list()->push_back(nbt::Tag{std::string{biome_name_of(value)}});
        }
        biomes_tag.compound()->push_back(nbt::CompoundEntry{"palette", std::move(biome_palette)});
        if (!section.biomes().data().empty()) {
            biomes_tag.compound()->push_back(
                nbt::CompoundEntry{"data", long_array_of(section.biomes().data())});
        }
        entry.compound()->push_back(nbt::CompoundEntry{"biomes", std::move(biomes_tag)});

        // ── Light ───────────────────────────────────────────────────────────
        //
        // Written out rather than recomputed on load: there is no light engine
        // on the read path, so recomputing would quietly differ from what the
        // client was last shown. A uniformly dark array is omitted, which is
        // what vanilla does and what makes most sections cost nothing.
        if (!section.sky_light().is_absent()) {
            entry.compound()->push_back(
                nbt::CompoundEntry{"SkyLight", byte_array_of(section.sky_light().to_bytes())});
        }
        if (!section.block_light().is_absent()) {
            entry.compound()->push_back(
                nbt::CompoundEntry{"BlockLight", byte_array_of(section.block_light().to_bytes())});
        }

        sections.list()->push_back(std::move(entry));
    }
    root.push_back(nbt::CompoundEntry{"sections", std::move(sections)});

    nbt::Tag heightmaps = nbt::Tag::make_compound();
    for (const HeightmapType type :
         {HeightmapType::MotionBlocking, HeightmapType::WorldSurface, HeightmapType::OceanFloor}) {
        heightmaps.compound()->push_back(nbt::CompoundEntry{
            std::string{to_string(type)}, long_array_of(chunk.heightmap(type).data())});
    }
    root.push_back(nbt::CompoundEntry{"Heightmaps", std::move(heightmaps)});

    root.push_back(
        nbt::CompoundEntry{"block_entities", nbt::Tag::make_list(nbt::TagType::Compound)});
    root.push_back(nbt::CompoundEntry{"block_ticks", nbt::Tag::make_list(nbt::TagType::Compound)});
    root.push_back(nbt::CompoundEntry{"fluid_ticks", nbt::Tag::make_list(nbt::TagType::Compound)});

    return document;
}

std::optional<Chunk> from_nbt(const nbt::Document& document, const ChunkCodecContext& context) {
    if (context.blocks == nullptr) {
        return std::nullopt;
    }

    const nbt::Tag* x_pos = document.root.find("xPos");
    const nbt::Tag* z_pos = document.root.find("zPos");
    const nbt::Tag* list  = document.root.find("sections");
    if (x_pos == nullptr || z_pos == nullptr || list == nullptr || list->list() == nullptr) {
        return std::nullopt;
    }

    const auto shape = WorldShape::overworld();
    Chunk      chunk{ChunkPos{static_cast<i32>(x_pos->as_i64()), static_cast<i32>(z_pos->as_i64())},
                     shape, context.air};

    std::unordered_map<std::string_view, u16> biome_ids;
    for (u16 id = 0; id < context.biome_names.size(); ++id) {
        biome_ids.emplace(context.biome_names[id], id);
    }

    for (const nbt::Tag& section_tag : *list->list()) {
        const nbt::Tag* y_tag = section_tag.find("Y");
        if (y_tag == nullptr) {
            continue;
        }
        const auto section_y = static_cast<i32>(y_tag->as_i64());
        // Vanilla writes an extra section below and above the world for
        // lighting. They hold no blocks and have nowhere to go here.
        if (section_y < shape.min_section() ||
            section_y >= shape.min_section() + static_cast<i32>(shape.section_count())) {
            continue;
        }
        ChunkSection* section = chunk.section_for_y(section_y * 16);
        if (section == nullptr) {
            continue;
        }

        if (const nbt::Tag* states = section_tag.find("block_states")) {
            const nbt::Tag* palette_tag = states->find("palette");
            if (palette_tag != nullptr && palette_tag->list() != nullptr) {
                std::vector<u16> palette;
                palette.reserve(palette_tag->list()->size());
                for (const nbt::Tag& entry : *palette_tag->list()) {
                    const auto state = state_from_tag(*context.blocks, entry);
                    if (!state) {
                        return std::nullopt;
                    }
                    palette.push_back(state->value());
                }
                const auto packed = packed_from(*states, palette, true);
                if (packed.bits == 0 && palette.size() != 1) {
                    return std::nullopt;
                }
                if (!section->load_blocks(packed.bits, palette, packed.words)) {
                    return std::nullopt;
                }
            }
        }

        if (const nbt::Tag* biomes = section_tag.find("biomes")) {
            const nbt::Tag* palette_tag = biomes->find("palette");
            if (palette_tag != nullptr && palette_tag->list() != nullptr) {
                std::vector<u16> palette;
                palette.reserve(palette_tag->list()->size());
                for (const nbt::Tag& entry : *palette_tag->list()) {
                    const auto found = biome_ids.find(entry.as_string());
                    palette.push_back(found == biome_ids.end() ? 0 : found->second);
                }
                if (!palette.empty()) {
                    const auto packed = packed_from(*biomes, palette, false);
                    (void)section->load_biomes(packed.bits, palette, packed.words);
                }
            }
        }

        if (const nbt::Tag* sky = section_tag.find("SkyLight")) {
            if (const auto* bytes = sky->get_if<nbt::Tag::ByteArray>()) {
                const std::vector<u8> raw(bytes->begin(), bytes->end());
                (void)section->sky_light().load(raw);
            }
        }
        if (const nbt::Tag* block_light = section_tag.find("BlockLight")) {
            if (const auto* bytes = block_light->get_if<nbt::Tag::ByteArray>()) {
                const std::vector<u8> raw(bytes->begin(), bytes->end());
                (void)section->block_light().load(raw);
            }
        }
    }

    // Heightmaps are recomputed rather than trusted. They are derived data, a
    // stored one can be stale — measured on a real world, one column in 4.5
    // million was — and recomputing costs a scan the load already pays for.
    chunk.recompute_world_surface();

    return chunk;
}

}  // namespace ov::world
