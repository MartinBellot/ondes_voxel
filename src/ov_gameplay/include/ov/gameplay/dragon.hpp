// The ender dragon: its node graph, its phases, its flight, its damage rules.
//
// Written from documentation — the Minecraft Wiki's "Ender Dragon" and "End
// crystal" pages (Java 1.20) and the speedrunning community's "Dragon fight
// mechanics" page (mcsr.miraheze.org), which describes each phase, the node
// rings and the path rules in prose — and fitted against the real server's
// dragon (scripts/measure_dragon.py, docs/provenance/dragon.md). No game code
// was read or translated; where the documentation stops, the constant is
// fitted to the measured flight and says so.
//
// Nothing here takes a server, a socket or a chunk map. A dragon is a point
// with a heading, a phase and a random source; what it asks of the world (the
// players it may target, the crystal count, the fountain's height) comes in a
// `DragonSurroundings`, and what it does to the world (a fireball, a breath
// cloud, experience) goes out in a `DragonOutput`. That is what lets a test fly
// a dragon for ten thousand ticks without a level.
//
// ── What is the game's, and what is not ─────────────────────────────────────
//
//   * the phases, their numbers (the `DragonPhase` metadata, index 16), their
//     transitions and their odds are the documented ones;
//   * the 24 nodes: 12 on a ring of radius 60, 8 on radius 40, 4 on radius 20,
//     heights max(73, top + 5 / 15 / 5); the outer ring is disabled with no
//     crystal left. The **links** between nodes are not documented in any
//     source read: they are rebuilt here from geometry (each node to its ring
//     neighbours and to the nearest nodes of the adjacent rings), named;
//   * the flight integrator (thrust, drag, turn rate) is fitted to the measured
//     trajectory, not taken from a description of the game's;
//   * the vision checks (strafe, sitting scanning) are taken as passing: the
//     game caches its first line-of-sight answer for the whole load, and an
//     arena with the probe in the open passes it;
//   * the parts' boxes are laid out from the documented overall size (16 wide,
//     8 tall, eight parts) and not measured.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/aabb.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace ov::gameplay {

/// The dragon's phases, numbered as the `DragonPhase` NBT tag and the metadata
/// (index 16) number them.
enum class DragonPhase : i32 {
    HoldingPattern   = 0,
    StrafePlayer     = 1,
    LandingApproach  = 2,
    Landing          = 3,
    Takeoff          = 4,
    SittingFlaming   = 5,
    SittingScanning  = 6,
    SittingAttacking = 7,
    ChargingPlayer   = 8,
    Dying            = 9,
    Hover            = 10,
};

[[nodiscard]] std::string_view dragon_phase_name(DragonPhase phase) noexcept;

/// Perched on the fountain: scanning, roaring or breathing.
[[nodiscard]] bool dragon_sitting(DragonPhase phase) noexcept;

// ── The node graph ──────────────────────────────────────────────────────────

inline constexpr usize kDragonNodeCount  = 24;
inline constexpr usize kDragonOuterNodes = 12;

/// The nodes the dragon paths between.
class DragonGraph {
public:
    DragonGraph() noexcept;

    /// Set each node's height from the column it stands in: `top(x, z)` is the
    /// first free block above the highest motion-blocking block (leaves
    /// excepted). The game does this the first time the dragon makes a path.
    void place(const std::function<i32(i32 x, i32 z)>& top);

    [[nodiscard]] bool placed() const noexcept { return placed_; }

    [[nodiscard]] BlockPos node(usize index) const noexcept { return nodes_[index]; }

    /// The outer ring is enabled only while a crystal is left.
    [[nodiscard]] static bool enabled(usize index, i32 crystals) noexcept {
        return index >= kDragonOuterNodes || crystals > 0;
    }

    /// Bit j set: node `index` leads to node j.
    [[nodiscard]] u32 links(usize index) const noexcept { return links_[index]; }

    /// The enabled node closest to the block containing `at`; 0 when none is
    /// within 100 blocks.
    [[nodiscard]] usize closest(Vec3d at, i32 crystals) const noexcept;

    /// The enabled node closest to node `index` (itself when enabled).
    [[nodiscard]] usize closest_enabled(usize index, i32 crystals) const noexcept;

    /// The shortest path from `from` to `to` — to the enabled node closest to
    /// it when it is disabled — over enabled nodes, written into `out` from
    /// `from` onwards. Returns its length; 0 when there is none (or when `to`
    /// is disabled and `from` is already the closest enabled node to it).
    usize find_path(usize from, usize to, i32 crystals, std::span<u8> out) const noexcept;

private:
    std::array<BlockPos, kDragonNodeCount> nodes_{};
    std::array<u32, kDragonNodeCount>      links_{};
    bool                                   placed_{false};
};

// ── The dragon ──────────────────────────────────────────────────────────────

/// A player the dragon may target: alive, in survival or adventure.
struct DragonPlayer {
    i32   id{0};
    Vec3d feet{};
    f64   eye_height{1.62};
};

