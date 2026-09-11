#include "rails_session.hpp"

#include "ov/base/log.hpp"
#include "ov/gameplay/redstone.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/varint.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <utility>

namespace ov::server {

namespace {

/// Set Passengers. **0x59**, found by content in a capture of the real server
/// (the cart's id, a count of 1, the probe's id) — scripts/measure_rails.py
/// `capture`.
constexpr i32 kSetPassengers = 0x59;

/// The metadata of a minecart, one NBT field at a time against a baseline
/// cart on the real server (`capture`).
constexpr u8 kHurtTime      = 8;   // VarInt
constexpr u8 kHurtDirection = 9;   // VarInt
constexpr u8 kDamage        = 10;  // Float
constexpr u8 kDisplayBlock  = 11;  // VarInt, a block state id
constexpr u8 kDisplayOffset = 12;  // VarInt
constexpr u8 kCustomDisplay = 13;  // Boolean
constexpr u8 kFurnaceFuel   = 14;  // Boolean, a furnace cart that burns

[[nodiscard]] u64 mix(u64 z) noexcept {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

void put(nbt::Tag& compound, std::string_view name, nbt::Tag value) {
    if (nbt::Tag* existing = compound.find(name)) {
        *existing = std::move(value);
        return;
    }
    compound.compound()->push_back(nbt::CompoundEntry{std::string{name}, std::move(value)});
}

[[nodiscard]] nbt::Tag doubles(std::initializer_list<f64> values) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Double);
    for (const f64 value : values) {
        (void)list.push(nbt::Tag{value});
    }
    return list;
}

[[nodiscard]] f64 list_f64(const nbt::Tag& compound, std::string_view name, usize index,
                           f64 fallback) {
    const nbt::Tag* list = compound.find(name);
    if (list == nullptr || list->list() == nullptr || list->list()->size() <= index) {
        return fallback;
    }
    return (*list->list())[index].as_f64(fallback);
}

[[nodiscard]] nbt::Tag uuid_tag(const net::Uuid& uuid) {
    return nbt::Tag{nbt::Tag::IntArray{
        static_cast<i32>(static_cast<u32>(uuid.most_significant >> 32U)),
        static_cast<i32>(static_cast<u32>(uuid.most_significant & 0xFFFFFFFFU)),
        static_cast<i32>(static_cast<u32>(uuid.least_significant >> 32U)),
        static_cast<i32>(static_cast<u32>(uuid.least_significant & 0xFFFFFFFFU))}};
}

[[nodiscard]] std::optional<net::Uuid> uuid_of(const nbt::Tag& compound) {
    const nbt::Tag* tag = compound.find("UUID");
    if (tag == nullptr) {
        return std::nullopt;
    }
    const auto* ints = tag->get_if<nbt::Tag::IntArray>();
    if (ints == nullptr || ints->size() != 4) {
        return std::nullopt;
    }
    const auto word = [&](usize i) { return static_cast<u64>(static_cast<u32>((*ints)[i])); };
    return net::Uuid{(word(0) << 32U) | word(1), (word(2) << 32U) | word(3)};
}

[[nodiscard]] std::vector<u8> passengers_packet(i32 cart, std::span<const i32> riders) {
    io::ByteWriter writer;
    net::write_varint(writer, cart);
    net::write_varint(writer, static_cast<i32>(riders.size()));
    for (const i32 rider : riders) {
        net::write_varint(writer, rider);
    }
    return writer.take();
}

[[nodiscard]] std::tuple<i32, i32, i32> key_of(BlockPos pos) noexcept {
    return {pos.x, pos.y, pos.z};
}

}  // namespace

RailsSession::RailsSession(const registry::BlockRegistry& blocks,
                           const registry::Registries&    registries)
    : blocks_{&blocks},
      registries_{&registries},
      signals_{blocks, registries},
      rails_{blocks, signals_} {
    entity_types_ = registries.find("minecraft:entity_type");
    for (usize i = 0; i < types_.size(); ++i) {
        types_[i] = -1;
        if (entity_types_) {
            if (const auto id = registries.protocol_id(*entity_types_, gameplay::kMinecartTypes[i])) {
                types_[i] = *id;
            }
        }
    }
    carts_.reserve(64);
    events_.contacts.reserve(64);
}

// ── Blocks ──────────────────────────────────────────────────────────────────

