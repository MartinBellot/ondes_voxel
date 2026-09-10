// What a container block *is*, in one place.
//
// Until this file existed the server knew exactly one container — the chest —
// and it knew it three times: once to open a screen over 27 hard-coded slots,
// once to move items around inside a click handler, and once more so that a
// comparator behind it could read a number. A barrel, a hopper and a dropper
// were each a fourth copy nobody had written, and `gameplay::HopperRules` —
// nine passing unit tests and a measured cooldown — had nothing to run on
// because there was no container it could be handed.
//
// So a container is described once, as data:
//
//   * which block it is and which block entity type goes with it;
//   * how many slots it has, what its screen is called and which
//     `minecraft:menu` the client must open — a 9x3 opened as a 9x6 shows
//     twenty-seven slots of somebody else's inventory;
//   * and **which faces admit which slots**, which is the part that is neither
//     obvious nor uniform: a furnace takes its input from above, its fuel from
//     the side, and gives its output up only to something reaching from below.
//
// ── The two shapes items take here ──────────────────────────────────────────
//
// `net::ItemStack` is the wire's, and it carries the item's NBT as raw bytes
// that this server deliberately does not parse. `gameplay::SlotStack` is the
// rules', and it carries a `tag` that is only ever compared. `TagPool` below is
// the bridge: it hashes the bytes into the `tag`, remembers them, and hands
// them back when a rule writes a stack somewhere else. Without it a hopper
// moving a named sword between two chests would deliver a plain one, and
// nothing would report an error.
//
// ── Sided access, and where the numbers come from ───────────────────────────
//
// The furnace faces are measured, not recalled: `scripts/measure_containers.py
// furnace-faces` puts one item into a furnace through each of the three faces
// with a hopper and reads back which slot it landed in. See
// docs/provenance/conteneurs.md.
#pragma once

#include "ov/gameplay/hopper.hpp"
#include "ov/math/vec.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"

#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

/// How a container answers "may I reach in through this face?".
///
/// An enum rather than a virtual per block: there are exactly three answers in
/// 1.20.1 for the containers this server opens, and naming them keeps the
/// furnace's rule from leaking into the chest's.
enum class SidedAccess : u8 {
    /// Every slot, both ways, through every face. A chest, a barrel, a dropper,
    /// a dispenser, a hopper.
    Open,
    /// Three slots with three admission rules. See `ContainerModel`.
    Furnace,
    /// A chest with one refusal: a shulker box never holds another shulker box,
    /// which is what stops the recursion.
    ShulkerBox,
};

/// One container block, described.
struct ContainerSpec {
    /// The block entity type on disk and on the wire, e.g. `minecraft:barrel`.
    std::string_view entity_type;
    i32              slots{0};
    /// The `minecraft:menu` entry the client opens. Empty when this server does
    /// not open a screen over it — a furnace's screen belongs to `workbench`,
    /// which owns its four counters as well as its slots.
    std::string_view menu;
    std::string_view title;
    SidedAccess      access{SidedAccess::Open};
    /// True when the block's own screen is `workbench`'s rather than the plain
    /// container window here. Set for the three furnaces.
    bool workbench{false};
};

/// The spec for a block, or nullptr when that block is not a container.
[[nodiscard]] const ContainerSpec* container_spec_for_block(std::string_view block_name) noexcept;

/// The spec for a block entity type, or nullptr.
///
/// Not the same question: seventeen coloured shulker boxes share one block
/// entity type, and a chest and a trapped chest do not.
[[nodiscard]] const ContainerSpec* container_spec_for_entity(std::string_view entity_type) noexcept;

/// Container blocks this project knows are containers and does not model.
///
/// Named rather than treated as "not a container", which is the failure mode
/// this project refuses: a brewing stand that silently reads as no container at
/// all makes a comparator behind it output the wire's own signal, and that
/// looks like a working circuit.
[[nodiscard]] std::span<const std::string_view> unmodelled_containers() noexcept;

/// The NBT of every item tag seen while moving items about.
///
/// `SlotStack::tag` is a 64-bit number the rules only compare; this turns it
/// back into the bytes a `net::ItemStack` needs. One pool is shared by every
/// container taking part in one transfer, which is what makes a tag mean the
/// same thing on both sides of it.
class TagPool {
public:
    /// The handle for these bytes. 0 for an empty tag, and the same number
    /// every time for the same bytes.
    [[nodiscard]] u64 intern(const std::vector<u8>& bytes);