/// What the dragon reads of the world, once per tick.
struct DragonSurroundings {
    /// The fight's crystal count (it is not the number alive: see the fight).
    i32 crystals{0};
    /// Targetable players (alive, survival or adventure), in the End.
    std::span<const DragonPlayer> players{};
    /// The first free block of the column at (0, 0): the fountain's top.
    i32 fountain_top{64};
    /// Peaceful: the dragon never breathes and always takes off.
    bool peaceful{false};
    /// Line of sight between two points (blocks only). Empty: always true.
    /// Measured to matter: a perched dragon did not charge a probe 100 blocks
    /// away and 15 below, behind the island's rim — it took off.
    std::function<bool(Vec3d from, Vec3d to)> sees;
};

/// What one tick asks the caller to carry out.
struct DragonOutput {
    /// A dragon fireball, from `fireball_from` along `fireball_direction`.
    bool  fireball{false};
    Vec3d fireball_from{};
    Vec3d fireball_direction{};
    /// Breath: a cloud of radius 5 at the first non-air block under
    /// `breath_from` (2.5 blocks ahead of the head), gone when the flaming ends.
    bool  breath_start{false};
    Vec3d breath_from{};
    bool  breath_stop{false};
    /// Roar before the breath (the `growl` sound).
    bool roar{false};
    /// The phase changed this tick (its metadata must be sent).
    bool phase_changed{false};
    /// Experience to drop at the dragon's position this tick.
    i32 experience{0};
    /// The death animation is over: remove the dragon, open the portal.
    bool dead{false};
};

/// The eight parts, in the order their network ids follow the dragon's.
enum class DragonPart : u8 { Head, Neck, Body, Tail1, Tail2, Tail3, Wing1, Wing2 };
inline constexpr usize kDragonPartCount = 8;

/// Where a hit comes from, as far as the dragon cares.
enum class DragonHurtSource : u8 {
    /// A player's melee hit.
    Player,
    /// An arrow or trident a player shot: bounces off a perched dragon.
    PlayerProjectile,
    /// Any explosion (a crystal's included).
    Explosion,
    /// `/damage`-like: the command path. Always lands.
    Command,
    /// Anything else — fire, fall, a mob: the dragon is immune.
    Other,
};

/// The flight's constants. Fitted to the measured trajectory
/// (docs/provenance/dragon.md § 3), except where the documentation gives them.
struct DragonFlight {
    /// Forward acceleration per tick, blocks per tick². With `drag` it sets
    /// the cruising speed: 1.0 block a tick; measured holding pattern, 10-tick
    /// windows: p50 0.89, p90 1.14.
    f64 thrust{0.1};
    /// Horizontal velocity kept per tick.
    f64 drag{0.9};
    /// The most the heading turns in one tick, in degrees.
    f32 turn{5.0F};
    /// The most the vertical velocity changes in one tick (documented: 0.015
    /// landing, 0.03 charging and dying; the default is fitted: holding
    /// pattern vertical speed p10 −0.058, p90 0.055).
    f64 climb{0.006};
    f64 climb_landing{0.015};
    f64 climb_fast{0.03};
    /// Vertical velocity kept per tick.
    f64 vertical_drag{0.9};
    /// The landing turns faster (the documented "snap").
    f32 turn_landing{20.0F};
};

/// Experience for the first dragon of a world, and for any after.
inline constexpr i32 kDragonFirstExperience = 12000;
inline constexpr i32 kDragonLaterExperience = 500;

/// Ticks of the death animation.
inline constexpr i32 kDragonDeathAnimation = 200;

/// Sitting damage that ends a perch.
inline constexpr f32 kDragonSittingDamageLimit = 50.0F;

/// Damage the dragon takes when the crystal healing it is destroyed.
inline constexpr f32 kDragonCrystalLoss = 10.0F;

class Dragon {
public:
    Dragon(Vec3d spawn, f32 yaw, f32 health, bool previously_killed,
           DragonFlight flight = {}) noexcept;

