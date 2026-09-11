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
    return kind;
}

constexpr MobKind neutral(MobKind kind) {
    kind.hostile = false;
    return kind;
}

//                         type                    attribute  stroll   doors  sun
constexpr std::array<MobKind, 20> kAll{{
    // The eight of M2. Stroll measured: zombie 0.11417 b/t, skeleton 0.13488,
    // creeper 0.08633 (modifier 0.8), spider 0.12429 (0.8).
    monster("minecraft:zombie", 0.23, 1.0, true, true),
    ranged(monster("minecraft:skeleton", 0.25, 1.0, false, true), 15.0),
    monster("minecraft:creeper", 0.25, 0.8, false, false),
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
    monster("minecraft:husk", 0.23, 1.0, true, false),
    ranged(monster("minecraft:stray", 0.25, 1.0, false, true), 15.0),
    monster("minecraft:drowned", 0.23, 1.0, false, true),
    monster("minecraft:cave_spider", 0.3, 0.8, false, false),
    ranged(monster("minecraft:witch", 0.25, 1.0, false, false), 10.0),
    // Neutral: strolls at 1.0 (0.19412) and attacks only when provoked, which
    // this server does not model yet — named in mobs-2.md.
    neutral(monster("minecraft:enderman", 0.3, 1.0, false, false)),
    // Hops rather than walks. Not measured; the walker moves it, named.
    monster("minecraft:slime", 0.3, 1.0, false, false),

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
