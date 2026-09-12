#include "ov/gameplay/brain/memory.hpp"

namespace ov::gameplay::brain {
namespace {

// Vanilla's names. The first eight are what a 1.20.1 villager writes under
// `Brain.memories` (read off a real server: measure_villager_life.py
// `schedule`); the rest live only in memory.
constexpr std::array<MemoryInfo, kMemoryCount> kInfo{{
    {"minecraft:home", MemoryKind::Pos, true},
    {"minecraft:job_site", MemoryKind::Pos, true},
    {"minecraft:potential_job_site", MemoryKind::Pos, true},
    {"minecraft:meeting_point", MemoryKind::Pos, true},
    {"minecraft:last_slept", MemoryKind::Number, true},
    {"minecraft:last_woken", MemoryKind::Number, true},
    {"minecraft:last_worked_at_poi", MemoryKind::Number, true},
    {"minecraft:golem_detected_recently", MemoryKind::Unit, true},
    {"minecraft:hurt_by", MemoryKind::Entity, false},
    {"minecraft:nearest_hostile", MemoryKind::Entity, false},
    {"minecraft:nearest_visible_player", MemoryKind::Player, false},
    {"minecraft:walk_target", MemoryKind::Pos, false},
    {"minecraft:look_target", MemoryKind::Entity, false},
    {"minecraft:interaction_target", MemoryKind::Entity, false},
    {"minecraft:breed_target", MemoryKind::Entity, false},
    {"minecraft:nearest_bed", MemoryKind::Pos, false},
    {"minecraft:heard_bell_time", MemoryKind::Number, false},
    {"minecraft:cant_reach_walk_target_since", MemoryKind::Number, false},
}};

constexpr std::array<std::string_view, 3> kDimensions{
    "minecraft:overworld", "minecraft:the_nether", "minecraft:the_end"};

}  // namespace

std::string_view dimension_name(Dimension dimension) noexcept {
    return kDimensions[static_cast<usize>(dimension)];
}

std::optional<Dimension> dimension_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kDimensions.size(); ++i) {
        if (kDimensions[i] == name) {
            return static_cast<Dimension>(i);
        }
    }
    return std::nullopt;
}

const MemoryInfo& memory_info(MemoryType type) noexcept {
    return kInfo[static_cast<usize>(type)];
}

std::optional<MemoryType> memory_from_name(std::string_view name) noexcept {
    for (usize i = 0; i < kInfo.size(); ++i) {
        if (kInfo[i].name == name) {
            return static_cast<MemoryType>(i);
        }
    }
    return std::nullopt;
}

void Memories::tick() noexcept {
    for (Slot& s : slots_) {
        if (!s.present || s.ttl == kForever) {
            continue;
        }
        // Vanilla: the time to live drops, and a memory at or under zero is
        // forgotten on the same pass.
        if (--s.ttl <= 0) {
            s = Slot{};
        }
    }
}

}  // namespace ov::gameplay::brain