    [[nodiscard]] Vec3d       position() const noexcept { return position_; }
    [[nodiscard]] Vec3d       velocity() const noexcept { return velocity_; }
    /// The heading: the direction the dragon travels.
    [[nodiscard]] f32         yaw() const noexcept { return yaw_; }
    /// The yaw the wire carries: the heading turned half round. Measured: the
    /// real server's yaw differs from the direction of travel by 160 to 200
    /// degrees in every phase that moves.
    [[nodiscard]] f32 wire_yaw() const noexcept;
    [[nodiscard]] f32         pitch() const noexcept { return pitch_; }
    [[nodiscard]] f32         health() const noexcept { return health_; }
    [[nodiscard]] DragonPhase phase() const noexcept { return phase_; }
    [[nodiscard]] i32         death_time() const noexcept { return death_time_; }
    [[nodiscard]] i32         phase_ticks() const noexcept { return phase_ticks_; }
    [[nodiscard]] std::optional<Vec3d> target() const noexcept { return target_; }
    [[nodiscard]] const DragonGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] DragonGraph&       graph() noexcept { return graph_; }
    /// Ticks since the last hit landed (the wings spare everyone for ten).
    [[nodiscard]] i32 since_hurt() const noexcept { return since_hurt_; }
    /// Sitting damage received during this perch.
    [[nodiscard]] f32 sitting_damage() const noexcept { return sitting_damage_; }

    /// The attack target of the strafe (a player id), if any.
    [[nodiscard]] std::optional<i32> attack_target() const noexcept { return attack_target_; }

    void set_phase(DragonPhase phase) noexcept;
    void set_health(f32 health) noexcept { health_ = health; }
    /// Blocks the dragon's body met this tick that it cannot break: the game
    /// slows a dragon in a wall.
    void set_in_wall(bool in_wall) noexcept { in_wall_ = in_wall; }

    /// One tick.
    void tick(const DragonSurroundings& world, math::LegacyRandomSource& random,
              DragonOutput& out);

    /// A hit on one part. Returns the health taken (0 when refused).
    f32 hurt(DragonPart part, f32 damage, DragonHurtSource source) noexcept;

    /// A crystal was destroyed. `player` is who the dragon strafes: the
    /// survival player who destroyed it, or the one nearest to it; nothing
    /// when there is none (or the destroyer was in creative).
    void crystal_destroyed(std::optional<i32> player) noexcept;

    /// The crystal healing it is gone: 10 damage, as an explosion on the head.
    void lose_crystal() noexcept;

    /// One point from the crystal drawing on it.
    void heal(f32 amount) noexcept;

    /// The whole box: 16 wide, 8 tall, round the position.
    [[nodiscard]] AABB box() const noexcept;

    /// The parts' boxes, in `DragonPart` order.
    [[nodiscard]] std::array<AABB, kDragonPartCount> parts() const noexcept;

    /// Total experience the death animation drops.
    [[nodiscard]] i32 experience() const noexcept {
        return previously_killed_ ? kDragonLaterExperience : kDragonFirstExperience;
    }

private:
    void tick_phase(const DragonSurroundings& world, math::LegacyRandomSource& random,
                    DragonOutput& out);
    void fly(const DragonSurroundings& world);
    void holding_pattern(const DragonSurroundings& world, math::LegacyRandomSource& random);
    void strafe(const DragonSurroundings& world, math::LegacyRandomSource& random,
                DragonOutput& out);
    void landing_approach(const DragonSurroundings& world, math::LegacyRandomSource& random);
    void takeoff(const DragonSurroundings& world, math::LegacyRandomSource& random);
    void sitting_scanning(const DragonSurroundings& world);
    void charging();
    void dying(const DragonSurroundings& world, DragonOutput& out);

    /// A new path in the current direction round the rings.
    void circle_path(const DragonSurroundings& world, math::LegacyRandomSource& random,
                     bool map_inner);
    /// Aim at the next node of the path, somewhere up to 20 blocks above it.
    void next_waypoint(math::LegacyRandomSource& random);
    [[nodiscard]] bool path_done() const noexcept { return path_index_ >= path_length_; }
    [[nodiscard]] bool needs_target() const noexcept;
    [[nodiscard]] Vec3d fountain(const DragonSurroundings& world) const noexcept;
    [[nodiscard]] const DragonPlayer* player(const DragonSurroundings& world, i32 id) const;
    [[nodiscard]] Vec3d head_centre() const noexcept;
    [[nodiscard]] f32 bearing_to(Vec3d point) const noexcept;

    DragonFlight flight_;
    DragonGraph  graph_;
    Vec3d        position_{};
    Vec3d        velocity_{};
    f32          yaw_{0.0F};
    f32          pitch_{0.0F};
    f32          health_{200.0F};
    bool         previously_killed_{false};
    bool         in_wall_{false};

    DragonPhase           phase_{DragonPhase::HoldingPattern};
    i32                   phase_ticks_{0};
    bool                  phase_entered_{true};
    /// A phase change (or the end of a breath) from outside the tick — a hit,
    /// a crystal — reported by the next tick's output.
    bool                  phase_dirty_{false};
    bool                  breath_pending_{false};
    std::optional<Vec3d>  target_;
    std::array<u8, kDragonNodeCount + 1> path_{};
    usize                 path_length_{0};
    usize                 path_index_{0};
    /// The holding pattern's and the strafe's own directions round the ring.
    bool                  clockwise_{false};
    bool                  strafe_clockwise_{false};
    std::optional<i32>    attack_target_;
    i32                   fireball_charge_{0};
    i32                   flame_count_{0};
    i32                   time_since_charge_{0};
    f32                   sitting_damage_{0.0F};
    i32                   death_time_{0};
    i32                   since_hurt_{1000};
    /// The living entity's hurt cooldown: a hit in the next ten ticks takes
    /// only what exceeds the last one.
    i32                   invulnerable_{0};
    f32                   last_hurt_{0.0F};
};

}  // namespace ov::gameplay
