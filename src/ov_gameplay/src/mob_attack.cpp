// ── mobs-3 ── See mob_attack.hpp. The numbers are documentary (the wiki's
// *Difficulty*, *Husk*, *Cave Spider*, *Armor*) and each is confronted with a
// real server in docs/provenance/mobs-3.md.
#include "ov/gameplay/mob_attack.hpp"

#include <algorithm>
#include <array>

namespace ov::gameplay {

f32 scale_for_difficulty(f32 amount, Difficulty difficulty) noexcept {
    switch (difficulty) {
        case Difficulty::Peaceful:
            return 0.0F;
        case Difficulty::Easy:
            return std::min(amount / 2.0F + 1.0F, amount);
        case Difficulty::Normal:
            return amount;
        case Difficulty::Hard:
            return amount * 3.0F / 2.0F;
    }
    return amount;
}

f32 effective_regional_difficulty(Difficulty difficulty, i64 game_time, i64 inhabited_time,
                                  f32 moon) noexcept {
    if (difficulty == Difficulty::Peaceful) {
        return 0.0F;
    }
    const bool hard = difficulty == Difficulty::Hard;
    f32        f    = 0.75F;
    const f32  aged = std::clamp(static_cast<f32>(game_time - 72000) / 1440000.0F, 0.0F, 1.0F) *
                     0.25F;
    f += aged;
    f32 g = 0.0F;
    g += std::clamp(static_cast<f32>(inhabited_time) / 3600000.0F, 0.0F, 1.0F) *
         (hard ? 1.0F : 0.75F);
    g += std::clamp(moon * 0.25F, 0.0F, aged);
    if (difficulty == Difficulty::Easy) {
        g *= 0.5F;
    }
    f += g;
    return static_cast<f32>(static_cast<i32>(difficulty)) * f;
}

std::optional<HitEffect> melee_hit_effect(std::string_view attacker_type, Difficulty difficulty,
                                          f32 regional) noexcept {
    if (attacker_type == "minecraft:husk") {
        const i32 duration = 140 * static_cast<i32>(regional);
        if (duration <= 0) {
            return std::nullopt;
        }
        return HitEffect{Effect::Hunger, duration, 0};
    }
    if (attacker_type == "minecraft:cave_spider") {
        if (difficulty == Difficulty::Normal) {
            return HitEffect{Effect::Poison, 7 * 20, 0};
        }
        if (difficulty == Difficulty::Hard) {
            return HitEffect{Effect::Poison, 15 * 20, 0};
        }
        return std::nullopt;
    }
    if (attacker_type == "minecraft:wither_skeleton") {
        return HitEffect{Effect::Wither, 10 * 20, 0};
    }
    return std::nullopt;
}

namespace {

struct ArmourRow {
    std::string_view item;
    ArmourPiece      piece;
};

// The wiki's Armor table. Toughness 2 for diamond, 3 and a tenth of knockback
// resistance for netherite.
constexpr std::array<ArmourRow, 25> kArmour{{
    {"minecraft:leather_helmet", {1, 0, 0}},
    {"minecraft:leather_chestplate", {3, 0, 0}},
    {"minecraft:leather_leggings", {2, 0, 0}},
    {"minecraft:leather_boots", {1, 0, 0}},
    {"minecraft:golden_helmet", {2, 0, 0}},
    {"minecraft:golden_chestplate", {5, 0, 0}},
    {"minecraft:golden_leggings", {3, 0, 0}},
    {"minecraft:golden_boots", {1, 0, 0}},
    {"minecraft:chainmail_helmet", {2, 0, 0}},
    {"minecraft:chainmail_chestplate", {5, 0, 0}},
    {"minecraft:chainmail_leggings", {4, 0, 0}},
    {"minecraft:chainmail_boots", {1, 0, 0}},
    {"minecraft:iron_helmet", {2, 0, 0}},
    {"minecraft:iron_chestplate", {6, 0, 0}},
    {"minecraft:iron_leggings", {5, 0, 0}},
    {"minecraft:iron_boots", {2, 0, 0}},
    {"minecraft:diamond_helmet", {3, 2, 0}},
    {"minecraft:diamond_chestplate", {8, 2, 0}},
    {"minecraft:diamond_leggings", {6, 2, 0}},
    {"minecraft:diamond_boots", {3, 2, 0}},
    {"minecraft:netherite_helmet", {3, 3, 0.1F}},
    {"minecraft:netherite_chestplate", {8, 3, 0.1F}},
    {"minecraft:netherite_leggings", {6, 3, 0.1F}},
    {"minecraft:netherite_boots", {3, 3, 0.1F}},
    {"minecraft:turtle_helmet", {2, 0, 0}},
}};

}  // namespace

std::optional<ArmourPiece> armour_piece(std::string_view item) noexcept {
    for (const ArmourRow& row : kArmour) {
        if (row.item == item) {
            return row.piece;
        }
    }
    return std::nullopt;
}

}  // namespace ov::gameplay
