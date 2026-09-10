// What comes out of a dispenser, and what comes out of a dropper.
//
// The dropper is the simple half and it is also the control: it **always**
// ejects, whatever it holds. So any item where the two machines agree is an
// item where the dispenser fell through to its default, and any item where they
// differ is a behaviour with a name. That is how the table below was read off a
// real server rather than assembled from memory — one dispenser and one dropper
// per item, twenty-four blocks apart, triggered and looked at.
//
// ── The shape of the answer ─────────────────────────────────────────────────
//
// A dispenser does not "dispense an item". It looks the item up in a registry
// of behaviours, and the behaviour decides what happens; the default behaviour
// is "eject it as an item entity", which is also the whole of the dropper. So
// this file is a lookup returning a `DispenseAction`, and the caller — which
// owns entities, fluids and the world — carries it out. `ov_gameplay` cannot
// place water or spawn an arrow; it can say which of those was asked for.
//
// ── What is measured, and what is refused ───────────────────────────────────
//
// Eighteen items were put through a real 1.20.1 server. What came back is in
// docs/provenance/redstone.md §13: the block on the target square, the entity
// that appeared, and what the machine still held afterwards. Three of them are
// worth stating because no rule predicts them:
//
//   * a **water bucket** leaves an empty bucket in the dispenser — the item is
//     replaced, not consumed;
//   * a **flint and steel** is neither consumed nor replaced: it comes back
//     with `Damage: 1`, so the rule is a durability hit and the item only
//     leaves when it breaks;
//   * **TNT destroyed its own dispenser.** The measurement cell came back "the
//     target block is not a block entity", which is the strongest possible
//     evidence that dispensed TNT is primed rather than dropped.
//
// Items whose behaviour was *not* measured are refused by name rather than
// given the default. That is this project's rule and it exists because the
// default is plausible for every one of them: a dispenser that silently drops
// a shulker box instead of placing it looks like a dispenser that works.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/hopper.hpp"
#include "ov/registry/registries.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace ov::gameplay {

/// What a machine was asked to do with one item.
enum class DispenseKind : u8 {
    /// Throw it on the floor in front, as an item entity. The dropper's only
    /// behaviour, and the dispenser's default.
    Eject,
    /// Put it into the container in front instead of on the floor. What both
    /// machines do when they face a chest or a hopper.
    IntoContainer,
    /// Place a fluid on the target square, and leave an empty bucket behind.
    PlaceFluid,
    /// Take the fluid on the target square into the bucket.
    TakeFluid,
    /// Light the target square, and damage the tool by one.
    Ignite,
    /// Spawn a projectile flying out of the face.
    Projectile,
    /// Spawn a primed explosive on the target square.
    PrimeTnt,
    /// Spawn the mob a spawn egg names.
    SpawnMob,
    /// Named and not implemented. The caller must not fall back to `Eject`.
    Refused,
};

/// One decision.
struct DispenseAction {
    DispenseKind kind{DispenseKind::Eject};
    /// The entity type a projectile or a spawn egg makes, empty otherwise.
    std::string_view entity{};
    /// The block a fluid placement writes, empty otherwise.
    std::string_view block{};
    /// True when the item is replaced rather than consumed — a water bucket
    /// becoming an empty one. `replacement` then names what it becomes.
    std::string_view replacement{};
    /// True when the item stays and takes a point of damage instead.
    bool damages{false};
    /// Why, when `kind` is `Refused`. Never empty in that case.
    std::string_view refusal{};
};

/// The behaviour table.
///
/// Built once against the item registry so the hot path is an array index, the
/// same shape `Signals` and `Redstone` use for blocks.
class Dispenser {
public:
    explicit Dispenser(const registry::Registries& registries);

    /// What a **dispenser** does with this item.
    [[nodiscard]] DispenseAction action_for(registry::ProtocolId item) const noexcept;

    /// What a **dropper** does with this item, which is the same thing every
    /// time: eject it, or put it in the container in front. A dropper has no
    /// per-item behaviour at all, and saying so in code is what stops the
    /// dispenser's table from leaking into it.
    [[nodiscard]] static DispenseAction dropper_action() noexcept {
        return DispenseAction{DispenseKind::Eject};
    }

    /// The first non-empty slot a machine would fire, or -1.
    ///
    /// Vanilla picks a **random** one among the non-empty slots. This picks the
    /// first, and says so: the choice is visible in a dispenser holding two
    /// different items, and reproducing the draw needs the machine's own RNG
    /// stream, which this project does not carry on a block entity yet.
    [[nodiscard]] static i32 slot_to_fire(const ItemContainer& machine);

    /// Items vanilla gives a behaviour to and this table does not.
    ///
    /// Named rather than left to the default. A test walks this list, so a gap
    /// is a failing assertion with an item name in it and not a dispenser that
    /// quietly drops a shulker box on the floor.
    [[nodiscard]] std::span<const std::string_view> unhandled() const noexcept {
        return unhandled_;
    }

private:
    std::vector<DispenseAction>   actions_;
    std::vector<std::string_view> unhandled_;
};

}  // namespace ov::gameplay