void RailsSession::neighbour_changed(ServerLevel& level, BlockPos pos) {
    const gameplay::RailUpdate update = rails_.neighbour_changed(level, pos);
    if (update.broke) {
        rail_drops_.push_back(RailDrop{pos, blocks_->block_name(blocks_->block_of(update.was))});
    }
    if (update.power_changed) {
        // The power goes on through the block under the rail, and on a slope
        // through the one above: that is how it reaches the next rail up.
        level.mark_changed(pos.below());
        const auto shape = rails_.shape_of(level.block_at(pos));
        if (shape && gameplay::is_ascending(*shape)) {
            level.mark_changed(pos.above());
        }
    }
}

bool RailsSession::scheduled_tick(ServerLevel& level, BlockPos pos, std::string_view what) {
    if (what != "minecraft:detector_rail") {
        return false;
    }
    if (rails_.kind_of(level.block_at(pos)) != gameplay::RailKind::Detector) {
        return true;
    }
    if (pressed_.contains(key_of(pos))) {
        level.schedule_tick(pos, what, kDetectorPeriod, world::TickQueue::Block,
                            world::TickPriority::Normal);
    } else if (rails_.set_detector(level, pos, false)) {
        level.mark_changed(pos.below());
    }
    return true;
}

// ── Network thread ──────────────────────────────────────────────────────────

void RailsSession::request_shape(BlockPos pos, bool facing_east_west) {
    const std::scoped_lock lock{pending_mutex_};
    pending_shapes_.push_back(PendingShape{pos, facing_east_west});
}

bool RailsSession::request_cart(gameplay::MinecartKind kind, BlockPos rail,
                                registry::BlockStateId rail_state, f32 yaw) {
    if (!rails_.is_rail(rail_state)) {
        return false;
    }
    // A cart set on a rail sits on it: a sixteenth up, half a block more on
    // a slope (the wiki; the first tick puts it on the line anyway).
    const auto shape = rails_.shape_of(rail_state);
    const f64  lift  = shape && gameplay::is_ascending(*shape) ? 0.5 : 0.0;
    const std::scoped_lock lock{pending_mutex_};
    pending_carts_.push_back(PendingCart{
        kind,
        Vec3d{static_cast<f64>(rail.x) + 0.5,
              static_cast<f64>(rail.y) + gameplay::kMinecartRailHeight + lift,
              static_cast<f64>(rail.z) + 0.5},
        yaw});
    return true;
}

void RailsSession::request_touch(CartTouch touch) {
    const std::scoped_lock lock{pending_mutex_};
    pending_touches_.push_back(std::move(touch));
}

void RailsSession::request_input(RiderInput input) {
    const std::scoped_lock lock{pending_mutex_};
    pending_inputs_.push_back(input);
}

bool RailsSession::owns(i32 type) const noexcept {
    return std::ranges::find(types_, type) != types_.end() && type >= 0;
}

bool RailsSession::has_pending() const {
    const std::scoped_lock lock{pending_mutex_};
    return !pending_shapes_.empty() || !pending_carts_.empty() || !pending_touches_.empty() ||
           !pending_inputs_.empty() || !pending_restores_.empty();
}

i32 RailsSession::vehicle_of(i32 player_id) const {
    const auto it = vehicle_of_.find(player_id);
    return it == vehicle_of_.end() ? -1 : it->second;
}

// ── Carts ───────────────────────────────────────────────────────────────────

gameplay::MinecartLogic* RailsSession::logic_of(entity::EntityWorld& world,
                                                entity::EntityHandle handle) const {
    return dynamic_cast<gameplay::MinecartLogic*>(world.logic(handle));
}

