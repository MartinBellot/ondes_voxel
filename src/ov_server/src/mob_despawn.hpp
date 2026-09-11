// ── mobs-3 ── Mobs that go away, and the ones that never do.
//
// `gameplay::decide_despawn` has been written and tested since M2 and nothing
// called it: every mob this server spawned stayed until the category cap was
// full, and then nothing ever spawned again. This is the pass that calls it,
// once a tick for every mob, with what the rule needs and an entity world
// cannot say: the nearest player, how long the mob has gone without one near
// (`noActionTime`), and whether it must never be removed
// (`PersistenceRequired`; an animal never is — docs/provenance/mobs-3.md § 2).
//
// The records are also what the Anvil save writes and reads back
// (entity_storage.hpp): a mob's persistence and its name live here, keyed by
// wire id, because `EntityState` (layer 8) carries neither.
#pragma once

#include "ov/entity/world.hpp"
#include "ov/gameplay/food.hpp"
#include "ov/gameplay/spawning.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace ov::server {

/// What a mob carries that the entity world does not.
struct MobRecord {
    /// `PersistenceRequired`: never removed by distance.
    bool persistence_required{false};
    /// `CustomName`, the JSON text component as stored; empty for none. Kept
    /// for the save; it does **not** pin the mob (measured, mobs-3.md § 2).
    std::string custom_name;
    /// `noActionTime`: ticks without a player inside the no-despawn distance.
    i32 no_action_ticks{0};
};

class MobRecords {
public:
    /// The record for a wire id, made on first use.
    [[nodiscard]] MobRecord&       at(i32 network_id) { return records_[network_id]; }
    [[nodiscard]] const MobRecord* find(i32 network_id) const noexcept;
    /// Mark a mob as never removed by distance.
    void pin(i32 network_id) { records_[network_id].persistence_required = true; }
    void forget(i32 network_id) { records_.erase(network_id); }
    [[nodiscard]] usize size() const noexcept { return records_.size(); }

private:
    std::unordered_map<i32, MobRecord> records_;
};

struct DespawnStats {
    usize immediate{0};
    usize random{0};
    usize peaceful{0};
};

class MobDespawn {
public:
    explicit MobDespawn(const registry::Registries& registries);

    /// One tick of every mob. Marks the removed ones `removed`, which the
    /// entity world acts on at the end of its next tick, like every other
    /// removal decided after the entity pass.
    ///
    /// `players` are the feet of every player a mob can be near — spectators
    /// excluded, creative included. With none, nothing is removed by distance:
    /// the rule measures from the nearest player, and there is none.
    /// `skip(type)` names the entities this pass must never touch (arrows,
    /// primed TNT, falling blocks): their owners remove them.
    DespawnStats tick(entity::EntityWorld& world, MobRecords& records,
                      std::span<const Vec3d> players, gameplay::Difficulty difficulty,
                      const std::function<bool(i32 type)>& skip);

private:
    const registry::Registries*         registries_{nullptr};
    std::optional<registry::RegistryId> types_;
    /// The spawner's category per protocol type id, looked up once.
    std::unordered_map<i32, gameplay::MobCategory> categories_;
    /// The pass's own generator, for the one-in-800 draw. Fixed seed: two runs
    /// of the same world thin out the same way.
    math::LegacyRandomSource random_{0x0DE5'9A3E'0000'0003LL};
};

}  // namespace ov::server
