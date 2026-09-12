// ── mobs-5 ── The endermen: anger, teleporting, water and rain, carrying.
//
// The rules are ov_gameplay's (enderman.hpp); this is the session that runs
// them each tick for the overworld's endermen, the way mob_effects.cpp runs the
// effects: a row per enderman, beside the entity world.
//
//   * **anger.** A player whose look meets its eyes (within the measured cone,
//     in range, nothing in the way) angers it, unless that player wears a carved
//     pumpkin; so does a hit. Anger lasts its drawn time; the brain's target is
//     the angering player until then, and the enderman's melee goal
//     (mob_logic.cpp) does the rest.
//   * **teleporting.** When hurt, and every tick it is wet — its feet or its
//     eyes in water, or the rain on it — it takes 1 `drown` damage through its
//     window and tries to teleport (enderman.hpp, random_teleport).
//   * **carrying.** With `mobGriefing`, an empty-handed enderman may take a
//     block of `#enderman_holdable` near its feet, and a carrying one may put
//     its block down on sturdy ground. The block changes are queued, not
//     written: the entity tick runs with the chunks locked, and the server
//     writes them after it (`edits`).
//   * **what the clients see.** Metadata 16 (the block), 17 (screaming) and
//     18 (stared at), measured; at spawn too.
//   * **the Anvil record.** `carriedBlockState`, at the vanilla key, as a
//     falling block's `BlockState` is written.
//
// Not done, and named in docs/provenance/mobs-5.md: the dodge of arrows, the
// teleport towards a far target, daylight teleports, `AngerTime` / `AngryAt`
// in the record, and a line of sight from the eyes to the block it takes.
#pragma once

#include "mob_combat.hpp"
#include "mob_effects.hpp"  // MobEffectHurt

#include "ov/base/types.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/collision.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace ov::server {

/// A player an enderman may be stared at by: alive, in survival or adventure,
/// in the overworld — the players a hostile mob may hunt.
struct EndermanWatcher {
    i32   player{0};
    Vec3d eye{};
    f32   yaw{0.0F};
    f32   pitch{0.0F};
    /// A carved pumpkin on the head: this player's stare provokes nothing.
    bool masked{false};
};

/// A block an enderman took (air) or put down, for the server to write.
struct EndermanBlockEdit {
    BlockPos               pos{};
    registry::BlockStateId state{};
};

/// What the session reaches outside itself for.
struct EndermenHost {
    std::function<bool(BlockPos)>                           raining_at;
    /// A resident block, or air.
    std::function<registry::BlockStateId(BlockPos)>         block_at;
    std::function<void(i32 packet_id, std::span<const u8>)> broadcast;
};

class Endermen {
public:
    /// `combat` may be null: then water does not hurt.
    Endermen(const registry::Registries& registries, const registry::BlockRegistry& blocks,
             MobCombat* combat);

    [[nodiscard]] bool owns(i32 type) const noexcept { return type == enderman_type_ && type >= 0; }

    /// One tick of every enderman in `world`. Runs with the chunks readable
    /// (`collisions`, `host.block_at`); writes none of them.
    void tick(entity::EntityWorld& world, const gameplay::CollisionWorld& collisions,
              std::span<const EndermanWatcher> watchers, bool griefing, i64 tick, i32 min_y,
              const EndermenHost& host);

    /// A hit landed on an enderman: it teleports on its next tick, and a
    /// player's hit angers it at that player (`attacker_player`, 0 for none).
    void on_hurt(i32 id, i32 attacker_player, i64 tick);

    /// The blocks taken and put down since `clear_edits`, in order.
    [[nodiscard]] std::span<const EndermanBlockEdit> edits() const noexcept { return edits_; }
    void clear_edits() noexcept { edits_.clear(); }

    /// The wet hits since `clear_hurts`: the server owes a Damage Event, or the
    /// death when `killed`.
    [[nodiscard]] std::span<const MobEffectHurt> hurts() const noexcept { return hurts_; }
    void clear_hurts() noexcept { hurts_.clear(); }

    /// Metadata 16, 17, 18 for a client that starts watching.
    void spawn_metadata(const entity::EntityState& state, net::MetadataWriter& fields) const;

    /// `carriedBlockState`, written or taken away.
    void write(const entity::EntityState& state, nbt::Tag& out) const;
    void read(entity::EntityState& state, const nbt::Tag& compound);

    void forget(i32 id);

    // ── For tests and the log ───────────────────────────────────────────────
    [[nodiscard]] bool                                  angry(i32 id) const noexcept;
    [[nodiscard]] i32                                   target_of(i32 id) const noexcept;
    [[nodiscard]] std::optional<registry::BlockStateId> carried(i32 id) const noexcept;
    [[nodiscard]] bool holdable(registry::BlockStateId state) const noexcept;
    /// Force the take and put chances (tests only: the calibrated ones are
    /// the game's).
    void set_chances(f32 pick, f32 place) noexcept {
        pick_chance_  = pick;
        place_chance_ = place;
    }

private:
    struct Row {
        i64                    anger_until{0};
        i32                    target{0};
        registry::BlockStateId carried{};
        bool                   has_carried{false};
        bool                   screaming{false};
        bool                   stared{false};
        bool                   teleport_pending{false};
        /// What the clients were last told.
        bool                   sent_valid{false};
        bool                   sent_has_carried{false};
        registry::BlockStateId sent_carried{};
        bool                   sent_screaming{false};
        bool                   sent_stared{false};
    };

    void anger(Row& row, i32 player, i64 tick);
    [[nodiscard]] bool wet(const entity::EntityState& state, const EndermenHost& host) const;
    void teleport(entity::EntityWorld& world, entity::EntityHandle handle, entity::EntityState& state,
                  const gameplay::CollisionWorld& collisions, i32 min_y);
    void carry(Row& row, const entity::EntityState& state, const EndermenHost& host);
    void flush(i32 id, Row& row, const EndermenHost& host) const;
    [[nodiscard]] bool edited(BlockPos pos) const noexcept;

    const registry::BlockRegistry* blocks_{nullptr};
    MobCombat*                     combat_{nullptr};
    i32                            enderman_type_{-1};
    /// `#enderman_holdable`, by block id.
    std::vector<bool>              holdable_;
    registry::BlockStateId         air_{};
    f32                            pick_chance_;
    f32                            place_chance_;
    gameplay::DamageConstants      constants_{};
    math::LegacyRandomSource       random_{0x0E4D'E7A4'0000'0005LL};

    /// Ordered by wire id: the order the endermen draw from the one generator.
    std::map<i32, Row>             rows_;
    std::vector<EndermanBlockEdit> edits_;
    std::vector<MobEffectHurt>     hurts_;
};

}  // namespace ov::server
