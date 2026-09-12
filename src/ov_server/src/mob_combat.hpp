// A mob being hit, and what it leaves behind.
//
// Two things the server needed and had nowhere to put:
//
//   * **an invulnerability window per mob.** `EntityState` (layer 8) carries a
//     health and a max health and nothing else — deliberately, because the
//     damage *rules* are layer 9 and a `HealthState` in an ov_entity header
//     would drag them down a layer. So the window lives here, in a table beside
//     the entity world, keyed by the wire id a client names in an Interact.
//
//   * **the entity loot tables.** `EntityLootTables` has been complete and
//     confronted against `/loot give <player> kill <entity>` since the combat
//     wave — 91 of 92 tables conforming, `docs/provenance/combat.md` § 8 — and
//     nothing in the server called it. A mob killed by a player dropped
//     nothing at all.
//
// This file owns neither packets nor entities. It answers "how much came off"
// and "what fell out"; the server sends the packets and places the stacks,
// because only the server has a socket and a ground.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/gameplay/loot.hpp"
#include "ov/gameplay/recipe.hpp"
#include "ov/math/random.hpp"
#include "ov/registry/registries.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

/// Load `entity_loot.ovpack`.
///
/// Nullopt, having said why in the log, when the file is missing or malformed.
/// Refused rather than defaulted: a server with an empty table set kills mobs
/// that drop nothing, which looks exactly like a server whose loot is not wired
/// up — and this whole file exists because that was the state of the world.
[[nodiscard]] std::optional<gameplay::EntityLootTables> load_entity_loot(
    const std::filesystem::path& pack, const registry::Registries& registries,
    const gameplay::RecipeBook* recipes);

/// What one hit did to a mob.
struct MobHurt {
    /// Any health came off.
    bool applied{false};
    /// How much did. Less than asked for when a window was already open.
    f32 dealt{0.0F};
    /// This hit took the last point.
    bool killed{false};
    /// The window swallowed it whole, so the client must not be told to flash.
    bool absorbed{false};
};

/// The damage windows of every mob, and the tables they draw on death.
class MobCombat {
public:
    /// `registries` and `loot` must outlive this. `loot` may be null, in which
    /// case a kill drops nothing and says so once rather than silently.
    MobCombat(const registry::Registries&       registries,
              const gameplay::EntityLootTables* loot) noexcept;

    /// One tick of every open window. Must run **before** the tick's damage —
    /// `tick_health` counts the gap between two hits in these calls.
    void tick(const gameplay::DamageConstants& constants);

    /// Apply one hit, writing the result back into `state.health`.
    ///
    /// The window rules are `gameplay::apply_damage`'s, measured for the
    /// player and the same code here: a mob hit twice inside ten ticks takes
    /// the difference, not the sum.
    [[nodiscard]] MobHurt hurt(entity::EntityState& state, f32 amount,
                               const gameplay::DamageConstants& constants);

    /// ── mobs-4 ── Any damage type through the same window: an effect's
    /// poison or wither, an instant damage. The entity's health is the truth —
    /// an effect may have healed the mob since the window last wrote it — so
    /// the window is reseeded from it before every hit, on both paths.
    [[nodiscard]] MobHurt hurt(entity::EntityState& state, gameplay::DamageKind kind, f32 amount,
                               const gameplay::DamageConstants& constants);

    /// ── mobs-4 ── Absorption lives in the window, where the damage rules
    /// take it from before the health.
    [[nodiscard]] f32 absorption(i32 network_id) const noexcept;
    void              set_absorption(const entity::EntityState& state, f32 amount);

    /// Draw the dead mob's table.
    ///
    /// Appends, like every other loot call in this project. Returns what the
    /// draw could not do — a referenced table that is not an entity table, an
    /// unsupported function — so the caller can count it instead of believing
    /// an empty result.
    ///
    /// `on_fire` is the loot tables' `this is on fire` — a cow that dies
    /// burning drops cooked beef. ── fire ──
    gameplay::DrawResult loot(const entity::EntityState&   state, bool killed_by_player,
                              u8 looting, math::XoroshiroRandomSource& random,
                              std::vector<gameplay::Drop>& out, bool on_fire = false) const;

    /// The registry name of an entity's type, or empty when the registry does
    /// not carry it.
    [[nodiscard]] std::string_view type_name(const entity::EntityState& state) const;

    /// Drop a mob's window. Called when the entity world removes it, so that a
    /// long-running server does not keep a row per mob that ever lived.
    void forget(i32 network_id);

    [[nodiscard]] usize tracked() const noexcept { return windows_.size(); }

private:
    const registry::Registries*        registries_{nullptr};
    const gameplay::EntityLootTables*  loot_{nullptr};
    std::optional<registry::RegistryId> entity_registry_;

    /// One row per mob that has ever been hit — not per mob alive. A mob that
    /// is never touched costs nothing here, which is what keeps this off the
    /// tick's cost even when a thousand of them are standing around.
    std::unordered_map<i32, gameplay::HealthState> windows_;
};

}  // namespace ov::server