void RailsSession::adopt(entity::EntityWorld& world, entity::EntityHandle handle,
                         const nbt::Tag* saved) {
    entity::EntityState* state = world.mutable_state(handle);
    if (state == nullptr) {
        return;
    }
    gameplay::MinecartBody body;
    for (usize i = 0; i < types_.size(); ++i) {
        if (types_[i] == state->type) {
            body.kind = static_cast<gameplay::MinecartKind>(i);
        }
    }
    Cart cart;
    cart.handle = handle;
    cart.kind   = body.kind;
    cart.saved  = nbt::Tag::make_compound();
    if (saved != nullptr) {
        cart.saved = *saved;
        if (const nbt::Tag* fuel = saved->find("Fuel")) {
            body.fuel = static_cast<i32>(fuel->as_i64(0));
        }
        if (const nbt::Tag* push = saved->find("PushX")) {
            body.push_x = push->as_f64(0.0);
        }
        if (const nbt::Tag* push = saved->find("PushZ")) {
            body.push_z = push->as_f64(0.0);
        }
        if (const nbt::Tag* fuse = saved->find("TNTFuse")) {
            body.fuse = static_cast<i32>(fuse->as_i64(-1));
        }
        if (const nbt::Tag* enabled = saved->find("Enabled")) {
            body.enabled = enabled->as_bool(true);
        }
        if (const nbt::Tag* custom = saved->find("CustomDisplayTile")) {
            body.custom_display = custom->as_bool(false);
        }
        if (const nbt::Tag* offset = saved->find("DisplayOffset")) {
            body.display_offset = static_cast<i32>(offset->as_i64(6));
        }
        if (const nbt::Tag* display = saved->find("DisplayState")) {
            const nbt::Tag* name = display->find("Name");
            const auto block = name ? blocks_->find_block(name->as_string()) : std::nullopt;
            if (block) {
                std::vector<std::pair<std::string_view, std::string_view>> pairs;
                if (const nbt::Tag* props = display->find("Properties");
                    props != nullptr && props->compound() != nullptr) {
                    for (const nbt::CompoundEntry& entry : *props->compound()) {
                        pairs.emplace_back(entry.name, entry.value.as_string());
                    }
                }
                const auto resolved = blocks_->state_for(*block, pairs);
                body.display_state  = static_cast<i32>(
                    (resolved ? *resolved : blocks_->default_state(*block)).value());
            }
        }
    }
    world.set_logic(handle, std::make_unique<gameplay::MinecartLogic>(rails_, events_, body));
    carts_[state->network_id] = std::move(cart);
}

void RailsSession::spawn_cart(entity::EntityWorld& world, gameplay::MinecartKind kind, Vec3d at,
                              f32 yaw, const RailsHost& host) {
    const auto spawned = world.spawn(gameplay::minecart_type(kind), at, net::Uuid{});
    if (!spawned) {
        OV_LOG_WARN("minecart: {} could not be spawned: {}", gameplay::minecart_type(kind),
                    entity::to_string(spawned.error()));
        return;
    }
    entity::EntityState* state = world.mutable_state(*spawned);
    const auto           id    = static_cast<u64>(static_cast<u32>(state->network_id));
    state->uuid = net::Uuid{mix(id ^ 0x5241494C5341u), mix(id + static_cast<u64>(++next_uuid_salt_))};
    state->yaw  = yaw;
    state->broadcast_position = state->position;
    state->broadcast_valid    = true;
    adopt(world, *spawned);
    spawn_packets(world, *state, host.broadcast);
}

void RailsSession::spawn_packets(entity::EntityWorld& world, const entity::EntityState& state,
                                 const RailsDeliver& deliver) const {
    net::SpawnEntity spawn;
    spawn.entity_id  = state.network_id;
    spawn.uuid       = state.uuid;
    spawn.type       = state.type;
    spawn.x          = state.position.x;
    spawn.y          = state.position.y;
    spawn.z          = state.position.z;
    spawn.yaw        = state.yaw;
    spawn.pitch      = state.pitch;
    spawn.velocity_x = state.velocity.x;
    spawn.velocity_y = state.velocity.y;
    spawn.velocity_z = state.velocity.z;
    deliver(net::clientbound::kSpawnEntity, net::encode_spawn_entity(spawn));

    const entity::EntityHandle handle = world.find(state.network_id);
    const auto* logic = handle == entity::kNoEntity
                            ? nullptr
                            : dynamic_cast<const gameplay::MinecartLogic*>(world.logic(handle));
    if (logic == nullptr) {
        return;
    }
    const gameplay::MinecartBody& body = logic->body();
    net::MetadataWriter           fields;
    if (body.custom_display) {
        fields.varint_value(kDisplayBlock, body.display_state);
        fields.varint_value(kDisplayOffset, body.display_offset);
        fields.boolean_value(kCustomDisplay, true);
    }
    if (body.kind == gameplay::MinecartKind::Furnace && body.fuel > 0) {
        fields.boolean_value(kFurnaceFuel, true);
    }
    if (body.damage > 0.0F) {
        fields.varint_value(kHurtTime, body.hurt_time);
        fields.varint_value(kHurtDirection, body.hurt_dir);
        fields.float_value(kDamage, body.damage);
    }
    if (!fields.empty()) {
        deliver(net::clientbound::kEntityMetadata,
                net::encode_entity_metadata(state.network_id, fields.take()));
    }
    if (const auto it = carts_.find(state.network_id); it != carts_.end() && it->second.rider >= 0) {
        const std::array<i32, 1> riders{it->second.rider};
        deliver(kSetPassengers, passengers_packet(state.network_id, riders));
    }
}

