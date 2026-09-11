// The dragon fight, as far as this server takes it.
//
// What the fight is, from the Minecraft Wiki's "Ender Dragon", "End crystal"
// and "Exit portal" pages (Java 1.20), and what was measured of it against the
// real server (scripts/measure_end_portal.py, docs/provenance/end.md):
//
//   * the first player to arrive starts it: the exit portal is built, empty,
//     on top of the column at the origin; the dragon appears at (0, 128, 0)
//     with 200 health; an End crystal stands on each of the ten spikes;
//   * a boss bar — "Ender Dragon", pink, a plain bar, the boss music and the
//     world fog — shows the dragon's health to every player within 192 blocks
//     of (0, 128, 0);
//   * the nearest crystal within 32 blocks of the dragon's box heals it one
//     point every ten ticks; hitting a crystal destroys it;
//   * the dragon is hit through its eight parts — whose network ids follow its
//     own — the head at full damage, the others at damage / 4 + min(damage, 1);
//   * at zero health it dies over 200 ticks, then the exit portal opens, the
//     first kill leaves the egg on the pillar, and one of the twenty gateways
//     opens (`end_gateway_order`).
//
// What is **not** the game's, named:
//
//   * the dragon's flight: it circles the island at a fixed radius and height,
//     where the game's holding pattern follows a node graph, and it has none of
//     the strafing, charging, perching or breath phases;
//   * a destroyed crystal does not explode, and the dragon is not told to
//     change phase;
//   * lethal damage kills at once, where the game first flies the dragon to the
//     portal (the `dying` phase) — and no hit cooldown is applied;
//   * the experience (12 000 on the first kill, 500 after) is not dropped:
//     this server's orbs live in the overworld's list;
//   * the fight is not saved: level.dat keeps the fixed DragonFight the world
//     was created with, and a restart starts a new fight;
//   * the respawn with four crystals on the exit portal is not implemented.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/end_portal.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"
#include "ov/protocol/types.hpp"
#include "ov/world/level.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ov::server {

/// Boss Bar (clientbound 0x0B in protocol 763).
inline constexpr i32 kBossBarPacket = 0x0B;

/// The dragon's maximum health.
inline constexpr f32 kDragonMaxHealth = 200.0F;

/// Ticks the death takes before the exit portal opens.
inline constexpr i32 kDragonDeathTicks = 200;

struct EndFightHost {
    /// Send to one player's connection — and to decide who hears the fight,
    /// the list of players in the End with their positions.
    std::function<void(i32 id, std::span<const u8> payload)> broadcast;
    /// Reserve `count` consecutive entity ids and return the first.
    std::function<i32(i32 count)> reserve_entity_ids;
};

class EndFight {
public:
    /// `dragon_type` and `crystal_type` are the entity types' ids in
    /// `minecraft:entity_type` — Mojang's, resolved by name by the caller.
    EndFight(const gameplay::EndPortalRules& rules, i64 seed, i32 dragon_type, i32 crystal_type);

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool dragon_alive() const noexcept { return dragon_alive_; }
    [[nodiscard]] bool dragon_killed() const noexcept { return killed_; }
    [[nodiscard]] bool previously_killed() const noexcept { return previously_killed_; }
    [[nodiscard]] std::optional<BlockPos> portal() const noexcept { return portal_; }
    [[nodiscard]] f32 dragon_health() const noexcept { return health_; }
    [[nodiscard]] i32 dragon_id() const noexcept { return dragon_id_; }
    [[nodiscard]] Vec3d dragon_position() const noexcept { return dragon_; }
    [[nodiscard]] usize crystals_alive() const noexcept;
    [[nodiscard]] std::optional<BlockPos> last_gateway() const noexcept { return last_gateway_; }

    /// The first player is in the End. `origin_top` is the heightmap
    /// (`MOTION_BLOCKING_NO_LEAVES`) at (0, 0). Writes through `level`.
    void start(world::LevelWriter& level, i32 origin_top, const EndFightHost& host);

    /// Everything a player who now sees the fight must be sent: the dragon,
    /// the crystals, the boss bar.
    void show_to(const std::function<void(i32, std::span<const u8>)>& send) const;

    /// The boss bar's removal, for a player who no longer sees the fight.
    void hide_from(const std::function<void(i32, std::span<const u8>)>& send) const;

    /// Does a player at `position` see the fight? Within 192 blocks of
    /// (0, 128, 0), as the game's.
    [[nodiscard]] static bool within_range(Vec3d position) noexcept;

    /// One tick: the flight, the crystals' healing, the death.
    void tick(world::LevelWriter& level, const EndFightHost& host);

    /// A hit on one of the fight's entities. False for any id it does not own.
    bool hurt(i32 entity_id, f32 damage, const EndFightHost& host);

private:
    struct Crystal {
        i32       entity_id{0};
        net::Uuid uuid{};
        Vec3d     position{};
        bool      alive{true};
    };

    void set_health(f32 health, const EndFightHost& host);
    void finish_death(world::LevelWriter& level, const EndFightHost& host);
    [[nodiscard]] std::vector<u8> boss_bar_add() const;

    const gameplay::EndPortalRules* rules_;
    i64                             seed_;
    i32                             dragon_type_;
    i32                             crystal_type_;

    bool                    started_{false};
    bool                    dragon_alive_{false};
    bool                    killed_{false};
    bool                    previously_killed_{false};
    std::optional<BlockPos> portal_;
    std::optional<BlockPos> last_gateway_;
    usize                   gateways_opened_{0};

    i32       dragon_id_{0};
    net::Uuid dragon_uuid_{};
    net::Uuid bar_uuid_{};
    Vec3d     dragon_{0.0, 128.0, 0.0};
    f32       yaw_{0.0F};
    f64       angle_{0.0};
    f32       health_{kDragonMaxHealth};
    i32       death_time_{0};
    i64       ticks_{0};
    /// The crystal the dragon is drawing on, or -1.
    i32 nearest_{-1};

    std::array<Crystal, 10> crystals_{};
    /// The fight's own draws (which crystal is looked for when). Seeded from
    /// the world seed; not the game's stream, whose draws are the dragon's.
    math::LegacyRandomSource random_;
};

}  // namespace ov::server
