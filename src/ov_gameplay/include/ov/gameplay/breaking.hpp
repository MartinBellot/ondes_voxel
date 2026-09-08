// How long a block takes to break, and whether it drops anything.
//
// Vanilla's rule is short but every term in it matters, and none of them are in
// Mojang's generated reports — they are Java code. What is written here was
// checked against a real 1.20.1 server by timing the server itself: it does not
// break a block on its own but waits for the client to claim it is done, and a
// claim made too early falls below the 0.7 threshold, at which point the server
// falls back to its own clock and breaks the block on the exact tick vanilla
// would. Counting that tick measures the rule without knowing it.
//
// Three results from that measurement are worth stating, because none is
// guessable:
//
//   * The tool's **family** gates harvesting, not only its tier. A netherite
//     shovel on stone takes the full 150 ticks a bare hand does; a wooden
//     pickaxe takes 23.
//   * Tier speeds are 2, 4, 6, 8 and 9 for wood, stone, iron, diamond and
//     netherite — each the only integer compatible with the ticks counted.
//   * Efficiency adds exactly `level * level + 1`, and only to a tool that was
//     already faster than a bare hand.
#pragma once

#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <optional>
#include <vector>

namespace ov::gameplay {

/// A tool tier, ordered the way the `needs_*_tool` tags order it.
enum class Tier : u8 { Wood = 0, Gold = 0, Stone = 1, Iron = 2, Diamond = 3, Netherite = 4 };

/// The families a digging tool can belong to. A tool only speeds up — and only
/// harvests — the blocks its own family covers.
enum class Family : u8 { None, Pickaxe, Axe, Shovel, Hoe, Sword, Shears };

/// What the player is holding, as far as breaking is concerned.
struct Held {
    /// The item's wire id, or nothing for an empty hand.
    std::optional<registry::ProtocolId> item;
    u8                                  efficiency{0};
};

/// How the player stands, which vanilla lets change the answer fivefold.
struct Stance {
    bool on_ground{true};
    bool head_in_water{false};
    bool aqua_affinity{false};
    /// Effect amplifier, or -1 for no effect. Amplifier 0 is level I.
    i8 haste{-1};
    i8 mining_fatigue{-1};
};

/// The breaking rule, with the registry lookups it needs resolved once.
///
/// Built per world rather than per break: every query would otherwise walk the
/// tag graph, and breaking a block is on the path of every player action.
class BreakRules {
public:
    BreakRules(const registry::BlockRegistry& blocks, const registry::Registries& registries);

    /// The family and tier of an item, if it is a digging tool at all.
    [[nodiscard]] Family family_of(registry::ProtocolId item) const noexcept;
    [[nodiscard]] Tier   tier_of(registry::ProtocolId item) const noexcept;

    /// Does this tool let the block drop what it holds?
    ///
    /// True for every block that does not demand a tool — which is most of
    /// them — and otherwise only for the right family at a sufficient tier.
    [[nodiscard]] bool can_harvest(registry::BlockStateId state, const Held& held) const noexcept;

    /// Vanilla's destroy speed: 1 for a bare hand, the tier's speed for the
    /// right family, then the enchantment, the effects, water and the ground.
    [[nodiscard]] f32 destroy_speed(registry::BlockStateId state, const Held& held,
                                    const Stance& stance) const noexcept;

    /// The fraction of a block broken per tick.
    ///
    /// The server counts with this rather than with the tick total, because
    /// vanilla's own shortcut is written in terms of it: a client that claims
    /// to be finished is believed once the fraction times the elapsed ticks
    /// reaches 0.7, and left to the server's clock below that.
    [[nodiscard]] f32 destroy_progress(registry::BlockStateId state, const Held& held,
                                       const Stance& stance) const noexcept;

    /// Ticks to break, or -1 for a block that never breaks.
    ///
    /// This is the number the server counts down; the client runs the same
    /// arithmetic to animate the cracks, so a difference of one tick is a
    /// block that visibly reappears after breaking.
    [[nodiscard]] i32 break_ticks(registry::BlockStateId state, const Held& held,
                                  const Stance& stance) const noexcept;

private:
    /// The wire id of a block, which is not its index in the pack.
    [[nodiscard]] registry::ProtocolId wire_id(registry::BlockId block) const noexcept;

    const registry::BlockRegistry* blocks_{nullptr};
    const registry::Registries*    registries_{nullptr};

    /// Tag ids resolved once. An absent tag is stored as nothing rather than as
    /// an empty one: telling "no such tag" from "a tag with no members" is what
    /// keeps a stale pack from silently making every block hand-breakable.
    std::optional<registry::TagId> mineable_[5]{};
    std::optional<registry::TagId> needs_[3]{};
    std::optional<registry::TagId> family_tag_[7]{};
    /// Wire id of each block, indexed by the pack's own block index.
    ///
    /// The two are not the same number and nothing warns when they are mixed:
    /// the pack orders blocks by name, the registry by Mojang's registration
    /// order. Using one for the other reads a tag at the wrong entry, and stone
    /// happens to land inside `mineable/pickaxe` either way — so the mistake
    /// looks correct until an ore is mined with the wrong tool.
    std::vector<registry::ProtocolId> block_ids_;
};

}  // namespace ov::gameplay