void RailsSession::send_shake(const entity::EntityState& state, const gameplay::MinecartBody& body,
                              const RailsHost& host) const {
    net::MetadataWriter fields;
    fields.varint_value(kHurtTime, body.hurt_time);
    fields.varint_value(kHurtDirection, body.hurt_dir);
    fields.float_value(kDamage, body.damage);
    host.broadcast(net::clientbound::kEntityMetadata,
                   net::encode_entity_metadata(state.network_id, fields.take()));
}

void RailsSession::mount(entity::EntityWorld& world, Cart& cart, i32 cart_id, i32 player_id,
                         const RailsHost& host) {
    (void)world;
    cart.rider              = player_id;
    vehicle_of_[player_id]  = cart_id;
    const std::array<i32, 1> riders{player_id};
    host.broadcast(kSetPassengers, passengers_packet(cart_id, riders));
}

void RailsSession::dismount(entity::EntityWorld& world, Cart& cart, i32 cart_id,
                            const RailsHost& host) {
    const i32 player = cart.rider;
    if (player < 0) {
        return;
    }
    cart.rider = -1;
    vehicle_of_.erase(player);
    controls_.erase(player);
    host.broadcast(kSetPassengers, passengers_packet(cart_id, {}));
    if (const entity::EntityState* state = world.state(cart.handle)) {
        // Where the cart is. Vanilla searches the eight cells round it for a
        // place to stand; that search is not implemented and is named.
        host.set_down(player, state->position);
    }
    if (auto* logic = logic_of(world, cart.handle)) {
        logic->body().ridden        = false;
        logic->body().rider_impulse = Vec3d{};
    }
}

void RailsSession::destroy(entity::EntityWorld& world, Cart& cart, entity::EntityState& state,
                           gameplay::MinecartLogic& logic, bool drops, const RailsHost& host) {
    dismount(world, cart, state.network_id, host);
    if (drops) {
        host.drop_item(state.position, gameplay::minecart_item(logic.body().kind), 1);
        // A container cart spills what it held.
        if (const nbt::Tag* items = cart.saved.find("Items");
            items != nullptr && items->list() != nullptr) {
            for (const nbt::Tag& stack : *items->list()) {
                const nbt::Tag* id    = stack.find("id");
                const nbt::Tag* count = stack.find("Count");
                if (id != nullptr && count != nullptr && count->as_i64(0) > 0) {
                    host.drop_item(state.position, id->as_string(),
                                   static_cast<i32>(count->as_i64(0)));
                }
            }
        }
    }
    state.removed = true;
}

void RailsSession::touch(entity::EntityWorld& world, const CartTouch& touch,
                         const RailsHost& host) {
    const auto it = carts_.find(touch.cart_id);
    if (it == carts_.end()) {
        return;
    }
    Cart&                    cart  = it->second;
    entity::EntityState*     state = world.mutable_state(cart.handle);
    gameplay::MinecartLogic* logic = logic_of(world, cart.handle);
    if (state == nullptr || logic == nullptr || state->removed) {
        return;
    }
    gameplay::MinecartBody& body = logic->body();
    if (touch.attack) {
        if (touch.creative) {
            // A creative player takes a cart away at once, and it drops nothing.
            destroy(world, cart, *state, *logic, false, host);
            return;
        }
        // Ten a point of damage; a bare hand is one point. Weapons are not
        // read: every hit counts as a fist. Named.
        body.hurt_dir  = -body.hurt_dir;
        body.hurt_time = 10;
        body.damage += 10.0F;
        send_shake(*state, body, host);
        if (body.damage > gameplay::kMinecartBreakDamage) {
            destroy(world, cart, *state, *logic, true, host);
        }
        return;
    }
    switch (body.kind) {
        case gameplay::MinecartKind::Rideable:
            if (!touch.sneaking && cart.rider < 0 && !vehicle_of_.contains(touch.player_id)) {
                mount(world, cart, touch.cart_id, touch.player_id, host);
            }
            break;
        case gameplay::MinecartKind::Furnace:
            if (touch.held == "minecraft:coal" || touch.held == "minecraft:charcoal") {
                body.fuel   = std::min(32000, body.fuel + gameplay::kFurnaceFuelPerCoal);
                body.push_x = state->position.x - touch.player_feet.x;
                body.push_z = state->position.z - touch.player_feet.z;
                if (!touch.creative && host.consume_held) {
                    host.consume_held(touch.player_id);
                }
            }
            break;
        default:
            OV_LOG_DEBUG("minecart: a {} opens no screen on this server yet",
                         gameplay::minecart_type(body.kind));
            break;
    }
}

