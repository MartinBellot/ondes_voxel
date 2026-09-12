// A brain's memories: what a mob knows, each fact with an optional expiry.
//
// Vanilla's villagers, piglins, axolotls, frogs and the warden are not driven
// by goals but by a *brain*: a set of memories that sensors fill and
// behaviours read, activities picked by a schedule, and behaviours that start
// when the memories they need are there. This header is the first part — the
// memory store — kept free of every other gameplay header, because `MobBrain`
// (goals.hpp) holds one and the rest of the brain (brain.hpp) needs the goals.
//
// ── Shape ───────────────────────────────────────────────────────────────────
//
// One fixed slot per memory type: no map, no allocation, a memory is an index.
// A value is one of five kinds (nothing but presence, a number, a position in
// a dimension, an entity, a player). Each slot may carry a time to live, which
// `tick` counts down: at zero the memory is forgotten — vanilla's
// `ExpirableValue`, whose `ttl` is what the save writes.
//
// ── Provenance ──────────────────────────────────────────────────────────────
//
// The memory names and which of them a villager saves are the Minecraft Wiki's
// ("Brain" and "Villager" pages) and were then read off a real 1.20.1 server's
// `Brain.memories` (scripts/measure_villager_life.py `schedule`);
// docs/provenance/cerveaux.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace ov::gameplay::brain {

/// The memory types this server knows. Order is ours; the names are vanilla's.
enum class MemoryType : u8 {
    // ── Saved by a villager (and read back) ──
    Home,
    JobSite,
    PotentialJobSite,
    MeetingPoint,
    LastSlept,
    LastWoken,
    LastWorkedAtPoi,
    GolemDetectedRecently,
    // ── Never saved ──
    HurtBy,              ///< who hurt the mob: an entity or a player
    NearestHostile,      ///< entity
    NearestVisiblePlayer,  ///< player
    WalkTarget,          ///< position
    LookTarget,          ///< entity or position
    InteractionTarget,   ///< entity
    BreedTarget,         ///< entity
    NearestBed,          ///< position
    HeardBellTime,       ///< number
    CantReachWalkTargetSince,  ///< number
    Count
};
inline constexpr usize kMemoryCount = static_cast<usize>(MemoryType::Count);

/// What a memory holds.
enum class MemoryKind : u8 { Unit, Number, Pos, Entity, Player };

/// Dimensions a remembered position may be in, as `GlobalPos` names them.
enum class Dimension : u8 { Overworld, Nether, End };
[[nodiscard]] std::string_view dimension_name(Dimension dimension) noexcept;
[[nodiscard]] std::optional<Dimension> dimension_from_name(std::string_view name) noexcept;

struct MemoryValue {
    MemoryKind kind{MemoryKind::Unit};
    /// Number, or the entity's network id / the player's entity id.
    i64       number{0};
    BlockPos  pos{};
    Dimension dimension{Dimension::Overworld};
    /// A walk target's speed (blocks a tick) and how close is close enough.
    f32 speed{0.0F};
    i32 close_enough{0};

    [[nodiscard]] static constexpr MemoryValue unit() noexcept { return {}; }
    [[nodiscard]] static constexpr MemoryValue walk_to(BlockPos p, f32 speed,
                                                       i32 close_enough) noexcept {
        return {MemoryKind::Pos, 0, p, Dimension::Overworld, speed, close_enough};
    }
    [[nodiscard]] static constexpr MemoryValue of_number(i64 n) noexcept {
        return {MemoryKind::Number, n, {}, Dimension::Overworld};
    }
    [[nodiscard]] static constexpr MemoryValue of_pos(BlockPos p,
                                                      Dimension d = Dimension::Overworld) noexcept {
        return {MemoryKind::Pos, 0, p, d};
    }
    [[nodiscard]] static constexpr MemoryValue of_entity(i32 network_id) noexcept {
        return {MemoryKind::Entity, network_id, {}, Dimension::Overworld};
    }
    [[nodiscard]] static constexpr MemoryValue of_player(i32 entity_id) noexcept {
        return {MemoryKind::Player, entity_id, {}, Dimension::Overworld};
    }
    friend constexpr bool operator==(const MemoryValue&, const MemoryValue&) = default;
};

/// A memory type's vanilla name (`minecraft:home`), the kind it holds, and
/// whether a villager's save writes it.
struct MemoryInfo {
    std::string_view name;
    MemoryKind       kind{MemoryKind::Unit};
    bool             saved{false};
};
[[nodiscard]] const MemoryInfo& memory_info(MemoryType type) noexcept;
[[nodiscard]] std::optional<MemoryType> memory_from_name(std::string_view name) noexcept;

/// No expiry.
inline constexpr i64 kForever = -1;

class Memories {
public:
    [[nodiscard]] bool has(MemoryType type) const noexcept { return slot(type).present; }
    [[nodiscard]] const MemoryValue* get(MemoryType type) const noexcept {
        const Slot& s = slot(type);
        return s.present ? &s.value : nullptr;
    }
    [[nodiscard]] std::optional<BlockPos> pos(MemoryType type) const noexcept {
        const MemoryValue* v = get(type);
        return v != nullptr && v->kind == MemoryKind::Pos ? std::optional{v->pos} : std::nullopt;
    }
    [[nodiscard]] std::optional<i64> number(MemoryType type) const noexcept {
        const MemoryValue* v = get(type);
        return v != nullptr ? std::optional{v->number} : std::nullopt;
    }
    /// Ticks left, or kForever.
    [[nodiscard]] i64 ttl(MemoryType type) const noexcept { return slot(type).ttl; }

    void set(MemoryType type, MemoryValue value, i64 ttl = kForever) noexcept {
        Slot& s   = slot(type);
        s.present = true;
        s.value   = value;
        s.ttl     = ttl;
    }
    /// Set when there is a value, erase when there is none: vanilla's
    /// `setMemory(type, Optional)`.
    void set_or_erase(MemoryType type, std::optional<MemoryValue> value) noexcept {
        if (value) {
            set(type, *value);
        } else {
            erase(type);
        }
    }
    void erase(MemoryType type) noexcept { slot(type) = Slot{}; }
    void clear() noexcept { slots_ = {}; }

    /// One tick: every expiring memory loses a tick of life and is forgotten
    /// at zero — vanilla's `forgetOutdatedMemories`, the first step of a
    /// brain's tick.
    void tick() noexcept;

private:
    struct Slot {
        bool        present{false};
        MemoryValue value{};
        i64         ttl{kForever};
    };
    [[nodiscard]] Slot& slot(MemoryType type) noexcept {
        return slots_[static_cast<usize>(type)];
    }
    [[nodiscard]] const Slot& slot(MemoryType type) const noexcept {
        return slots_[static_cast<usize>(type)];
    }
    std::array<Slot, kMemoryCount> slots_{};
};

}  // namespace ov::gameplay::brain
