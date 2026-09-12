// ── mobs-4 ── See mob_body.hpp.
#include "ov/gameplay/mob_body.hpp"

#include <array>

namespace ov::gameplay {
namespace {

// minecraft.wiki, *Undead* and *Arthropod* (mob categories, Java 1.20.1).
// Zombie and skeleton (undead) and spider (arthropod) are the three measured
// by effets.md § 7; the rest are the wiki's lists.
constexpr std::array<std::string_view, 13> kUndead{
    "minecraft:zombie",          "minecraft:husk",           "minecraft:drowned",
    "minecraft:zombie_villager", "minecraft:zombified_piglin", "minecraft:zoglin",
    "minecraft:skeleton",        "minecraft:stray",          "minecraft:wither_skeleton",
    "minecraft:phantom",         "minecraft:wither",         "minecraft:skeleton_horse",
    "minecraft:zombie_horse",
};

constexpr std::array<std::string_view, 5> kArthropods{
    "minecraft:spider", "minecraft:cave_spider", "minecraft:silverfish", "minecraft:endermite",
    "minecraft:bee",
};

}  // namespace

EffectTarget::Body effect_body(std::string_view type_name) noexcept {
    for (const std::string_view name : kUndead) {
        if (name == type_name) {
            return EffectTarget::Body::Undead;
        }
    }
    for (const std::string_view name : kArthropods) {
        if (name == type_name) {
            return EffectTarget::Body::Arthropod;
        }
    }
    return EffectTarget::Body::Ordinary;
}

}  // namespace ov::gameplay