void RailsSession::before_entity_tick(entity::EntityWorld& world, ServerLevel& level,
                                      WorldTicks& ticks, const RailsHost& host) {
    {
        const std::scoped_lock lock{pending_mutex_};
        shapes_now_.swap(pending_shapes_);
        carts_now_.swap(pending_carts_);
        touches_now_.swap(pending_touches_);
        inputs_now_.swap(pending_inputs_);
        restores_now_.swap(pending_restores_);
    }
    // ── persistence ── the carts that came back with their riders
    for (PendingRestore& restore : restores_now_) {
        if (host.player_ready && !host.player_ready(restore.player_id)) {
            if (++restore.waited < 1200) {
                const std::scoped_lock lock{pending_mutex_};
                pending_restores_.push_back(std::move(restore));
            } else {
                OV_LOG_WARN("minecart: player {} never arrived to ride the cart from their file "
                            "— the cart is dropped",
                            restore.player_id);
            }
            continue;
        }
        const nbt::Tag* entity = restore.root_vehicle.find("Entity");
        if (entity == nullptr) {
            continue;
        }
        const auto               uuid = uuid_of(*entity);
        std::optional<net::Uuid> attach;
        if (const nbt::Tag* ints = restore.root_vehicle.find("Attach")) {
            nbt::Tag wrap = nbt::Tag::make_compound();
            put(wrap, "UUID", *ints);
            attach = uuid_of(wrap);
        }
        // The cart may be here already: a world saved with it in its chunk.
        i32 cart_id = -1;
        for (const auto& [id, cart] : carts_) {
            const entity::EntityState* state = world.state(cart.handle);
            if (state != nullptr && uuid && state->uuid == *uuid && cart.rider < 0) {
                cart_id = id;
                break;
            }
        }
        if (cart_id < 0) {
            const auto handle = adopt_saved(world, *entity);
            if (!handle) {
                OV_LOG_WARN("minecart: the vehicle in player {}'s file is not a cart this server "
                            "runs — not put back",
                            restore.player_id);
                continue;
            }
            const entity::EntityState* state = world.state(*handle);
            cart_id                          = state->network_id;
            spawn_packets(world, *state, host.broadcast);
        }
        if (attach && uuid && *attach != *uuid) {
            OV_LOG_WARN("minecart: player {} sat in an entity riding the cart, not in the cart — "
                        "the cart is back, the player is not in it",
                        restore.player_id);
            continue;
        }
        const auto cart = carts_.find(cart_id);
        if (cart != carts_.end() && !vehicle_of_.contains(restore.player_id)) {
            mount(world, cart->second, cart_id, restore.player_id, host);
            OV_LOG_INFO("minecart: player {} is back in their cart", restore.player_id);
        }
    }
    restores_now_.clear();
    for (const PendingShape& shape : shapes_now_) {
        const registry::BlockStateId state = level.block_at(shape.pos);
        if (!rails_.is_rail(state)) {
            continue;
        }
        const registry::BlockStateId start =
            rails_.placement_state(level, shape.pos, state, shape.east_west);
        if (start != state) {
            level.set_block(shape.pos, start);
        }
        rails_.on_placed(level, shape.pos);
    }
    if (!shapes_now_.empty()) {
        (void)ticks.settle_changes(level);
    }
    for (const PendingCart& cart : carts_now_) {
        spawn_cart(world, cart.kind, cart.at, cart.yaw, host);
    }
    for (const CartTouch& t : touches_now_) {
        touch(world, t, host);
    }
    for (const RiderInput& input : inputs_now_) {
        controls_[input.player_id] = input;
        if ((input.flags & 0x02U) != 0) {
            if (const auto vehicle = vehicle_of_.find(input.player_id); vehicle != vehicle_of_.end()) {
                if (const auto cart = carts_.find(vehicle->second); cart != carts_.end()) {
                    dismount(world, cart->second, vehicle->second, host);
                }
            }
        }
    }
    shapes_now_.clear();
    carts_now_.clear();
    touches_now_.clear();
    inputs_now_.clear();

    // A rider's controls, turned by where they look: forward at yaw 0 is +z.
    for (auto& [id, cart] : carts_) {
        gameplay::MinecartLogic* logic = logic_of(world, cart.handle);
        if (logic == nullptr) {
            continue;
        }
        gameplay::MinecartBody& body = logic->body();
        body.ridden                  = cart.rider >= 0;
        body.rider_impulse           = Vec3d{};
        if (cart.rider >= 0) {
            if (const auto it = controls_.find(cart.rider); it != controls_.end()) {
                const f64 yaw = static_cast<f64>(it->second.yaw) * std::numbers::pi / 180.0;
                const f64 f   = static_cast<f64>(it->second.forward);
                const f64 s   = static_cast<f64>(it->second.sideways);
                body.rider_impulse =
                    Vec3d{(s * std::cos(yaw) - f * std::sin(yaw)) * kRiderPush, 0.0,
                          (f * std::cos(yaw) + s * std::sin(yaw)) * kRiderPush};
            }
        }
    }
    events_.contacts.clear();
}

