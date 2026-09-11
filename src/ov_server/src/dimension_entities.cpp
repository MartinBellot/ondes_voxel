// ── persistence ── See dimension_entities.hpp.
#include "dimension_entities.hpp"

#include <utility>

namespace ov::server {

DimensionEntities::DimensionEntities(const registry::Registries& registries,
                                     std::filesystem::path directory, entity::EntityWorld* world,
                                     EntityStorageHost host)
    : world_{world},
      host_{std::move(host)},
      storage_{registries, std::move(directory), world != nullptr} {
    if (world_ == nullptr) {
        own_world_.emplace(registries);
        world_ = &*own_world_;
    }
}

usize DimensionEntities::load_resident(world::ChunkMap& chunks) {
    scratch_.clear();
    chunks.for_each([&](ChunkPos pos, const world::Chunk&) {
        if (!storage_.is_loaded(pos)) {
            scratch_.push_back(pos);
        }
    });
    usize read = 0;
    for (const ChunkPos pos : scratch_) {
        read += storage_.load_chunk(pos, *world_, records_, host_).entities;
    }
    return read;
}

void DimensionEntities::unload(std::span<const ChunkPos> chunks, std::vector<i32>& removed) {
    if (chunks.empty()) {
        return;
    }
    (void)storage_.unload_chunks(chunks, *world_, records_, host_, removed);
}

EntityStorageStats DimensionEntities::save() {
    return storage_.save_all(*world_, records_, host_);
}

}  // namespace ov::server
