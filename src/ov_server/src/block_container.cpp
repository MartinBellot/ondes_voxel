#include "block_container.hpp"

#include "ov/gameplay/redstone.hpp"
#include "ov/nbt/binary.hpp"

#include <algorithm>
#include <array>

namespace ov::server {
namespace {

/// Every container this server models, and one line each.
///
/// Ordered by nothing in particular; the two lookups below are linear over
/// forty entries and happen when a screen opens or a block is placed, never in
/// a tick body.
struct BlockSpec {
    std::string_view block;
    ContainerSpec    spec;
};

/// 9x3 twice over: the chest and its two relatives that are the same window.
constexpr ContainerSpec kChest{"minecraft:chest", 27, "minecraft:generic_9x3", "Chest",
                               SidedAccess::Open, false};
constexpr ContainerSpec kTrappedChest{"minecraft:trapped_chest", 27, "minecraft:generic_9x3",
                                      "Trapped Chest", SidedAccess::Open, false};
constexpr ContainerSpec kBarrel{"minecraft:barrel", 27, "minecraft:generic_9x3", "Barrel",
                                SidedAccess::Open, false};
constexpr ContainerSpec kShulker{"minecraft:shulker_box", 27, "minecraft:shulker_box",
                                 "Shulker Box", SidedAccess::ShulkerBox, false};
constexpr ContainerSpec kHopper{"minecraft:hopper", 5, "minecraft:hopper", "Hopper",
                                SidedAccess::Open, false};
constexpr ContainerSpec kDispenser{"minecraft:dispenser", 9, "minecraft:generic_3x3", "Dispenser",
                                   SidedAccess::Open, false};
constexpr ContainerSpec kDropper{"minecraft:dropper", 9, "minecraft:generic_3x3", "Dropper",
                                 SidedAccess::Open, false};
/// The three furnaces. `menu` is empty because `workbench.cpp` opens their
/// screen — it owns the four counters as well as the three slots — but they are
/// still containers here, because a hopper feeds one and a comparator reads one.
constexpr ContainerSpec kFurnace{"minecraft:furnace", 3, {}, "Furnace", SidedAccess::Furnace, true};
constexpr ContainerSpec kBlastFurnace{"minecraft:blast_furnace", 3,   {},
                                      "Blast Furnace",          SidedAccess::Furnace, true};
constexpr ContainerSpec kSmoker{"minecraft:smoker", 3, {}, "Smoker", SidedAccess::Furnace, true};

/// The seventeen shulker boxes all share one block entity type, which is why
/// `container_spec_for_entity` cannot simply be `container_spec_for_block` with
/// a different argument.
constexpr std::array<std::string_view, 17> kShulkerColours{
    "minecraft:shulker_box",        "minecraft:white_shulker_box",
    "minecraft:orange_shulker_box", "minecraft:magenta_shulker_box",
    "minecraft:light_blue_shulker_box", "minecraft:yellow_shulker_box",
    "minecraft:lime_shulker_box",   "minecraft:pink_shulker_box",
    "minecraft:gray_shulker_box",   "minecraft:light_gray_shulker_box",
    "minecraft:cyan_shulker_box",   "minecraft:purple_shulker_box",
    "minecraft:blue_shulker_box",   "minecraft:brown_shulker_box",
    "minecraft:green_shulker_box",  "minecraft:red_shulker_box",
    "minecraft:black_shulker_box",
};

constexpr std::array<BlockSpec, 10> kBlocks{{
    {"minecraft:chest", kChest},
    {"minecraft:trapped_chest", kTrappedChest},
    {"minecraft:barrel", kBarrel},
    {"minecraft:hopper", kHopper},
    {"minecraft:dispenser", kDispenser},
    {"minecraft:dropper", kDropper},
    {"minecraft:furnace", kFurnace},
    {"minecraft:blast_furnace", kBlastFurnace},
    {"minecraft:smoker", kSmoker},
    {"minecraft:shulker_box", kShulker},
}};

/// Containers in 1.20.1 this server does not model, by name.
///
/// The ender chest is here because it is not a block inventory at all — its
/// contents belong to the player who opened it — and modelling it as twenty-
/// seven slots on the block would give every player the same one.
constexpr std::array<std::string_view, 6> kUnmodelled{
    "minecraft:brewing_stand", "minecraft:ender_chest", "minecraft:lectern",
    "minecraft:chiseled_bookshelf", "minecraft:jukebox", "minecraft:crafter",
};

/// A furnace's three slots are 0 input, 1 fuel, 2 output. Named because the
/// numbers are otherwise three magic constants in four places.
constexpr i32 kFurnaceInput  = 0;
constexpr i32 kFurnaceFuel   = 1;
constexpr i32 kFurnaceOutput = 2;

/// 64-bit FNV-1a. Only ever compared and used as a map key, so any decent
/// avalanche will do; named rather than `std::hash` because the value has to be
/// the same in every build for a saved tag to keep meaning the same thing
/// inside one run.
[[nodiscard]] u64 fnv1a(std::span<const u8> bytes) noexcept {
    u64 hash = 0xCBF29CE484222325ULL;
    for (const u8 byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001B3ULL;
    }
    // 0 means "no tag" to the rules, so a tag that hashes to zero has to move.
    return hash == 0 ? 1 : hash;
}

}  // namespace

const ContainerSpec* container_spec_for_block(std::string_view block_name) noexcept {
    if (std::ranges::find(kShulkerColours, block_name) != kShulkerColours.end()) {
        return &kShulker;
    }
    for (const BlockSpec& entry : kBlocks) {
        if (entry.block == block_name) {
            return &entry.spec;
        }
    }
    return nullptr;
}

const ContainerSpec* container_spec_for_entity(std::string_view entity_type) noexcept {
    for (const BlockSpec& entry : kBlocks) {
        if (entry.spec.entity_type == entity_type) {
            return &entry.spec;
        }
    }
    return nullptr;
}

std::span<const std::string_view> unmodelled_containers() noexcept { return kUnmodelled; }

// ── TagPool ─────────────────────────────────────────────────────────────────

u64 TagPool::intern(const std::vector<u8>& bytes) {
    if (bytes.empty()) {
        return 0;
    }
    const u64 tag = fnv1a(bytes);
    tags_.try_emplace(tag, bytes);
    return tag;
}

const std::vector<u8>& TagPool::bytes(u64 tag) const {
    static const std::vector<u8> kNone;
    const auto                   found = tags_.find(tag);
    return found == tags_.end() ? kNone : found->second;
}

// ── BlockInventory ──────────────────────────────────────────────────────────

BlockInventory::BlockInventory(const ContainerSpec& spec, const registry::Registries* registries,
                               std::optional<registry::RegistryId> item_registry)
    : spec_{&spec},
      registries_{registries},
      item_registry_{item_registry},
      slots_(static_cast<usize>(spec.slots)) {}

void BlockInventory::load(const nbt::Tag& data) {
    for (net::ItemStack& stack : slots_) {
        stack = {};
    }
    const nbt::Tag* items = data.find("Items");
    if (items == nullptr || items->list() == nullptr || registries_ == nullptr ||
        !item_registry_) {
        return;
    }
    for (const nbt::Tag& entry : *items->list()) {
        const nbt::Tag* slot  = entry.find("Slot");
        const nbt::Tag* id    = entry.find("id");
        const nbt::Tag* count = entry.find("Count");
        if (slot == nullptr || id == nullptr || count == nullptr) {
            continue;
        }
        const auto index = static_cast<usize>(slot->as_i64());
        if (index >= slots_.size()) {
            continue;  // a slot this container does not have
        }
        const auto item = registries_->protocol_id(*item_registry_, id->as_string());
        if (!item) {
            continue;  // an item this version does not have
        }
        net::ItemStack stack{*item, static_cast<i8>(count->as_i64()), {}};
        // The item's own NBT, kept as the bytes the wire wants. Vanilla stores
        // it as a `tag` compound; the wire wants a whole named document, so it
        // is re-encoded once here rather than parsed and re-parsed per click.
        if (const nbt::Tag* tag = entry.find("tag"); tag != nullptr) {
            stack.nbt = nbt::write(nbt::Document{"tag", *tag});
        }
        slots_[index] = std::move(stack);
    }
}

void BlockInventory::store(nbt::Tag& data) const {
    nbt::Tag items = nbt::Tag::make_list(nbt::TagType::Compound);
    if (registries_ != nullptr && item_registry_) {
        for (usize i = 0; i < slots_.size(); ++i) {
            if (slots_[i].empty()) {
                continue;  // empty slots are omitted, not stored as air
            }
            const std::string_view name = registries_->entry_of(*item_registry_, slots_[i].item_id);
            if (name.empty()) {
                continue;
            }
            nbt::Tag entry = nbt::Tag::make_compound();
            (void)entry.put("Slot", nbt::Tag{static_cast<i8>(i)});
            (void)entry.put("id", nbt::Tag{std::string{name}});
            (void)entry.put("Count", nbt::Tag{slots_[i].count});
            if (!slots_[i].nbt.empty()) {
                if (const auto document = nbt::read(slots_[i].nbt)) {
                    (void)entry.put("tag", document->root);
                }
            }
            (void)items.push(std::move(entry));
        }
    }
    if (data.compound() == nullptr) {
        data = nbt::Tag::make_compound();
    }
    (void)data.put("Items", std::move(items));
}

i32 BlockInventory::comparator_reading() const {
    f32  fullness = 0.0F;
    bool any      = false;
    for (const net::ItemStack& stack : slots_) {
        if (stack.empty()) {
            continue;
        }
        any = true;
        const f32 limit = registries_ != nullptr
                              ? static_cast<f32>(registries_->max_stack_size(stack.item_id))
                              : 64.0F;
        fullness += static_cast<f32>(stack.count) / (limit > 0.0F ? limit : 64.0F);
    }
    if (slots_.empty()) {
        return 0;
    }
    return gameplay::Redstone::container_reading(fullness / static_cast<f32>(slots_.size()), any);
}

// ── ContainerBridge ─────────────────────────────────────────────────────────

gameplay::SlotStack ContainerBridge::slot(i32 index) const {
    if (index < 0 || index >= inventory_->size()) {
        return {};
    }
    const net::ItemStack& stack = inventory_->stacks()[static_cast<usize>(index)];
    if (stack.empty()) {
        return {};
    }
    return gameplay::SlotStack{stack.item_id, stack.count, pool_->intern(stack.nbt)};
}

void ContainerBridge::set_slot(i32 index, const gameplay::SlotStack& stack) {
    if (index < 0 || index >= inventory_->size()) {
        return;
    }
    net::ItemStack& target = inventory_->stacks()[static_cast<usize>(index)];
    if (stack.empty()) {
        target = {};
        return;
    }
    target.item_id = stack.item;
    target.count   = static_cast<i8>(stack.count);
    // The tag comes back out of the pool rather than being kept from whatever
    // used to be in this slot: a hopper writing a named sword into an empty
    // slot has nothing to keep, and one writing a plain sword over a named one
    // must not inherit the name.
    target.nbt = pool_->bytes(stack.tag);
}

i32 ContainerBridge::max_stack(registry::ProtocolId item) const {
    if (registries_ == nullptr) {
        return 64;
    }
    const i32 limit = registries_->max_stack_size(item);  // NOLINT(*-narrowing-conversions)
    // A hopper holds at most one stack's worth per slot whatever the item is,
    // and a registry that does not know the item says so rather than zero —
    // zero would make every insert fail and look exactly like a full container.
    return limit > 0 ? limit : 64;
}

bool ContainerBridge::can_take_from(i32 index, Direction face) const {
    switch (inventory_->spec().access) {
        case SidedAccess::Open:
        case SidedAccess::ShulkerBox: return true;
        case SidedAccess::Furnace:
            // Measured: a hopper under a furnace pulls its **output**, and
            // nothing else. The fuel slot is reachable from below in vanilla
            // only to take an emptied bucket back out, which this server does
            // not produce yet — so it is refused rather than half-modelled.
            if (face == Direction::Down) {
                return index == kFurnaceOutput;
            }
            return false;
    }
    return true;
}

bool ContainerBridge::can_place_into(i32 index, const gameplay::SlotStack& stack,
                                     Direction face) const {
    switch (inventory_->spec().access) {
        case SidedAccess::Open: return true;
        case SidedAccess::ShulkerBox: {
            // A shulker box never holds another shulker box. Everything else
            // goes in, including from a hopper.
            if (registries_ == nullptr) {
                return true;
            }
            const auto items = inventory_->item_registry();
            if (!items) {
                return true;
            }
            return !registries_->entry_of(*items, stack.item).ends_with("shulker_box");
        }
        case SidedAccess::Furnace:
            // The three faces, measured with one hopper each:
            //   above → the input slot;
            //   the side → the fuel slot;
            //   below → nothing goes in, the bottom is an exit.
            if (face == Direction::Up) {
                return index == kFurnaceInput;
            }
            if (face == Direction::Down) {
                return false;
            }
            return index == kFurnaceFuel;
    }
    return true;
}

// ── Placement ───────────────────────────────────────────────────────────────

world::BlockEntity new_container_entity(const ContainerSpec& spec, i32 x, i32 y, i32 z,
                                        const registry::Registries*         registries,
                                        std::optional<registry::RegistryId> block_entity_registry) {
    world::BlockEntity entity;
    entity.x    = static_cast<u8>(x & 15);
    entity.y    = y;
    entity.z    = static_cast<u8>(z & 15);
    entity.type = std::string{spec.entity_type};
    entity.type_id =
        registries != nullptr && block_entity_registry
            ? registries->protocol_id(*block_entity_registry, spec.entity_type).value_or(0)
            : 0;
    entity.data = nbt::Tag::make_compound();
    // An empty `Items` list rather than no list at all. Vanilla writes one for
    // an empty chest, and a save that omits it reloads the same either way —
    // but the shape a reader finds is then the shape it was written with, which
    // is what the cross round-trip test checks.
    const BlockInventory empty{spec, registries,
                               registries != nullptr ? registries->find("minecraft:item")
                                                     : std::nullopt};
    empty.store(entity.data);
    return entity;
}

}  // namespace ov::server
