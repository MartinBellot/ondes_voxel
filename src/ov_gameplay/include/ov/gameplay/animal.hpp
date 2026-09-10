// What a farm animal carries that a zombie does not: an age, a love timer, a
// fleece, a saddle, an egg on the way.
//
// Plain structs, and deliberately in a header of their own: `MobBrain`
// (goals.hpp) holds one, and the rules that act on it (breeding.hpp) need the
// goals — a header each way would be a cycle. Every number that gives these
// fields a meaning is measured and lives in breeding.hpp with its provenance;
// see docs/provenance/elevage.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"

#include <string_view>

namespace ov::gameplay {

/// The husbandry half of a mob. Zero everywhere is an adult with nothing on.
struct AnimalState {
    /// Vanilla's `Age`: negative while young, counting up to 0; positive while
    /// a parent cools down, counting down to 0. Measured: one per tick, NoAI
    /// or not.
    i32 age{0};
    /// Ticks of love left (`InLove`). Measured: 600 when fed.
    i32 love{0};
    /// The player whose food started the love, for the statistic. -1: none
    /// (a summoned `InLove` animal breeds just the same — measured).
    i32 love_cause{-1};

    /// Sheep only. Wool colour 0..15 in the dye order, and the shorn flag.
    i8   colour{0};
    bool sheared{false};
    /// Pig only.
    bool saddled{false};
    /// Chicken only: ticks until the next egg (`EggLayTime`).
    i32 egg_time{0};

    [[nodiscard]] constexpr bool baby() const noexcept { return age < 0; }
    [[nodiscard]] constexpr bool in_love() const noexcept { return love > 0; }
};

/// A player as a tempted animal sees one: where, and what is in each hand.
///
/// The names are registry names (`minecraft:wheat`), borrowed from the item
/// registry the caller holds — which outlives the tick they are used in.
struct Tempter {
    Vec3d            position{};
    std::string_view main_hand;
    std::string_view off_hand;
};

/// Something an animal did this tick that only the caller can finish: a birth
/// needs a spawn, a laid egg an item on the ground, an eaten tuft a block write.
enum class AnimalEventKind : u8 {
    /// `self` and `other` bred; the child goes at `at`.
    Birth,
    /// `self` stopped being a baby this tick. Its box is already the adult's.
    GrewUp,
    /// `self` (a chicken) laid an egg at `at`.
    LaidEgg,
    /// `self` (a sheep) ate: `block` is the grass block below it or the tuft
    /// at its feet, which the caller turns into dirt or air.
    AteGrass,
    /// `self` (a sheep) put its head down to eat: forty ticks from now.
    GrazeStart,
};

struct AnimalEvent {
    AnimalEventKind      kind{AnimalEventKind::Birth};
    entity::EntityHandle self{entity::kNoEntity};
    entity::EntityHandle other{entity::kNoEntity};
    Vec3d                at{};
    BlockPos             block{};
    /// For AteGrass: true when `block` is a grass block (becomes dirt), false
    /// when it is a tuft of grass at the feet (becomes air).
    bool grass_block{false};
};

}  // namespace ov::gameplay
