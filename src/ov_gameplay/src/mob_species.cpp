// ── mobs-2 ── The species this server brings to life, and nothing about them
// that is not a number or a flag.
//
// Every `movement_speed` is the attribute measured on a real 1.20.1 server
// (data/vanilla/1.20.1/normalized/entities.json). Every modifier was read off
// the same server, goal by goal, by scripts/measure_mobs2.py: the cruise speed
// of each species strolling, panicking and chasing on bare grass, turned back
// into a modifier through the walk law `v = 2.15859 · s²` (walk_speed.hpp).
// docs/provenance/mobs-2.md § 1 has the table, and says which rows are fits
// rather than round goal modifiers (the rabbit hops, the fox leaps) and which
// modifiers are not measured at all (`follow_parent`, `avoid_sun`).
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/villager.hpp"  // ── villagers ──

#include <array>

namespace ov::gameplay {
namespace {

constexpr MobKind monster(std::string_view type, f64 attribute, f64 stroll, bool doors, bool sun) {
    MobKind kind{};
    kind.type_name      = type;
    kind.category       = MobCategory::Monster;
    kind.movement_speed = attribute;
    kind.stroll         = stroll;
    kind.chase          = 1.0;
    kind.opens_doors    = doors;
    kind.avoids_sun     = sun;
    kind.hostile        = true;
    return kind;
}

constexpr MobKind animal(std::string_view type, f64 attribute, f64 stroll, f64 panic, bool breeds) {
    MobKind kind{};
    kind.type_name      = type;
    kind.category       = MobCategory::Creature;
    kind.movement_speed = attribute;
    kind.stroll         = stroll;
    kind.panic          = panic;
    kind.panics         = panic > 0.0;
    kind.breeds         = breeds;
    return kind;
}

constexpr MobKind ranged(MobKind kind, f64 hold_at) {
    kind.hold_at = hold_at;
    kind.melee   = false;  // ── mobs-3 ── it shoots or throws; it does not swing
    return kind;
}

// ── mobs-3 ──
/// A mob whose attack is not a swing: it closes to `hold_at` and stops. The
/// creeper's 3 is the swell's start (primed_tnt.hpp, kCreeperSwellStart):
/// vanilla's creeper stops walking the moment it begins to swell.
constexpr MobKind no_swing(MobKind kind, f64 hold_at) {
    kind.hold_at = hold_at;
    kind.melee   = false;
    return kind;
}

/// The zombie family hunts villagers as well as players, and seeks at its
/// measured `follow_range` of 35.
constexpr MobKind villager_hunter(MobKind kind) {
    kind.hunts_villagers = true;
    kind.follow_range    = 35.0;
    return kind;
}

constexpr MobKind follow(MobKind kind, f64 range) {
    kind.follow_range = range;
    return kind;
}
// ── end mobs-3 ──

constexpr MobKind neutral(MobKind kind) {
    kind.hostile = false;
    return kind;
}

// ── nether-2 ── The stroll modifiers of the Nether's walkers, as measured on
// bare grass (scripts/measure_nether_mobs.py stroll, nether-2.md § 3.1):
// piglin and brute 0.09517 b/t (0.6 of 0.35), hoglin and zoglin 0.03108 (0.4 of
// 0.3), zombified piglin 0.11414 (1.0 of 0.23), wither skeleton 0.13485 (1.0 of
// 0.25), strider on lava 0.06610 (1.0 of 0.175) and cold on grass 0.02880
// (0.66). The blaze hovers and the magma cube hops: neither cruise is a walk,
// both stay at 1.0 through the walker and are named.
struct NetherStroll {
    f64 piglin{0.6};
    f64 piglin_brute{0.6};
    f64 hoglin{0.4};
    f64 zoglin{0.4};
    f64 zombified_piglin{1.0};
    f64 wither_skeleton{1.0};
    f64 blaze{1.0};
    f64 magma_cube{1.0};
    f64 strider{1.0};
    f64 strider_cold{0.66};
};
constexpr NetherStroll kNetherStroll{};

//                         type                    attribute  stroll   doors  sun
constexpr MobKind creature(MobKind kind) {
    kind.category = MobCategory::Creature;
    return kind;
}

constexpr std::array<MobKind, 30> kAll{{
    // ── nether-2 ── The Nether's walkers. Attributes measured
    // (normalized/entities.json); stroll modifiers read off a real server by
    // scripts/measure_nether_mobs.py, docs/provenance/nether-2.md § 3.1. The
    // ghast is not here: it floats (GhastFlight, nether_mobs.hpp).
    monster("minecraft:piglin", 0.35, kNetherStroll.piglin, true, false),
    monster("minecraft:piglin_brute", 0.35, kNetherStroll.piglin_brute, true, false),
    monster("minecraft:hoglin", 0.3, kNetherStroll.hoglin, false, false),
    monster("minecraft:zoglin", 0.3, kNetherStroll.zoglin, false, false),
    // Hostile for its goals, but it hunts nobody until angered: the Nether's
    // session gives it no quarry and sets its target when it is hit.
    monster("minecraft:zombified_piglin", 0.23, kNetherStroll.zombified_piglin, true, false),
    monster("minecraft:wither_skeleton", 0.25, kNetherStroll.wither_skeleton, false, false),
    monster("minecraft:blaze", 0.23, kNetherStroll.blaze, false, false),
    monster("minecraft:magma_cube", 0.3, kNetherStroll.magma_cube, false, false),
    creature(neutral(monster("minecraft:strider", 0.175, kNetherStroll.strider, false, false))),
    // ── end nether-2 ──

    // The eight of M2. Stroll measured: zombie 0.11417 b/t, skeleton 0.13488,
    // creeper 0.08633 (modifier 0.8), spider 0.12429 (0.8).
    villager_hunter(monster("minecraft:zombie", 0.23, 1.0, true, true)),  // ── mobs-3 ──
    ranged(monster("minecraft:skeleton", 0.25, 1.0, false, true), 15.0),
    no_swing(monster("minecraft:creeper", 0.25, 0.8, false, false), 3.0),  // ── mobs-3 ──
    monster("minecraft:spider", 0.3, 0.8, false, false),
    // Stroll 1.0 for all four (cow 0.08632, pig 0.13482, sheep 0.11415,
    // chicken 0.13485); panic cow 2.0, pig 1.25, sheep 1.25, chicken 1.4.
    animal("minecraft:cow", 0.2, 1.0, 2.0, true),
    animal("minecraft:pig", 0.25, 1.0, 1.25, true),
    animal("minecraft:sheep", 0.23, 1.0, 1.25, true),
    animal("minecraft:chicken", 0.25, 1.0, 1.4, true),

    // New species. Husk and drowned stroll exactly as a zombie (0.11417,
    // 0.11416), stray and witch as a skeleton (0.13485, 0.13484), the cave
    // spider as a spider (0.12426).
    villager_hunter(monster("minecraft:husk", 0.23, 1.0, true, false)),  // ── mobs-3 ──
    ranged(monster("minecraft:stray", 0.25, 1.0, false, true), 15.0),
    follow(monster("minecraft:drowned", 0.23, 1.0, false, true), 35.0),  // ── mobs-3 ──
    monster("minecraft:cave_spider", 0.3, 0.8, false, false),
    ranged(monster("minecraft:witch", 0.25, 1.0, false, false), 10.0),
    // Neutral: strolls at 1.0 (0.19412) and attacks only when provoked, which
    // this server does not model yet — named in mobs-2.md.
    follow(neutral(monster("minecraft:enderman", 0.3, 1.0, false, false)), 64.0),  // ── mobs-3 ──
    // Hops rather than walks. Not measured; the walker moves it, named.
    // ── mobs-3 ── It hurts by contact, which is not a swing: not done.
    no_swing(monster("minecraft:slime", 0.3, 1.0, false, false), 0.0),
    // ── mobs-3 ── A zombie in every way this server models: the attribute
    // (0.23, entities.json), the sun, the doors; it keeps what the villager
    // was (zombification, mobs-3.md § 4).
    villager_hunter(monster("minecraft:zombie_villager", 0.23, 1.0, true, true)),

    // Rabbit: a hop, not a walk — 0.13175 b/t strolling and 0.380 panicking
    // are fitted through the law (0.824, 1.40), not goal modifiers.
    animal("minecraft:rabbit", 0.3, 0.824, 1.40, false),
    // Wolf strolls at 1.0 (0.19416) and does not panic.
    animal("minecraft:wolf", 0.3, 1.0, 0.0, false),
    // Fox strolls at 1.0 (0.19392); its panic leaps, 0.859 b/t, fitted 2.1.
    animal("minecraft:fox", 0.3, 1.0, 2.1, false),
    // Cat strolls at 0.8 (0.12432) and flees at 1.5 (0.429).
    animal("minecraft:cat", 0.3, 0.8, 1.5, false),
    // Horse strolls at 0.7 (0.05354) and panics at 1.2 (0.157).
    animal("minecraft:horse", 0.225, 0.7, 1.2, false),
}};

}  // namespace

// ── nether-2 ── A strider out of lava: the same, at its cold pace.
const MobKind& strider_cold_kind() noexcept {
    static constexpr MobKind kCold = creature(
        neutral(monster("minecraft:strider", 0.175, kNetherStroll.strider_cold, false, false)));
    return kCold;
}

std::span<const MobKind> mob_kinds() noexcept {
    return kAll;
}

const MobKind* mob_kind(std::string_view type_name) noexcept {
    for (const MobKind& kind : kAll) {
        if (kind.type_name == type_name) {
            return &kind;
        }
    }
    // ── villagers ── a table of its own, in villager.cpp
    return villager_mob_kind(type_name);
}

}  // namespace ov::gameplay
