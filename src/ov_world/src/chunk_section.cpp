#include "ov/world/chunk_section.hpp"

#include <vector>

namespace ov::world {
namespace {

/// Look up a block's only state, or fall back to `fallback`.
///
/// air, void_air and cave_air each have exactly one state — measured against
/// the generated dataset, not assumed — so the block's first state is the whole
/// of it.
[[nodiscard]] registry::BlockStateId single_state(const registry::BlockRegistry& blocks,
                                                  std::string_view               name,
                                                  registry::BlockStateId         fallback) {
    const auto block = blocks.find_block(name);
    if (!block) {
        return fallback;
    }
    return blocks.first_state(*block);
}

}  // namespace

AirStates AirStates::from(const registry::BlockRegistry& blocks) {
    const auto air = single_state(blocks, "minecraft:air", registry::BlockStateId{0});
    return AirStates{
        air,
        single_state(blocks, "minecraft:void_air", air),
        single_state(blocks, "minecraft:cave_air", air),
    };
}

ChunkSection::ChunkSection(AirStates air)
    : air_{air},
      blocks_{PalettedContainer::blocks(air.air.value())},
      biomes_{PalettedContainer::biomes(0)} {}

registry::BlockStateId ChunkSection::get_block(usize x, usize y, usize z) const noexcept {
    return registry::BlockStateId{blocks_.get(section_index(x, y, z))};
}

void ChunkSection::set_block(usize x, usize y, usize z, registry::BlockStateId state) {
    if (x >= kSectionSize || y >= kSectionSize || z >= kSectionSize) {
        return;
    }

    const usize index = section_index(x, y, z);
    const bool  was   = air_.is_air(registry::BlockStateId{blocks_.get(index)});
    const bool  now   = air_.is_air(state);

    blocks_.set(index, state.value());

    // Adjust rather than recount: the wire needs this number on every send.
    if (was && !now) {
        ++non_air_count_;
    } else if (!was && now) {
        --non_air_count_;
    }
}

u16 ChunkSection::get_biome(usize x, usize y, usize z) const noexcept {
    return biomes_.get(biome_index(x, y, z));
}

void ChunkSection::set_biome(usize x, usize y, usize z, u16 biome) {
    if (x >= kSectionSize || y >= kSectionSize || z >= kSectionSize) {
        return;
    }
    biomes_.set(biome_index(x, y, z), biome);
}

void ChunkSection::fill_biome(u16 biome) {
    // assign() picks the tightest representation, which for one distinct value
    // is single-valued with no packed data at all — what vanilla writes.
    const std::vector<u16> all(64, biome);
    biomes_.assign(all);
}

bool ChunkSection::load_blocks(u8 bits, std::span<const u16> palette, std::span<const u64> data) {
    if (!blocks_.load_packed(bits, palette, data)) {
        return false;
    }
    recount();
    return true;
}

bool ChunkSection::load_biomes(u8 bits, std::span<const u16> palette, std::span<const u64> data) {
    return biomes_.load_packed(bits, palette, data);
}

void ChunkSection::recount() noexcept {
    // The single-valued case is the common one by a wide margin, and answering
    // it without walking 4096 entries is what keeps loading a world fast.
    if (blocks_.is_single_valued()) {
        non_air_count_ = air_.is_air(registry::BlockStateId{blocks_.get(0)}) ? 0 : 4096;
        return;
    }

    u16 count = 0;
    for (usize i = 0; i < 4096; ++i) {
        if (!air_.is_air(registry::BlockStateId{blocks_.get(i)})) {
            ++count;
        }
    }
    non_air_count_ = count;
}

}  // namespace ov::world
