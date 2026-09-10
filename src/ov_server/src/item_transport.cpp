#include "item_transport.hpp"

#include "ov/base/log.hpp"

#include <algorithm>

namespace ov::server {
namespace {

/// The NBT field a hopper keeps its wait in. Vanilla's name, because the save
/// has to be readable by the real game: a hopper reloaded with an unknown field
/// and no `TransferCooldown` is a hopper that moves an item on the tick it
/// loads, and that is visible in a filter.
constexpr std::string_view kCooldownField = "TransferCooldown";

}  // namespace

bool hopper_suck_contains(BlockPos hopper, f64 x, f64 y, f64 z) noexcept {
    // Vanilla's suck box is (0, 1, 0) to (1, 1.5, 1) relative to the hopper,
    // and what it tests is an **intersection** with the item's own box, not
    // whether the item's point is inside it. An item entity is a quarter of a
    // block wide, a quarter tall, and its reported position is its centre in x
    // and z and its feet in y.
    constexpr f64 kItemHalfWidth = 0.125;
    constexpr f64 kItemHeight    = 0.25;
    constexpr f64 kSuckTop       = 1.5;

    const auto bx = static_cast<f64>(hopper.x);
    const auto by = static_cast<f64>(hopper.y);
    const auto bz = static_cast<f64>(hopper.z);
    return x + kItemHalfWidth > bx && x - kItemHalfWidth < bx + 1.0 &&
           z + kItemHalfWidth > bz && z - kItemHalfWidth < bz + 1.0 &&
           y + kItemHeight > by + 1.0 && y < by + kSuckTop;
}

ItemTransport::ItemTransport(const registry::BlockRegistry& blocks,
                             const registry::Registries&    registries)
    : blocks_{&blocks},
      registries_{&registries},
      item_registry_{registries.find("minecraft:item")},
      dispenser_{registries},
      hopper_block_{blocks.find_block("minecraft:hopper")},
      dispenser_block_{blocks.find_block("minecraft:dispenser")},
      dropper_block_{blocks.find_block("minecraft:dropper")} {
    hoppers_.reserve(256);
    pending_.reserve(64);
}

void ItemTransport::note_block_change(BlockPos pos, registry::BlockStateId before,
                                      registry::BlockStateId after) {
    if (before == after || blocks_ == nullptr) {
        return;
    }
    const registry::BlockId block = blocks_->block_of(after);
    const bool              known = (dispenser_block_ && block == *dispenser_block_) ||
                       (dropper_block_ && block == *dropper_block_);
    if (!known) {
        return;
    }
    // The same block on both sides, or the edge means nothing: a dispenser
    // replaced by a dropper is not a dispenser being triggered.
    if (blocks_->block_of(before) != block) {
        return;
    }
    const auto triggered = blocks_->find_property(block, "triggered");
    if (!triggered) {
        return;
    }
    const bool was = blocks_->property_value(before, *triggered) == "true";
    const bool now = blocks_->property_value(after, *triggered) == "true";
    if (!was && now) {
        // Only the rising edge. A dispenser held triggered by a lever fires
        // once, which is what everybody's first dispenser clock is built out
        // of failing to do.
        pending_.push_back(PendingFire{pos, 0});
    }
}

bool ItemTransport::open_container(const TransportHost& host, BlockPos pos,
                                   LoadedContainer& out) const {
    if (!host.chunk) {
        return false;
    }
    world::Chunk* chunk = host.chunk(pos.x >> 4, pos.z >> 4);
    if (chunk == nullptr) {
        return false;
    }
    world::BlockEntity* entity = chunk->block_entity_at(static_cast<usize>(pos.x & 15), pos.y,
                                                        static_cast<usize>(pos.z & 15));
    if (entity == nullptr) {
        return false;
    }
    const ContainerSpec* spec = container_spec_for_entity(entity->type);
    if (spec == nullptr) {
        return false;
    }
    out.chunk     = chunk;
    out.entity    = entity;
    out.spec      = spec;
    out.inventory = BlockInventory{*spec, registries_, item_registry_};
    out.inventory.load(entity->data);
    return true;
}

void ItemTransport::store_container(const TransportHost& host, LoadedContainer& container,
                                    BlockPos pos) const {
    container.inventory.store(container.entity->data);
    if (host.mark_dirty) {
        host.mark_dirty(pos.x >> 4, pos.z >> 4);
    }
    if (host.container_changed) {
        host.container_changed(pos);
    }
}

void ItemTransport::rebuild_index(const TransportHost& host, std::span<const ChunkPos> loaded) {
    hoppers_.clear();
    if (!host.chunk) {
        return;
    }
    for (const ChunkPos& where : loaded) {
        world::Chunk* chunk = host.chunk(where.x, where.z);
        if (chunk == nullptr) {
            continue;
        }
        for (const world::BlockEntity& entity : chunk->block_entities()) {
            if (entity.type != "minecraft:hopper") {
                continue;
            }
            hoppers_.push_back(
                Machine{BlockPos{where.x * 16 + static_cast<i32>(entity.x), entity.y,
                                 where.z * 16 + static_cast<i32>(entity.z)}});
        }
    }
}

void ItemTransport::tick_hopper(const TransportHost& host, BlockPos pos, TransportStats& stats) {
    LoadedContainer self;
    if (!open_container(host, pos, self) || self.spec->entity_type != "minecraft:hopper") {
        return;  // broken, or replaced by something else since the index
    }

    const registry::BlockStateId state =
        self.chunk->get_block(static_cast<usize>(pos.x & 15), pos.y, static_cast<usize>(pos.z & 15));
    const registry::BlockId block = blocks_->block_of(state);
    if (!hopper_block_ || block != *hopper_block_) {
        return;
    }

    // The wait comes down every tick whether the hopper is enabled or not:
    // vanilla decrements before it looks at the flag, so a hopper unlocked
    // mid-wait finishes the wait it was in rather than moving at once.
    auto cooldown = static_cast<i32>(self.entity->data.find(std::string{kCooldownField}) != nullptr
                                         ? self.entity->data.find(std::string{kCooldownField})
                                               ->as_i64()
                                         : 0);
    --cooldown;

    const auto enabled_property = blocks_->find_property(block, "enabled");
    const bool enabled =
        !enabled_property || blocks_->property_value(state, *enabled_property) == "true";

    bool moved = false;
    if (cooldown <= 0 && enabled) {
        const auto facing_property = blocks_->find_property(block, "facing");
        const auto facing          = facing_property ? direction_from_name(blocks_->property_value(
                                                  state, *facing_property))
                                                     : std::optional<Direction>{Direction::Down};
        const Direction where = facing.value_or(Direction::Down);

        const Vec3i    step   = direction_offset(where);
        const BlockPos target = {pos.x + step.x, pos.y + step.y, pos.z + step.z};
        const BlockPos above  = {pos.x, pos.y + 1, pos.z};

        LoadedContainer into;
        LoadedContainer from;
        const bool      has_target = open_container(host, target, into);
        const bool      has_source = open_container(host, above, from);

        ContainerBridge self_bridge{self.inventory, registries_, tags_};
        // Built unconditionally because a bridge holds references and costs
        // nothing; only the pointers handed to the rule depend on what is
        // actually there.
        ContainerBridge into_bridge{has_target ? into.inventory : self.inventory, registries_,
                                    tags_};
        ContainerBridge from_bridge{has_source ? from.inventory : self.inventory, registries_,
                                    tags_};

        const gameplay::HopperRules::TickResult result = gameplay::HopperRules::tick(
            self_bridge, has_target ? &into_bridge : nullptr, where,
            has_source ? &from_bridge : nullptr);

        if (result.pushed && has_target) {
            store_container(host, into, target);
        }
        if (result.pulled && has_source) {
            store_container(host, from, above);
        }
        moved = result.moved();

        // Item entities, and **only** when there is no container above: that is
        // vanilla's own else-branch, and a hopper under a chest that also
        // swallowed the items falling on the chest would move two items a
        // cycle rather than one.
        if (!has_source && host.collect) {
            const bool took = host.collect(pos, [&](net::ItemStack& stack) {
                if (stack.empty()) {
                    return false;
                }
                gameplay::SlotStack incoming{stack.item_id, stack.count,
                                             tags_.intern(stack.nbt)};
                const i32 fitted =
                    gameplay::HopperRules::insert(self_bridge, incoming, Direction::Up);
                if (fitted == 0) {
                    return false;
                }
                // Whatever did not fit stays on the floor. `insert` leaves the
                // remainder in the stack it was given, and a caller that
                // ignored it would destroy items rather than fail.
                stack.count = static_cast<i8>(incoming.count);
                return true;
            });
            if (took) {
                ++stats.collected;
                moved = true;
            }
        }
    }

    if (moved) {
        cooldown = gameplay::HopperRules::kTransferCooldown;
        ++stats.hopper_moves;
    }
    (void)self.entity->data.put(std::string{kCooldownField},
                                nbt::Tag{static_cast<i32>(std::max(cooldown, 0))});
    store_container(host, self, pos);
}

void ItemTransport::fire_machine(const TransportHost& host, BlockPos pos, TransportStats& stats) {
    LoadedContainer machine;
    if (!open_container(host, pos, machine)) {
        return;
    }
    const bool is_dispenser = machine.spec->entity_type == "minecraft:dispenser";
    const bool is_dropper   = machine.spec->entity_type == "minecraft:dropper";
    if (!is_dispenser && !is_dropper) {
        return;
    }

    ContainerBridge bridge{machine.inventory, registries_, tags_};
    const i32       index = gameplay::Dispenser::slot_to_fire(bridge);
    if (index < 0) {
        // Vanilla plays a click and does nothing. Nothing to save either.
        return;
    }
    net::ItemStack& stack = machine.inventory.stacks()[static_cast<usize>(index)];

    const registry::BlockStateId state =
        machine.chunk->get_block(static_cast<usize>(pos.x & 15), pos.y,
                                 static_cast<usize>(pos.z & 15));
    const registry::BlockId block         = blocks_->block_of(state);
    const auto              facing_of     = blocks_->find_property(block, "facing");
    const auto              facing        = facing_of ? direction_from_name(blocks_->property_value(
                                              state, *facing_of))
                                                      : std::optional<Direction>{};
    const Direction         where         = facing.value_or(Direction::North);
    const Vec3i             step          = direction_offset(where);
    const BlockPos          front         = {pos.x + step.x, pos.y + step.y, pos.z + step.z};

    const gameplay::DispenseAction action =
        is_dropper ? gameplay::Dispenser::dropper_action() : dispenser_.action_for(stack.item_id);

    // A **dropper** facing a container puts the item in it rather than on the
    // floor. A dispenser does not — measured, one of each facing a chest: see
    // docs/provenance/conteneurs.md.
    LoadedContainer ahead;
    if (is_dropper && open_container(host, front, ahead)) {
        ContainerBridge     into{ahead.inventory, registries_, tags_};
        gameplay::SlotStack one{stack.item_id, 1, tags_.intern(stack.nbt)};
        const i32 fitted = gameplay::HopperRules::insert(into, one, opposite(where));
        if (fitted > 0) {
            stack.count = static_cast<i8>(stack.count - 1);
            if (stack.count <= 0) {
                stack = {};
            }
            store_container(host, ahead, front);
            store_container(host, machine, pos);
            ++stats.fired;
        }
        return;
    }

    switch (action.kind) {
        case gameplay::DispenseKind::Eject:
        case gameplay::DispenseKind::IntoContainer: {
            net::ItemStack thrown = stack;
            thrown.count          = 1;
            stack.count           = static_cast<i8>(stack.count - 1);
            if (stack.count <= 0) {
                stack = {};
            }
            if (host.eject) {
                host.eject(front, where, thrown);
            }
            store_container(host, machine, pos);
            ++stats.fired;
            return;
        }
        case gameplay::DispenseKind::PlaceFluid:
        case gameplay::DispenseKind::TakeFluid:
        case gameplay::DispenseKind::Ignite:
        case gameplay::DispenseKind::Projectile:
        case gameplay::DispenseKind::PrimeTnt:
        case gameplay::DispenseKind::SpawnMob:
        case gameplay::DispenseKind::Refused:
            break;
    }

    // Named, and nothing happens — the item stays in the machine. Falling back
    // to Eject here is precisely the failure this project refuses: a dispenser
    // that drops a water bucket instead of placing water is indistinguishable
    // from one that works until somebody builds a farm on it.
    ++stats.unsupported;
    name_unsupported(std::string{registries_ != nullptr && item_registry_
                                     ? registries_->entry_of(*item_registry_, stack.item_id)
                                     : std::string_view{"?"}});
}

void ItemTransport::name_unsupported(std::string what) {
    if (std::ranges::find(unsupported_, what) != unsupported_.end()) {
        return;
    }
    OV_LOG_DEBUG("dispenser: {} has a behaviour this server names and does not carry out", what);
    unsupported_.push_back(std::move(what));
}

TransportStats ItemTransport::tick(const TransportHost& host, std::span<const ChunkPos> loaded,
                                   i64 now) {
    TransportStats stats;
    if (blocks_ == nullptr || registries_ == nullptr) {
        return stats;
    }

    if (indexed_at_ < 0 || now - indexed_at_ >= kIndexPeriod) {
        rebuild_index(host, loaded);
        indexed_at_ = now;
    }
    stats.hoppers = hoppers_.size();

    for (const Machine& hopper : hoppers_) {
        tick_hopper(host, hopper.pos, stats);
    }

    // The scheduled firings. A rising edge noted during this tick's redstone
    // drain gets its `at` here, so that the four-tick delay is counted from the
    // tick the flag went up rather than from whenever the pass happens to run.
    for (PendingFire& fire : pending_) {
        if (fire.at == 0) {
            fire.at = now + kFireDelay;
        }
    }
    for (const PendingFire& fire : pending_) {
        if (fire.at <= now) {
            fire_machine(host, fire.pos, stats);
        }
    }
    std::erase_if(pending_, [now](const PendingFire& fire) { return fire.at <= now; });

    return stats;
}

}  // namespace ov::server