RailsStats RailsSession::after_entity_tick(entity::EntityWorld& world, ServerLevel& level,
                                           WorldTicks& ticks, const RailsHost& host) {
    RailsStats stats;
    for (const RailDrop& drop : rail_drops_) {
        host.drop_item(Vec3d{static_cast<f64>(drop.pos.x) + 0.5, static_cast<f64>(drop.pos.y) + 0.25,
                             static_cast<f64>(drop.pos.z) + 0.5},
                       drop.item, 1);
        ++stats.broken;
    }
    rail_drops_.clear();
    decltype(pressed_) pressed;
    bool               wrote = false;
    for (const gameplay::MinecartEvents::Contact& contact : events_.contacts) {
        const gameplay::MinecartContact& c = contact.contact;
        if (c.kind == gameplay::RailKind::Detector) {
            pressed[key_of(c.rail)] = contact.network_id;
            if (rails_.set_detector(level, c.rail, true)) {
                level.mark_changed(c.rail.below());
                level.schedule_tick(c.rail, "minecraft:detector_rail", kDetectorPeriod,
                                    world::TickQueue::Block, world::TickPriority::Normal);
                ++stats.detector_changes;
                wrote = true;
            }
        } else if (c.kind == gameplay::RailKind::Activator && c.powered) {
            // A powered activator rail throws a rider off (the wiki).
            if (const auto it = carts_.find(contact.network_id);
                it != carts_.end() && it->second.rider >= 0) {
                dismount(world, it->second, contact.network_id, host);
            }
        }
    }
    pressed_.swap(pressed);

    for (auto it = carts_.begin(); it != carts_.end();) {
        Cart&                      cart  = it->second;
        const entity::EntityState* state = world.state(cart.handle);
        if (state == nullptr || state->removed) {
            if (cart.rider >= 0) {
                vehicle_of_.erase(cart.rider);
                controls_.erase(cart.rider);
                host.broadcast(kSetPassengers, passengers_packet(it->first, {}));
            }
            it = carts_.erase(it);
            continue;
        }
        if (cart.rider >= 0 && !host.carry_rider(cart.rider, state->position)) {
            // The rider left the server: the cart is free again.
            vehicle_of_.erase(cart.rider);
            controls_.erase(cart.rider);
            cart.rider = -1;
            host.broadcast(kSetPassengers, passengers_packet(it->first, {}));
        }
        if (const gameplay::MinecartLogic* logic = logic_of(world, cart.handle);
            logic != nullptr && logic->body().kind == gameplay::MinecartKind::Furnace) {
            const bool burning = logic->body().fuel > 0;
            if (burning != cart.fuel_sent) {
                cart.fuel_sent = burning;
                net::MetadataWriter fields;
                fields.boolean_value(kFurnaceFuel, burning);
                host.broadcast(net::clientbound::kEntityMetadata,
                               net::encode_entity_metadata(state->network_id, fields.take()));
            }
        }
        ++it;
    }
    if (wrote) {
        (void)ticks.settle_changes(level);
    }
    stats.carts = carts_.size();
    return stats;
}

i32 RailsSession::container_reading(const Cart& cart, gameplay::MinecartKind kind) const {
    const usize slots = gameplay::minecart_slots(kind);
    if (slots == 0) {
        return 0;
    }
    f32  fullness = 0.0F;
    bool any      = false;
    if (const nbt::Tag* items = cart.saved.find("Items");
        items != nullptr && items->list() != nullptr) {
        for (const nbt::Tag& stack : *items->list()) {
            const nbt::Tag* count = stack.find("Count");
            const i64       n     = count != nullptr ? count->as_i64(0) : 0;
            if (n > 0) {
                any = true;
                // Every item is taken to stack to 64. Named: an ender pearl
                // reads four times too light.
                fullness += static_cast<f32>(n) / 64.0F;
            }
        }
    }
    return gameplay::Redstone::container_reading(fullness / static_cast<f32>(slots), any);
}

