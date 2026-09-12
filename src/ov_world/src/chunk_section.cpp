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
      storage_{std::make_shared<Storage>(Storage{
          PalettedContainer::blocks(air.air.value()),
          PalettedContainer::biomes(0),
          LightArray{},
          LightArray{},
          0,
      })} {}

ChunkSection::ChunkSection(const ChunkSection& other) noexcept
    : air_{other.air_}, storage_{other.storage_}, shared_{true} {
    other.shared_ = true;
}

ChunkSection& ChunkSection::operator=(const ChunkSection& other) noexcept {
    if (this != &other) {
        air_          = other.air_;
        storage_      = other.storage_;
        shared_       = true;
        other.shared_ = true;
    }
    return *this;
}

void ChunkSection::unshare() {
    if (!shared_) {
        return;
    }
    // Read-only on the old storage: another thread may be reading it too, and
    // two readers are not a race. The old one is freed by whoever lets go last.
    storage_ = std::make_shared<Storage>(*storage_);
    shared_  = false;
}

registry::BlockStateId ChunkSection::get_block(usize x, usize y, usize z) const noexcept {
    return registry::BlockStateId{storage_->blocks.get(section_index(x, y, z))};
}

void ChunkSection::set_block(usize x, usize y, usize z, registry::BlockStateId state) {
    if (x >= kSectionSize || y >= kSectionSize || z >= kSectionSize) {
        return;
    }
    unshare();

    Storage&    storage = *storage_;
    const usize index   = section_index(x, y, z);
    const bool  was     = air_.is_air(registry::BlockStateId{storage.blocks.get(index)});
    const bool  now     = air_.is_air(state);

    storage.blocks.set(index, state.value());

    // Adjust rather than recount: the wire needs this number on every send.
    if (was && !now) {
        ++storage.non_air_count;
    } else if (!was && now) {
        --storage.non_air_count;
    }
}

u16 ChunkSection::get_biome(usize x, usize y, usize z) const noexcept {
    return storage_->biomes.get(biome_index(x, y, z));
}

void ChunkSection::set_biome(usize x, usize y, usize z, u16 biome) {
    if (x >= kSectionSize || y >= kSectionSize || z >= kSectionSize) {
        return;
    }
    unshare();
    storage_->biomes.set(biome_index(x, y, z), biome);
}

void ChunkSection::fill_biome(u16 biome) {
    unshare();
    // assign() picks the tightest representation, which for one distinct value
    // is single-valued with no packed data at all — what vanilla writes.
    const std::vector<u16> all(64, biome);
    storage_->biomes.assign(all);
}

bool ChunkSection::load_blocks(u8 bits, std::span<const u16> palette, std::span<const u64> data) {
    unshare();
    if (!storage_->blocks.load_packed(bits, palette, data)) {
        return false;
    }
    recount();
    return true;
}

bool ChunkSection::load_biomes(u8 bits, std::span<const u16> palette, std::span<const u64> data) {
    unshare();
    return storage_->biomes.load_packed(bits, palette, data);
}

void ChunkSection::recount() {
    unshare();
    Storage& storage = *storage_;

    // The single-valued case is the common one by a wide margin, and answering
    // it without walking 4096 entries is what keeps loading a world fast.
    if (storage.blocks.is_single_valued()) {
        storage.non_air_count =
            air_.is_air(registry::BlockStateId{storage.blocks.get(0)}) ? 0 : 4096;
        return;
    }

    u16 count = 0;
    for (usize i = 0; i < 4096; ++i) {
        if (!air_.is_air(registry::BlockStateId{storage.blocks.get(i)})) {
            ++count;
        }
    }
    storage.non_air_count = count;
}

}  // namespace ov::world
