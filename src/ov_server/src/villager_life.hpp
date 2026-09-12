// Villager life, as this server finishes it: what a villager's brain asks for
// that needs more than a LevelView — a baby, an iron golem, a harvested crop
// — the type a villager takes from its biome, gossip forgotten day by day, and
// the gossip the combat path writes (a hit, a killing, a cure).
//
// The rules are in ov_gameplay (brain/villager_brain.hpp, brain/gossip.hpp).
// Callbacks rather than a reference to the server, as for husbandry.hpp. All
// of it runs on the tick thread, in the entity block.
//
// Provenance: docs/provenance/cerveaux.md (scripts/measure_villager_life.py).
#pragma once

#include "ov/entity/world.hpp"
#include "ov/gameplay/villager.hpp"
#include "ov/gameplay/wandering_trader.hpp"
#include "ov/math/random.hpp"

#include <span>
#include "ov/protocol/types.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::server {

struct VillagerLifeHost {
    /// A new mob of this type, with its behaviour, not yet announced.
    std::function<entity::EntityHandle(std::string_view type, Vec3d at)> create_mob;
    /// Tell every client about a mob created by `create_mob`.
    std::function<void(const entity::EntityState& state)> announce;
    /// An Entity Event to every client.
    std::function<void(i32 entity, i8 status)> entity_event;
    /// The biome's registry name at a block; empty when unknown.
    std::function<std::string_view(BlockPos)> biome_at;
    /// Break the crop at a block and say what it drops (item name, count).
    std::function<void(BlockPos, std::vector<std::pair<std::string_view, i32>>& drops)> harvest;
    /// Place a block, by name, at a position (a crop planted).
    std::function<void(BlockPos, std::string_view block)> place;
    /// The height a mob stands at in a column (the motion-blocking top), or
    /// nothing where the column is not resident.
    std::function<std::optional<i32>(i32 x, i32 z)> surface;
};

struct VillagerLifeStats {
    usize births{0};
    usize no_bed{0};
    usize golems{0};
    usize harvests{0};
    usize plants{0};
    usize typed{0};
    usize decays{0};
    usize traders_left{0};
    usize traders_came{0};
};

/// The type a villager takes from its biome. Measured on 53 biomes: the snowy
/// ones snow, desert and badlands desert, jungles jungle, savannas savanna,
/// swamps swamp, taigas and windswept hills taiga, every other plains.
[[nodiscard]] gameplay::VillagerType villager_type_for_biome(std::string_view biome) noexcept;

/// The crop a seed grows into, or empty.
[[nodiscard]] std::string_view crop_of_seed(std::string_view seed) noexcept;

class VillagerLife {
public:
    VillagerLife(const registry::Registries& registries, u64 seed);

    /// Point the villagers' world at this module's event sink and the types
    /// the brains need. Once, when both modules exist.
    void attach(gameplay::VillagerWorld& world);

    /// Before the entity tick: a type from its biome for every villager that
    /// has none yet, and a day's forgetting where one is due.
    VillagerLifeStats before_entity_tick(entity::EntityWorld& world, i64 game_time,
                                         const VillagerLifeHost& host);
    /// After it: the births, golems, harvests and plantings the brains asked for.
    VillagerLifeStats after_entity_tick(entity::EntityWorld& world, const VillagerLifeHost& host);

    // ── Gossip from the combat path ─────────────────────────────────────────
    /// A player hit this villager: minor_negative 25 (measured, 25 then 50).
    void on_player_hurt(entity::EntityWorld& world, i32 villager, const net::Uuid& player);
    /// A player killed this villager: the villagers that saw it hear
    /// major_negative 25 — those within 16 blocks of it.
    void on_player_killed(entity::EntityWorld& world, Vec3d where, i32 victim,
                          const net::Uuid& player);
    /// A player cured the zombie villager this villager was: major_positive
    /// 20 and minor_positive 25.
    void on_cured(entity::EntityWorld& world, i32 villager, const net::Uuid& player);

    [[nodiscard]] i32 iron_golem_type() const noexcept { return iron_golem_type_; }

    // ── The wandering trader ────────────────────────────────────────────────
    /// Once a tick: the spawner's day (wandering_trader.hpp) and, when it comes
    /// up, one chance in ten of a trader near a random player — ten columns
    /// tried within 48 blocks — with two trader llamas beside it (not leashed:
    /// named). True when one came.
    bool tick_trader_spawner(entity::EntityWorld& world, std::span<const Vec3d> players,
                             bool allowed, const VillagerLifeHost& host);
    [[nodiscard]] const gameplay::TraderSpawner& trader_spawner() const noexcept {
        return trader_spawner_;
    }

private:
    gameplay::TraderSpawner trader_spawner_{};
    std::vector<gameplay::brain::VillagerEvent>              events_;
    std::vector<std::pair<std::string_view, i32>>            drops_;
    i32                                                      villager_type_{-1};
    i32                                                      iron_golem_type_{-1};
    math::LegacyRandomSource                                 random_;
};

}  // namespace ov::server