i32 RailsSession::comparator_signal(const world::LevelView& level, BlockPos pos) const {
    const registry::BlockStateId state = level.block_at(pos);
    if (rails_.kind_of(state) != gameplay::RailKind::Detector) {
        return -1;
    }
    if (!rails_.powered(state)) {
        return 0;
    }
    const auto pressed = pressed_.find(key_of(pos));
    if (pressed == pressed_.end()) {
        return 0;
    }
    const auto cart = carts_.find(pressed->second);
    if (cart == carts_.end()) {
        return 0;
    }
    return container_reading(cart->second, cart->second.kind);
}

void RailsSession::forget_player(entity::EntityWorld& world, i32 player_id,
                                 const RailsHost& host) {
    const auto vehicle = vehicle_of_.find(player_id);
    if (vehicle == vehicle_of_.end()) {
        controls_.erase(player_id);
        return;
    }
    if (const auto cart = carts_.find(vehicle->second); cart != carts_.end()) {
        dismount(world, cart->second, vehicle->second, host);
    }
}

// ── Persistence ─────────────────────────────────────────────────────────────

nbt::Tag RailsSession::cart_nbt(const entity::EntityState&   state,
                                const gameplay::MinecartBody& body) const {
    nbt::Tag tag = nbt::Tag::make_compound();
    if (const auto it = carts_.find(state.network_id); it != carts_.end()) {
        tag = it->second.saved;
    }
    put(tag, "id", nbt::Tag{std::string{gameplay::minecart_type(body.kind)}});
    put(tag, "Pos", doubles({state.position.x, state.position.y, state.position.z}));
    put(tag, "Motion", doubles({state.velocity.x, state.velocity.y, state.velocity.z}));
    nbt::Tag rotation = nbt::Tag::make_list(nbt::TagType::Float);
    (void)rotation.push(nbt::Tag{state.yaw});
    (void)rotation.push(nbt::Tag{state.pitch});
    put(tag, "Rotation", std::move(rotation));
    put(tag, "UUID", uuid_tag(state.uuid));
    put(tag, "OnGround", nbt::Tag::make_bool(state.on_ground));
    if (tag.find("FallDistance") == nullptr) {
        put(tag, "FallDistance", nbt::Tag{0.0F});
        put(tag, "Fire", nbt::Tag{i16{-1}});
        put(tag, "Air", nbt::Tag{i16{300}});
        put(tag, "Invulnerable", nbt::Tag::make_bool(false));
        put(tag, "PortalCooldown", nbt::Tag{i32{0}});
    }
    switch (body.kind) {
        case gameplay::MinecartKind::Furnace:
            put(tag, "Fuel", nbt::Tag{static_cast<i16>(std::clamp(body.fuel, 0, 32767))});
            put(tag, "PushX", nbt::Tag{body.push_x});
            put(tag, "PushZ", nbt::Tag{body.push_z});
            break;
        case gameplay::MinecartKind::Tnt: put(tag, "TNTFuse", nbt::Tag{i32{body.fuse}}); break;
        case gameplay::MinecartKind::Hopper:
            put(tag, "Enabled", nbt::Tag::make_bool(body.enabled));
            break;
        default: break;
    }
    if (body.custom_display) {
        put(tag, "CustomDisplayTile", nbt::Tag::make_bool(true));
        put(tag, "DisplayOffset", nbt::Tag{i32{body.display_offset}});
        const registry::BlockStateId display{static_cast<u16>(body.display_state)};
        const registry::BlockId      block = blocks_->block_of(display);
        nbt::Tag                     state_tag = nbt::Tag::make_compound();
        put(state_tag, "Name", nbt::Tag{std::string{blocks_->block_name(block)}});
        const auto properties = blocks_->properties(block);
        if (!properties.empty()) {
            nbt::Tag props = nbt::Tag::make_compound();
            for (const registry::PropertyView& property : properties) {
                put(props, property.name,
                    nbt::Tag{std::string{blocks_->property_value(display, property)}});
            }
            put(state_tag, "Properties", std::move(props));
        }
        put(tag, "DisplayState", std::move(state_tag));
    }
    return tag;
}