    /// The bytes behind a handle. Empty for 0, and empty for a handle this pool
    /// never issued — which cannot happen while one pool serves one transfer.
    [[nodiscard]] const std::vector<u8>& bytes(u64 tag) const;

private:
    std::unordered_map<u64, std::vector<u8>> tags_;
};

/// A container's items, in the server's own shape.
///
/// Loaded from the block entity, worked on, written back. Deliberately a value
/// rather than a view over the NBT: a click handler swaps two slots forty times
/// a second and re-encoding a tag list per swap would be the only allocation in
/// the tick loop that had no reason to be there.
class BlockInventory {
public:
    BlockInventory() = default;

    /// An empty inventory of the right size for this spec.
    BlockInventory(const ContainerSpec& spec, const registry::Registries* registries,
                   std::optional<registry::RegistryId> item_registry);

    /// Read `Items` out of a block entity's NBT.
    ///
    /// Vanilla stores them as a list of `{Slot, id, Count, tag}` and **omits
    /// empty slots**, so the list is not indexed by slot and its length says
    /// nothing about the container's size. Reading it as an array is the bug
    /// that puts every chest's contents in the wrong place.
    void load(const nbt::Tag& data);

    /// Write them back, in the shape vanilla reads. Empty slots are omitted.
    void store(nbt::Tag& data) const;

    [[nodiscard]] std::span<net::ItemStack>       stacks() noexcept { return slots_; }
    [[nodiscard]] std::span<const net::ItemStack> stacks() const noexcept { return slots_; }

    [[nodiscard]] i32 size() const noexcept { return static_cast<i32>(slots_.size()); }

    [[nodiscard]] const ContainerSpec& spec() const noexcept { return *spec_; }

    /// The item registry this inventory names its items in. Needed by the
    /// bridge, which has to ask what an item is called to tell a shulker box
    /// from anything else.
    [[nodiscard]] std::optional<registry::RegistryId> item_registry() const noexcept {
        return item_registry_;
    }

    /// The comparator's reading of this container, 0..15.
    ///
    /// `floor(14 * fullness / size) + 1` for a non-empty container and 0 for an
    /// empty one — the rule `gameplay::Redstone::container_reading` already
    /// holds, measured over 28 fill levels. This only supplies the fullness.
    [[nodiscard]] i32 comparator_reading() const;

private:
    const ContainerSpec*                spec_{nullptr};
    const registry::Registries*         registries_{nullptr};
    std::optional<registry::RegistryId> item_registry_{};
    std::vector<net::ItemStack>         slots_;
};

/// A `BlockInventory` as the item-moving rules see it.
///
/// Holds references, owns nothing, and costs nothing to build — which is what
/// lets the hopper pass make three of them per hopper per tick without
/// allocating.
class ContainerBridge final : public gameplay::ItemContainer {
public:
    ContainerBridge(BlockInventory& inventory, const registry::Registries* registries,
                    TagPool& pool)
        : inventory_{&inventory}, registries_{registries}, pool_{&pool} {}

    [[nodiscard]] i32 slot_count() const override { return inventory_->size(); }

    [[nodiscard]] gameplay::SlotStack slot(i32 index) const override;

    void set_slot(i32 index, const gameplay::SlotStack& stack) override;

    [[nodiscard]] i32 max_stack(registry::ProtocolId item) const override;

    [[nodiscard]] bool can_take_from(i32 index, Direction face) const override;

    [[nodiscard]] bool can_place_into(i32 index, const gameplay::SlotStack& stack,
                                      Direction face) const override;

private:
    BlockInventory*             inventory_{nullptr};
    const registry::Registries* registries_{nullptr};
    TagPool*                    pool_{nullptr};
};

/// Build the block entity a freshly placed container needs.
///
/// A container block with no block entity is the failure the lab's own plots
/// were built with: the block is there, it opens nothing, and nothing says so.
[[nodiscard]] world::BlockEntity new_container_entity(const ContainerSpec& spec, i32 x, i32 y,
                                                      i32 z, const registry::Registries* registries,
                                                      std::optional<registry::RegistryId>
                                                          block_entity_registry);

}  // namespace ov::server
