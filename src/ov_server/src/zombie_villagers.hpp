// ── mobs-3 ── A villager a zombie kills, and the cure that brings it back.
//
// Measured on the real server (docs/provenance/villageois.md § 10) and not
// wired until this wave:
//
//   * a villager killed by a zombie rises as a zombie villager — never on
//     Easy (0/10), half the time on Normal (37/90), always on Hard (10/10) —
//     keeping its type, profession, level, experience and offers;
//   * a zombie villager under Weakness, given a golden apple, starts to cure:
//     `ConversionTime` 3600 + a draw in 0..2400 (measured 3734 to 5974 over 23),
//     shortened by iron bars and beds nearby (the wiki's rule: one tick in a
//     hundred, each such block within four has three chances in ten to take a
//     tick more off, fourteen blocks at most);
//   * cured, it is the same villager again.
//
// The zombie villager's own brain is a zombie's (mob_species.cpp); what it
// remembers of the villager lives here, keyed by wire id, and the Anvil save
// reads and writes it through `kept`.
#pragma once

#include "mob_attacks.hpp"

#include "ov/entity/world.hpp"
#include "ov/gameplay/food.hpp"
#include "ov/gameplay/villager_state.hpp"
#include "ov/math/random.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ov::server {

struct ZombieVillagerHost {
    /// Make a mob of this type here, with the server's own behaviour for it.
    /// `kNoEntity` when the type cannot be spawned.
    std::function<entity::EntityHandle(std::string_view type, Vec3d at)> create_mob;
    /// ── brains ── A cure finished: the villager's network id and the player
    /// who started it (the gossip it owes them). Null: nobody hears.
    std::function<void(i32 villager, i32 player)> on_cured;
    /// Tell every client about a mob just made.
    std::function<void(const entity::EntityState& state)> announce;
    /// The player's main hand, by registry name; empty for none.
    std::function<std::string_view(i32 player)> held;
    /// Creative players keep their apple.
    std::function<bool(i32 player)> creative;
    std::function<void(i32 player)> consume_held;
    /// Is this block an iron bar or a bed: what speeds a cure.
    std::function<bool(BlockPos pos)> speeds_cure;
};

struct ZombieVillagerStats {
    usize risen{0};
    usize cures_started{0};
    usize cured{0};
};

class ZombieVillagers {
public:
    explicit ZombieVillagers(const registry::Registries& registries);

    [[nodiscard]] bool owns(i32 type) const noexcept {
        return type == zombie_villager_type_ && type >= 0;
    }

    /// A villager one of the zombie family killed. True when it rose as a
    /// zombie villager — the caller then does not tell a death.
    bool on_villager_killed(entity::EntityWorld& world, const MobKill& kill,
                            gameplay::Difficulty difficulty, const ZombieVillagerHost& host);

    /// A right-click on an entity, from the network thread.
    void queue_interact(i32 player, i32 entity);

    /// A splash potion broke: zombie villagers in its reach take its Weakness
    /// (the box of four around the impact, intensity `1 − d/4`, as brewing
    /// gives a player). `weakness_ticks` is the potion's own duration, 0 when
    /// it carries no Weakness.
    void on_splash(const entity::EntityWorld& world, Vec3d at, i32 weakness_ticks);

    /// One tick: the clicks, the Weakness running down, the cures.
    ZombieVillagerStats tick(entity::EntityWorld& world, const ZombieVillagerHost& host,
                             const AttackDeliver& deliver);

    /// Indices 19 (converting) and 20 (VillagerData), for the spawn packets.
    void spawn_metadata(const entity::EntityState& state, net::MetadataWriter& fields) const;

    /// What a zombie villager remembers, made on first use (the Anvil load).
    [[nodiscard]] gameplay::VillagerState* kept(i32 network_id);
    [[nodiscard]] i32 conversion_time(i32 network_id) const noexcept;
    void set_conversion_time(i32 network_id, i32 ticks);
    /// Weakness, for the tests and the end-to-end check: ticks left.
    void weaken(i32 network_id, i32 ticks) { records_[network_id].weakness = ticks; }

private:
    struct Record {
        gameplay::VillagerState villager{};
        /// `ConversionTime`: ticks left, -1 when not curing.
        i32 conversion{-1};
        /// Weakness ticks left. Mob effects are not modelled on this server;
        /// this one is, because the cure asks for it.
        i32 weakness{0};
        /// ── brains ── the player whose apple started the cure, -1: none
        i32 curer{-1};
    };

    void finish_cure(entity::EntityWorld& world, entity::EntityState& zombie, Record& record,
                     const ZombieVillagerHost& host);
    [[nodiscard]] i32 cure_speed(const entity::EntityState& zombie, const ZombieVillagerHost& host);

    i32 zombie_villager_type_{-1};
    i32 villager_type_{-1};

    std::unordered_map<i32, Record> records_;
    std::vector<i32>                gone_;

    std::mutex                       mutex_;
    std::vector<std::pair<i32, i32>> clicks_;
    std::vector<std::pair<i32, i32>> handling_;

    /// Fixed seed: two runs of the same world rise and cure the same way.
    math::LegacyRandomSource random_{0x2B1E'0000'C0DE'0001LL};
};

}  // namespace ov::server