std::optional<entity::EntityHandle> RailsSession::adopt_saved(entity::EntityWorld& world,
                                                              const nbt::Tag&      compound) {
    const nbt::Tag* id   = compound.find("id");
    const auto      kind = id != nullptr ? gameplay::minecart_kind(id->as_string()) : std::nullopt;
    if (!kind) {
        return std::nullopt;
    }
    const Vec3d at{list_f64(compound, "Pos", 0, 0.0), list_f64(compound, "Pos", 1, 0.0),
                   list_f64(compound, "Pos", 2, 0.0)};
    const auto spawned =
        world.spawn(gameplay::minecart_type(*kind), at, uuid_of(compound).value_or(net::Uuid{}));
    if (!spawned) {
        OV_LOG_WARN("minecart: a {} read from disk cannot be spawned: {}",
                    gameplay::minecart_type(*kind), entity::to_string(spawned.error()));
        return std::nullopt;
    }
    entity::EntityState* state = world.mutable_state(*spawned);
    state->velocity            = Vec3d{list_f64(compound, "Motion", 0, 0.0),
                                       list_f64(compound, "Motion", 1, 0.0),
                                       list_f64(compound, "Motion", 2, 0.0)};
    state->yaw                 = static_cast<f32>(list_f64(compound, "Rotation", 0, 0.0));
    state->pitch               = static_cast<f32>(list_f64(compound, "Rotation", 1, 0.0));
    if (const nbt::Tag* ground = compound.find("OnGround")) {
        state->on_ground = ground->as_bool(false);
    }
    state->broadcast_position = state->position;
    state->broadcast_valid    = true;
    // Everything it was saved with — a `Passengers` list included: a mob
    // riding a cart vanilla saved is carried inside it, not run.
    adopt(world, *spawned, &compound);
    return *spawned;
}

std::optional<nbt::Tag> RailsSession::save_entity(entity::EntityWorld& world,
                                                  entity::EntityHandle handle) const {
    const entity::EntityState* state = world.state(handle);
    const auto* logic = dynamic_cast<const gameplay::MinecartLogic*>(world.logic(handle));
    if (state == nullptr || logic == nullptr || state->removed) {
        return std::nullopt;
    }
    return cart_nbt(*state, logic->body());
}

void RailsSession::release(entity::EntityWorld& world, entity::EntityHandle handle) {
    const entity::EntityState* state = world.state(handle);
    if (state == nullptr) {
        return;
    }
    const auto it = carts_.find(state->network_id);
    if (it == carts_.end()) {
        return;
    }
    // A player rider keeps the chunks round them loaded, so a ridden cart
    // should not be here; if one is, the rider is let go without a packet —
    // the Remove Entities that follows takes the cart from every client.
    if (it->second.rider >= 0) {
        vehicle_of_.erase(it->second.rider);
        controls_.erase(it->second.rider);
    }
    for (auto press = pressed_.begin(); press != pressed_.end();) {
        press = press->second == state->network_id ? pressed_.erase(press) : std::next(press);
    }
    carts_.erase(it);
}

// ── persistence: RootVehicle ────────────────────────────────────────────────

std::optional<TakenVehicle> RailsSession::take_vehicle(entity::EntityWorld& world, i32 player_id) {
    {
        // Left before their cart was put back: it goes back into the file.
        const std::scoped_lock lock{pending_mutex_};
        for (auto it = pending_restores_.begin(); it != pending_restores_.end(); ++it) {
            if (it->player_id == player_id) {
                TakenVehicle taken{std::move(it->root_vehicle), -1};
                pending_restores_.erase(it);
                return taken;
            }
        }
    }
    const auto vehicle = vehicle_of_.find(player_id);
    if (vehicle == vehicle_of_.end()) {
        return std::nullopt;
    }
    const i32  cart_id = vehicle->second;
    const auto cart    = carts_.find(cart_id);
    if (cart == carts_.end()) {
        vehicle_of_.erase(vehicle);
        return std::nullopt;
    }
    const entity::EntityState*     state = world.state(cart->second.handle);
    const gameplay::MinecartLogic* logic = logic_of(world, cart->second.handle);
    if (state == nullptr || logic == nullptr) {
        return std::nullopt;
    }
    // The cart as it is saved, its rider not among its passengers: the
    // player is the one holding it.
    TakenVehicle taken;
    taken.root_vehicle = nbt::Tag::make_compound();
    put(taken.root_vehicle, "Attach", uuid_tag(state->uuid));
    put(taken.root_vehicle, "Entity", cart_nbt(*state, logic->body()));
    taken.cart_id = cart_id;
    const entity::EntityHandle handle = cart->second.handle;
    for (auto press = pressed_.begin(); press != pressed_.end();) {
        press = press->second == cart_id ? pressed_.erase(press) : std::next(press);
    }
    carts_.erase(cart);
    vehicle_of_.erase(player_id);
    controls_.erase(player_id);
    (void)world.remove(handle);
    return taken;
}

void RailsSession::request_restore(i32 player_id, nbt::Tag root_vehicle) {
    const std::scoped_lock lock{pending_mutex_};
    pending_restores_.push_back(PendingRestore{player_id, std::move(root_vehicle), 0});
}

}  // namespace ov::server
