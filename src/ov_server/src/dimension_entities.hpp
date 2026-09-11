// ── persistence ── The entities of the Nether and of the End, to and from
// `DIM-1/entities` and `DIM1/entities`.
//
// One `EntityStorage` per dimension — each the only writer of its own
// directory, as the overworld's is of `entities/` — with what the server
// loop needs round it: the chunks to read once they are resident, the chunks
// a level evicted, a save. The Nether's mobs are read into the Nether's own
// entity world (nether_mobs.hpp); the End has no mob world, so its storage
// brings back only what its adopters run (the dragon, the crystals, the items
// and orbs) and carries every other entity through untouched.
//
// Threads: the tick thread, with the players' and the chunks' locks held, as
// for the overworld's storage.
#pragma once

#include "entity_storage.hpp"

#include "ov/entity/world.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk_map.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ov::server {

class DimensionEntities {
public:
    /// `world` null: no mob is brought to life here, and the storage is
    /// handed an empty world of its own.
    DimensionEntities(const registry::Registries& registries, std::filesystem::path directory,
                      entity::EntityWorld* world, EntityStorageHost host);

    DimensionEntities(const DimensionEntities&)            = delete;
    DimensionEntities& operator=(const DimensionEntities&) = delete;

    [[nodiscard]] EntityStorage&       storage() noexcept { return storage_; }
    [[nodiscard]] const EntityStorage& storage() const noexcept { return storage_; }

    /// Read every chunk resident in `chunks` whose entities were not read
    /// yet. Returns how many entities came back.
    usize load_resident(world::ChunkMap& chunks);

    /// The level evicted these chunks: their entities go to disk and leave;
    /// their wire ids are appended to `removed`.
    void unload(std::span<const ChunkPos> chunks, std::vector<i32>& removed);

    EntityStorageStats save();

    [[nodiscard]] bool is_loaded(ChunkPos chunk) const noexcept { return storage_.is_loaded(chunk); }

private:
    std::optional<entity::EntityWorld> own_world_;
    entity::EntityWorld*               world_;
    MobRecords                         records_;
    EntityStorageHost                  host_;
    EntityStorage                      storage_;
    std::vector<ChunkPos>              scratch_;
};

}  // namespace ov::server
