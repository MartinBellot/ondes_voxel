// ── mobs-3 ── Mobs to and from the Anvil entity format: `entities/r.X.Z.mca`.
//
// Since 1.17 a world keeps its entities apart from its blocks: one region file
// per 32×32 chunks under `entities/`, each chunk a compound
// `{DataVersion, Position: [I; x, z], Entities: [...]}`, each entity the
// compound `/data get entity` prints (the wiki's *Entity format*; confirmed
// against a world the real 1.20.1 server wrote, mobs-3.md § 3). Until this
// wave this server wrote none of it: every mob died with the process.
//
// What is kept, and why it can be handed back to vanilla:
//
//   * the fields this server models are written from its own state —
//     position, motion, rotation, UUID, health, `PersistenceRequired`,
//     `CustomName`, the age and love of an animal, a sheep's wool, a pig's
//     saddle, a chicken's egg timer, a villager's data, experience and offers,
//     a slime's size, a creeper's charge, a zombie villager's villager and its
//     conversion;
//   * every other field of a mob that was read from disk — `Brain`,
//     `Attributes`, `ArmorItems`, a wolf's `Owner`, a villager's `Gossips` —
//     is carried through untouched, and so is every entity this server does
//     not spawn at all (a dropped item, an arrow, a painting): it goes back
//     into the chunk it came from. A field this server does not understand is
//     not a field it may throw away.
//
// Threads: everything here runs on the tick thread, which is the only one
// that touches the entity world.
#pragma once

#include "mob_despawn.hpp"

#include "ov/entity/world.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/nbt/region.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/registries.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov::gameplay {
struct VillagerState;
}

namespace ov::server {

/// What the storage reaches outside itself for.
struct EntityStorageHost {
    /// Give a mob read from disk its behaviour: the server's own choice of
    /// brain for the type, as for a mob it spawned itself.
    std::function<void(entity::EntityHandle handle, std::string_view type)> attach;
    /// Tell every client about a mob that has just been read.
    std::function<void(const entity::EntityState& state)> announce;
    /// Entities that are not mobs and are not saved here: arrows, primed TNT,
    /// falling blocks.
    std::function<bool(i32 type)> transient;
    /// A slime's size (1, 2, 4), and setting it on one read from disk.
    std::function<i32(i32 network_id)>                        slime_size;
    std::function<void(entity::EntityState& state, i32 size)> set_slime_size;
    /// A creeper's charge, and charging one read from disk.
    std::function<bool(i32 network_id)> creeper_powered;
    std::function<void(i32 network_id)> charge_creeper;
    /// A zombie villager's remembered villager and its cure's countdown
    /// (-1: not curing). Null: zombie villagers keep nothing.
    std::function<gameplay::VillagerState*(i32 network_id)> zombie_villager;
    std::function<i32(i32 network_id)>                      conversion_time;
    std::function<void(i32 network_id, i32 ticks)>          set_conversion_time;
};

struct EntityStorageStats {
    usize chunks{0};
    usize entities{0};
    /// Carried through untouched: entities this server does not spawn.
    usize carried{0};
    /// Compounds without `id` or `Pos`, or a DataVersion that is not 1.20.1's:
    /// counted and named in the log, never guessed at.
    usize refused{0};
};

class EntityStorage {
public:
    /// `directory` is the world's `entities/`, created on first write.
    EntityStorage(const registry::Registries& registries, std::filesystem::path directory);

    /// Read a chunk's entities into the world, once. A chunk with nothing on
    /// disk is marked loaded all the same.
    EntityStorageStats load_chunk(ChunkPos chunk, entity::EntityWorld& world, MobRecords& records,
                                  const EntityStorageHost& host);

    [[nodiscard]] bool is_loaded(ChunkPos chunk) const noexcept;

    /// Chunks go away: their mobs are written and taken out of the world.
    /// Appends the wire ids removed, for the caller's Remove Entities.
    EntityStorageStats unload_chunks(std::span<const ChunkPos> chunks, entity::EntityWorld& world,
                                     MobRecords& records, const EntityStorageHost& host,
                                     std::vector<i32>& removed);

    /// Write every loaded chunk's mobs. Nothing leaves the world.
    EntityStorageStats save_all(entity::EntityWorld& world, const MobRecords& records,
                                const EntityStorageHost& host);

    /// One mob's compound, as vanilla stores it. Public for the tests.
    [[nodiscard]] nbt::Tag encode(entity::EntityWorld& world, entity::EntityHandle handle,
                                  const MobRecords& records, const EntityStorageHost& host) const;

    /// Spawn one mob from its compound. Nothing for a compound this server
    /// does not spawn (see `carried`); the caller keeps such a compound.
    std::optional<entity::EntityHandle> decode(const nbt::Tag& compound, entity::EntityWorld& world,
                                               MobRecords& records, const EntityStorageHost& host);

    /// Would this type be spawned by `decode`: a living mob this server has a
    /// behaviour or a spawn category for.
    [[nodiscard]] bool spawns(std::string_view type) const;

    /// Forget what was carried through for a mob that is gone for good.
    void forget(i32 network_id) { carried_.erase(network_id); }

private:
    [[nodiscard]] std::filesystem::path region_path(i32 region_x, i32 region_z) const;
    [[nodiscard]] const nbt::RegionFile* region(i32 region_x, i32 region_z);
    /// Write these chunks' lists, one region file at a time.
    void write(const std::map<i64, std::vector<nbt::Tag>>& chunks);
    [[nodiscard]] bool saved(const entity::EntityState& state, const EntityStorageHost& host) const;

    const registry::Registries*         registries_{nullptr};
    std::optional<registry::RegistryId> types_;
    std::optional<registry::RegistryId> items_;
    std::filesystem::path               directory_;
    std::unordered_set<i64>             loaded_;
    /// Region files read, by region; nullopt when there is none on disk.
    std::map<std::pair<i32, i32>, std::optional<nbt::RegionFile>> regions_;
    /// The compound each mob was read with: every field this server does not
    /// model goes back out through it.
    std::unordered_map<i32, nbt::Tag> carried_;
    /// The entities of a chunk this server does not spawn, by chunk key: they
    /// are written back into the chunk exactly as they were read.
    std::unordered_map<i64, std::vector<nbt::Tag>> foreign_;
    /// Chunks whose entry on disk holds entities. A save writes these and the
    /// chunks with mobs now, and nothing else: writing every loaded chunk
    /// wrote 289 empty lists every 30 s on a server with one mob.
    std::unordered_set<i64> on_disk_;
};

}  // namespace ov::server
