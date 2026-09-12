// ── tame ── See taming.hpp; the numbers are in docs/provenance/apprivoisement.md.
#define OV_LOG_CATEGORY "server"

#include "taming.hpp"

#include "entity_nbt.hpp"

#include "ov/base/log.hpp"
#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/tame.hpp"
#include "ov/io/byte_writer.hpp"
#include "ov/protocol/varint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace ov::server {
namespace {

/// Set Passengers and Set Equipment, protocol 763 (the archive's numbering;
/// Set Passengers is the rails session's, captured on a real server).
constexpr i32 kSetPassengers = 0x59;
constexpr i32 kSetEquipment  = 0x55;
/// The chest slot, where a horse wears its armour (EquipmentSlot order:
/// main hand, off hand, feet, legs, chest, head).
constexpr u8 kChestSlot = 4;
/// The interaction reach, as husbandry.cpp's: six blocks, eyes to the box.
constexpr f64 kReach = 6.0;
/// An ocelot trusts only a player within three blocks (the wiki).
constexpr f64 kOcelotReach = 3.0;

/// `minecraft:cat_variant` in its wire order. Measured: `tabby` sends 0 and
/// `black`, the default, sends nothing; test_tame_server.cpp checks the rest
/// against the report's protocol ids.
constexpr std::array<std::string_view, 11> kCatVariants{
    "tabby", "black", "red", "siamese", "british_shorthair", "calico",
    "persian", "ragdoll", "white", "jellie", "all_black"};

[[nodiscard]] f64 box_distance_sq(const entity::EntityState& state, Vec3d point) noexcept {
    const f64  half = static_cast<f64>(state.width) * 0.5;
    const auto axis = [](f64 v, f64 lo, f64 hi) { return v < lo ? lo - v : (v > hi ? v - hi : 0.0); };
    const f64  dx   = axis(point.x, state.position.x - half, state.position.x + half);
    const f64  dy = axis(point.y, state.position.y, state.position.y + static_cast<f64>(state.height));
    const f64  dz = axis(point.z, state.position.z - half, state.position.z + half);
    return dx * dx + dy * dy + dz * dz;
}

[[nodiscard]] gameplay::Mob* mob_of(entity::EntityWorld& world, entity::EntityHandle handle) {
    return dynamic_cast<gameplay::Mob*>(world.logic(handle));
}

[[nodiscard]] std::vector<u8> passengers_packet(i32 vehicle, std::span<const i32> riders) {
    io::ByteWriter writer;
    net::write_varint(writer, vehicle);
    net::write_varint(writer, static_cast<i32>(riders.size()));
    for (const i32 rider : riders) {
        net::write_varint(writer, rider);
    }
    return writer.take();
}

[[nodiscard]] bool pet(gameplay::TameFamily family) noexcept {
    return family == gameplay::TameFamily::Wolf || family == gameplay::TameFamily::Cat ||
           family == gameplay::TameFamily::Parrot;
}

[[nodiscard]] bool equine(gameplay::TameFamily family) noexcept {
    return family == gameplay::TameFamily::Horse || family == gameplay::TameFamily::Llama;
}

[[nodiscard]] bool chest_bearer(std::string_view type) noexcept {
    return type == "minecraft:donkey" || type == "minecraft:mule" || type == "minecraft:llama" ||
           type == "minecraft:trader_llama";
}

[[nodiscard]] std::optional<i8> carpet_colour(std::string_view item) noexcept {
    constexpr std::string_view kPrefix = "minecraft:";
    constexpr std::string_view kSuffix = "_carpet";
    if (!item.starts_with(kPrefix) || !item.ends_with(kSuffix)) {
        return std::nullopt;
    }
    const std::string_view colour =
        item.substr(kPrefix.size(), item.size() - kPrefix.size() - kSuffix.size());
    const auto names = gameplay::colour_names();
    for (usize i = 0; i < names.size(); ++i) {
        if (names[i] == colour) {
            return static_cast<i8>(i);
        }
    }
    return std::nullopt;
}

[[nodiscard]] nbt::Tag item_compound(std::string_view id) {
    nbt::Tag stack = nbt::Tag::make_compound();
    (void)stack.put("id", nbt::Tag{std::string{id}});
    (void)stack.put("Count", nbt::Tag{i8{1}});
    return stack;
}

[[nodiscard]] std::string_view item_id_of(const nbt::Tag* stack) noexcept {
    if (stack == nullptr) {
        return {};
    }
    const nbt::Tag* id = stack->find("id");
    return id == nullptr ? std::string_view{} : id->as_string();
}

constexpr std::string_view kMaxHealth = "minecraft:generic.max_health";
constexpr std::string_view kSpeed     = "minecraft:generic.movement_speed";
constexpr std::string_view kJump      = "minecraft:horse.jump_strength";

/// Put one attribute's base into an `Attributes` list, keeping the others.
void put_attribute(nbt::Tag& list, std::string_view name, f64 base) {
    if (std::vector<nbt::Tag>* entries = list.list()) {
        for (nbt::Tag& entry : *entries) {
            const nbt::Tag* key = entry.find("Name");
            if (key != nullptr && key->as_string() == name) {
                (void)entry.put("Base", nbt::Tag{base});
                return;
            }
        }
    }
    nbt::Tag entry = nbt::Tag::make_compound();
    (void)entry.put("Base", nbt::Tag{base});
    (void)entry.put("Name", nbt::Tag{std::string{name}});
    (void)list.push(std::move(entry));
}

[[nodiscard]] std::optional<f64> read_attribute(const nbt::Tag& compound, std::string_view name) {
    const nbt::Tag* list = compound.find("Attributes");
    if (list == nullptr || list->list() == nullptr) {
        return std::nullopt;
    }
    for (const nbt::Tag& entry : *list->list()) {
        const nbt::Tag* key = entry.find("Name");
        if (key != nullptr && key->as_string() == name) {
            if (const nbt::Tag* base = entry.find("Base")) {
                return base->as_f64();
            }
        }
    }
    return std::nullopt;
}

void put_owner(nbt::Tag& out, const gameplay::TameState& tame) {
    if (tame.owner) {
        (void)out.put("Owner", uuid_tag(*tame.owner));
    } else {
        (void)out.erase("Owner");
    }
}

}  // namespace

std::span<const std::string_view> Taming::cat_variants() noexcept { return kCatVariants; }

Taming::Taming(const registry::Registries& registries) : registries_{&registries} {
    items_      = registries.find("minecraft:item");
    types_      = registries.find("minecraft:entity_type");
    attributes_ = registries.find("minecraft:attribute");
    world_.cat_type          = type_id("minecraft:cat");
    world_.ocelot_type       = type_id("minecraft:ocelot");
    world_.creeper_type      = type_id("minecraft:creeper");
    world_.sheep_type        = type_id("minecraft:sheep");
    world_.rabbit_type       = type_id("minecraft:rabbit");
    world_.fox_type          = type_id("minecraft:fox");
    world_.skeleton_type     = type_id("minecraft:skeleton");
    world_.llama_type        = type_id("minecraft:llama");
    world_.trader_llama_type = type_id("minecraft:trader_llama");
    world_.wolf_type         = type_id("minecraft:wolf");
    pending_.reserve(16);
    working_.reserve(16);
    owners_.reserve(16);
    events_.reserve(16);
    players_.reserve(16);
}

std::string_view Taming::type_name(i32 type) const {
    return types_ ? registries_->entry_of(*types_, type) : std::string_view{};
}

i32 Taming::item_id(std::string_view name) const {
    return items_ ? registries_->protocol_id(*items_, name).value_or(-1) : -1;
}

std::string_view Taming::item_name(i32 id) const {
    return items_ ? registries_->entry_of(*items_, id) : std::string_view{};
}

i32 Taming::type_id(std::string_view name) const {
    return types_ ? registries_->protocol_id(*types_, name).value_or(-1) : -1;
}

// ── The network thread ──────────────────────────────────────────────────────

void Taming::queue_interact(i32 player, i32 entity, net::Hand hand, bool sneaking) {
    const std::scoped_lock lock{mutex_};
    pending_.push_back(Click{player, entity, hand, sneaking});
}

void Taming::queue_vehicle_move(i32 player, Vec3d at, f32 yaw, f32 pitch) {
    const std::scoped_lock lock{mutex_};
    moves_[player] = Move{at, yaw, pitch};
}

void Taming::queue_input(i32 player, u8 flags) {
    if ((flags & 0x02U) == 0) {
        return;
    }
    const std::scoped_lock lock{mutex_};
    getting_off_.push_back(player);
}

i32 Taming::vehicle_of(i32 player) const {
    const auto it = riding_.find(player);
    return it == riding_.end() ? 0 : it->second;
}

// ── Metadata ────────────────────────────────────────────────────────────────

void Taming::spawn_metadata(entity::EntityWorld& world, const entity::EntityState& state,
                            net::MetadataWriter& fields) const {
    const gameplay::MobBrain* brain = gameplay::mob_brain_of(world, world.find(state.network_id));
    if (brain == nullptr || !brain->tame.active()) {
        return;
    }
    namespace md                  = net::metadata;
    const gameplay::TameState& t  = brain->tame;
    const std::string_view     ty = type_name(state.type);
    using F                       = gameplay::TameFamily;
    if (pet(t.family)) {
        const i8 flags = static_cast<i8>((t.sitting ? md::kTameFlagSitting : 0) |
                                         (t.tame ? md::kTameFlagTame : 0));
        if (flags != 0) {
            fields.byte_value(md::kTameFlags, flags);
        }
        if (t.owner) {
            fields.optional_uuid_value(md::kTameOwner, t.owner);
        }
    }
    switch (t.family) {
    case F::Wolf:
        if (t.collar != 14) {
            fields.varint_value(md::kWolfCollar, t.collar);
        }
        if (t.anger > 0) {
            fields.varint_value(md::kWolfAnger, t.anger);
        }
        break;
    case F::Cat:
        if (t.variant != md::kCatDefaultVariant) {
            fields.cat_variant_value(md::kCatVariant, t.variant);
        }
        if (t.collar != 14) {
            fields.varint_value(md::kCatCollar, t.collar);
        }
        break;
    case F::Parrot:
        if (t.variant != 0) {
            fields.varint_value(md::kParrotVariant, t.variant);
        }
        break;
    case F::Ocelot:
        if (t.trusting) {
            fields.boolean_value(md::kOcelotTrusting, true);
        }
        break;
    case F::Horse:
    case F::Llama: {
        const i8 flags = static_cast<i8>((t.tame ? md::kHorseFlagTame : 0) |
                                         (t.saddled ? md::kHorseFlagSaddled : 0) |
                                         (t.bred ? md::kHorseFlagBred : 0));
        if (flags != 0) {
            fields.byte_value(md::kHorseFlags, flags);
        }
        if (ty == "minecraft:horse" && t.variant != 0) {
            fields.varint_value(md::kHorseVariant, t.variant);
        }
        if (chest_bearer(ty) && t.chested) {
            fields.boolean_value(md::kChested, true);
        }
        if (t.family == F::Llama) {
            if (t.strength != 0) {
                fields.varint_value(md::kLlamaStrength, t.strength);
            }
            if (const auto colour = carpet_colour(t.armour)) {
                fields.varint_value(md::kLlamaCarpet, *colour);
            }
            if (t.variant != 0) {
                fields.varint_value(md::kLlamaVariant, t.variant);
            }
        }
        break;
    }
    case F::Rabbit:
        if (t.variant != 0) {
            fields.varint_value(md::kRabbitType, t.variant);
        }
        break;
    case F::Fox:
        if (t.variant != 0) {
            fields.varint_value(md::kFoxType, t.variant);
        }
        if (t.flag) {
            fields.byte_value(md::kFoxFlags, md::kFoxFlagSleeping);
        }
        break;
    case F::Turtle:
        if (t.flag) {
            fields.boolean_value(md::kTurtleHasEgg, true);
        }
        break;
    case F::Bee:
        if (t.flag) {
            fields.byte_value(md::kBeeFlags, md::kBeeFlagNectar);
        }
        break;
    case F::Goat:
        if (t.flag) {
            fields.boolean_value(md::kGoatScreaming, true);
        }
        if (!t.left_horn) {
            fields.boolean_value(md::kGoatLeftHorn, false);
        }
        if (!t.right_horn) {
            fields.boolean_value(md::kGoatRightHorn, false);
        }
        break;
    case F::Camel:
        if (brain->animal.saddled) {
            fields.byte_value(md::kHorseFlags, md::kHorseFlagSaddled);
        }
        break;
    case F::None:
        break;
    }
}

void Taming::send_metadata(const entity::EntityState& state, const gameplay::MobBrain& brain,
                           const TamingDeliver& deliver) const {
    namespace md                 = net::metadata;
    const gameplay::TameState& t = brain.tame;
    const std::string_view     ty = type_name(state.type);
    using F                      = gameplay::TameFamily;
    // An update carries its fields even at their defaults: a wolf that stood
    // up has to be told the sitting bit is gone.
    net::MetadataWriter fields;
    fields.float_value(md::kHealth, state.health);
    if (pet(t.family)) {
        fields.byte_value(md::kTameFlags, static_cast<i8>((t.sitting ? md::kTameFlagSitting : 0) |
                                                          (t.tame ? md::kTameFlagTame : 0)));
        fields.optional_uuid_value(md::kTameOwner, t.owner);
    }
    switch (t.family) {
    case F::Wolf:
        fields.varint_value(md::kWolfCollar, t.collar);
        fields.varint_value(md::kWolfAnger, t.anger);
        break;
    case F::Cat:
        fields.cat_variant_value(md::kCatVariant, t.variant);
        fields.varint_value(md::kCatCollar, t.collar);
        break;
    case F::Ocelot:
        fields.boolean_value(md::kOcelotTrusting, t.trusting);
        break;
    case F::Horse:
    case F::Llama:
        fields.byte_value(md::kHorseFlags, static_cast<i8>((t.tame ? md::kHorseFlagTame : 0) |
                                                           (t.saddled ? md::kHorseFlagSaddled : 0) |
                                                           (t.bred ? md::kHorseFlagBred : 0)));
        if (chest_bearer(ty)) {
            fields.boolean_value(md::kChested, t.chested);
        }
        if (t.family == F::Llama) {
            fields.varint_value(md::kLlamaCarpet, carpet_colour(t.armour).value_or(i8{-1}));
        }
        break;
    case F::Parrot:
    case F::Rabbit:
    case F::Fox:
    case F::Turtle:
    case F::Bee:
    case F::Goat:
    case F::Camel:
    case F::None:
        break;
    }
    deliver(net::clientbound::kEntityMetadata,
            net::encode_entity_metadata(state.network_id, fields.take()));
}

void Taming::attributes(entity::EntityWorld& world, const entity::EntityState& state,
                        std::vector<net::AttributeValue>& values) const {
    const gameplay::MobBrain* brain = gameplay::mob_brain_of(world, world.find(state.network_id));
    if (brain == nullptr || !brain->tame.active()) {
        return;
    }
    const gameplay::TameState& t = brain->tame;
    for (net::AttributeValue& value : values) {
        if (value.name == kMaxHealth) {
            value.value = static_cast<f64>(state.max_health);
        } else if (t.stats.drawn() && value.name == kSpeed) {
            value.value = t.stats.speed;
        } else if (t.stats.drawn() && value.name == kJump) {
            value.value = t.stats.jump;
        }
    }
}

void Taming::send_equipment(const entity::EntityState& state, const gameplay::TameState& tame,
                            const TamingDeliver& deliver) const {
    io::ByteWriter writer;
    net::write_varint(writer, state.network_id);
    writer.write_u8(kChestSlot);
    net::ItemStack stack;
    if (!tame.armour.empty()) {
        stack.item_id = item_id(tame.armour);
        stack.count   = 1;
    }
    net::write_slot(writer, stack);
    deliver(kSetEquipment, writer.take());
}

void Taming::spawn_extra(entity::EntityWorld& world, const entity::EntityState& state,
                         const TamingDeliver& deliver) const {
    const gameplay::MobBrain* brain = gameplay::mob_brain_of(world, world.find(state.network_id));
    if (brain == nullptr || !brain->tame.active()) {
        return;
    }
    const gameplay::TameState& t = brain->tame;
    if (t.family == gameplay::TameFamily::Horse && !t.armour.empty()) {
        send_equipment(state, t, deliver);
    }
    if (t.rider != 0) {
        const std::array<i32, 1> riders{t.rider};
        deliver(kSetPassengers, passengers_packet(state.network_id, riders));
    }
}

// ── Riding ──────────────────────────────────────────────────────────────────

void Taming::mount(entity::EntityWorld& world, entity::EntityHandle handle, i32 player,
                   const TamingDeliver& deliver) {
    entity::EntityState* state = world.mutable_state(handle);
    gameplay::Mob*       mob   = mob_of(world, handle);
    if (state == nullptr || mob == nullptr) {
        return;
    }
    gameplay::MobBrain& brain = mob->mutable_brain();
    brain.tame.rider          = player;
    brain.tame.ridden_for     = 0;
    brain.follower.clear();
    brain.wants_move    = false;
    riding_[player]     = state->network_id;
    OV_LOG_DEBUG("tame: tick {}: {} mounts {} ({})", now_, player, state->network_id,
                 type_name(state->type));
    const std::array<i32, 1> riders{player};
    deliver(kSetPassengers, passengers_packet(state->network_id, riders));
}

void Taming::dismount(entity::EntityWorld& world, i32 player, std::string_view why,
                      const TamingHost& host, const TamingDeliver& deliver) {
    const auto it = riding_.find(player);
    if (it == riding_.end()) {
        return;
    }
    const i32 vehicle = it->second;
    riding_.erase(it);
    OV_LOG_DEBUG("tame: tick {}: {} off {} ({})", now_, player, vehicle, why);
    const entity::EntityHandle handle = world.find(vehicle);
    entity::EntityState*       state  = world.mutable_state(handle);
    if (gameplay::Mob* mob = mob_of(world, handle)) {
        mob->mutable_brain().tame.rider = 0;
    }
    deliver(kSetPassengers, passengers_packet(vehicle, {}));
    if (state != nullptr && host.set_down) {
        // Beside the animal. Vanilla searches round it for a place to stand;
        // that search is not implemented and is named (as the carts').
        host.set_down(player, state->position +
                                  Vec3d{static_cast<f64>(state->width) * 0.5 + 0.5, 0.0, 0.0});
    }
}

void Taming::forget_player(entity::EntityWorld& world, i32 player, const TamingHost& host,
                           const TamingDeliver& deliver) {
    dismount(world, player, "left", host, deliver);
    tracks_.erase(player);
}

// ── Clicks ──────────────────────────────────────────────────────────────────

void Taming::interact(entity::EntityWorld& world, const Click& click, const TamingHost& host,
                      const TamingDeliver& deliver, TamingStats& stats) {
    using F                           = gameplay::TameFamily;
    const entity::EntityHandle handle = world.find(click.entity);
    entity::EntityState*       state  = world.mutable_state(handle);
    gameplay::Mob*             mob    = mob_of(world, handle);
    if (state == nullptr || state->removed || mob == nullptr || !host.with_hand) {
        return;
    }
    const std::string_view    type = type_name(state->type);
    const gameplay::TameKind* kind = gameplay::tame_kind(type);
    if (kind == nullptr || !kind->own_goals) {
        return;
    }
    const auto who = std::ranges::find(players_, click.player, &TamingPlayer::network_id);
    if (who == players_.end()) {
        return;
    }
    gameplay::MobBrain&    brain  = mob->mutable_brain();
    gameplay::TameState&   tame   = brain.tame;
    gameplay::AnimalState& animal = brain.animal;

    (void)host.with_hand(click.player, click.hand, [&](HusbandryHand& hand) {
        if (box_distance_sq(*state, hand.eyes) >= kReach * kReach) {
            return;
        }
        net::ItemStack&        held = hand.inventory[hand.slot];
        const std::string_view item =
            held.item_id == 0 || held.count <= 0 ? std::string_view{} : item_name(held.item_id);
        const auto consume = [&] {
            if (hand.creative) {
                return;
            }
            held.count = static_cast<i8>(held.count - 1);
            if (held.count <= 0) {
                held = net::ItemStack{};
            }
            hand.send_slot(hand.slot);
        };
        const auto event = [&](i8 status) {
            deliver(net::clientbound::kEntityEvent, net::encode_entity_event(state->network_id, status));
        };
        const auto love_or_grow = [&]() -> bool {
            if (animal.baby()) {
                gameplay::age_up(animal, gameplay::feeding_growth(animal.age));
                return true;
            }
            if (animal.age == 0 && !animal.in_love()) {
                animal.love       = gameplay::kLoveTicks;
                animal.love_cause = click.player;
                event(gameplay::kLoveEventStatus);
                return true;
            }
            return false;
        };

        // ── Wolf, cat, parrot ──
        if (pet(tame.family)) {
            if (!tame.tame) {
                if (tame.family == F::Wolf && tame.anger > 0) {
                    return;  // an angry wolf takes nothing
                }
                if (!gameplay::is_taming_food(*kind, item)) {
                    return;  // measured: meat on a wild wolf is kept
                }
                consume();
                if (gameplay::taming_roll(random_, kind->taming_odds)) {
                    tame.tame    = true;
                    tame.owner   = who->uuid;
                    tame.sitting = true;  // measured: `Sitting: 1b` after the hearts
                    tame.anger   = 0;
                    tame.angry_at.reset();
                    brain.target        = entity::kNoEntity;
                    brain.target_player = 0;
                    brain.follower.clear();
                    gameplay::apply_tame_body(tame, *state);
                    state->health = state->max_health;  // measured: 6 → 20 on a wolf
                    event(gameplay::kTamedEventStatus);
                    ++stats.tamed;
                } else {
                    event(gameplay::kRefusedEventStatus);
                    ++stats.refused;
                }
                tame.dirty = true;
                return;
            }
            if (!tame.owned_by(who->uuid)) {
                return;
            }
            if (tame.family != F::Parrot && gameplay::is_tame_food(*kind, item)) {
                // A hurt pet heals by the food's hunger points (the wiki's
                // rule; measured only as "consumed", every test wolf was full
                // — see apprivoisement.md); a full adult falls in love
                // (measured: `InLove` 591 read nine ticks later, status 18);
                // a pup grows.
                if (state->health < state->max_health) {
                    const i32 heal = tame.family == F::Wolf
                                         ? gameplay::wolf_food_heal(item).value_or(1)
                                         : 2;  // cod and salmon: 2 hunger points
                    state->health = std::min(state->max_health, state->health + static_cast<f32>(heal));
                    consume();
                    tame.dirty = true;
                    return;
                }
                if (love_or_grow()) {
                    consume();
                    tame.dirty = true;
                }
                return;
            }
            if (kind->collar) {
                if (const auto colour = gameplay::dye_colour(item)) {
                    // Measured: a dye of the collar's own colour is kept.
                    if (*colour != tame.collar) {
                        tame.collar = *colour;
                        consume();
                        tame.dirty = true;
                    }
                    return;
                }
            }
            // Anything else, an empty hand included (measured: a stick, a
            // bone, a cod, a cookie, bread on a wolf): stand up or sit down.
            tame.sitting = !tame.sitting;
            brain.follower.clear();
            brain.wants_move    = false;
            brain.target        = entity::kNoEntity;
            brain.target_player = 0;
            tame.dirty          = true;
            return;
        }

        // ── Ocelot ──
        if (tame.family == F::Ocelot) {
            if (!gameplay::is_taming_food(*kind, item)) {
                return;
            }
            if (!tame.trusting) {
                const f64 dx = who->feet.x - state->position.x;
                const f64 dz = who->feet.z - state->position.z;
                if (dx * dx + dz * dz >= kOcelotReach * kOcelotReach) {
                    return;
                }
                consume();
                if (gameplay::taming_roll(random_, kind->taming_odds)) {
                    tame.trusting = true;
                    event(gameplay::kTrustedEventStatus);
                    ++stats.tamed;
                } else {
                    event(gameplay::kDistrustEventStatus);
                    ++stats.refused;
                }
                tame.dirty = true;
                return;
            }
            if (love_or_grow()) {
                consume();
            }
            return;
        }

        // ── Horse, donkey, mule, llama ──
        if (!equine(tame.family)) {
            return;
        }
        const auto food = gameplay::horse_food(type, item);
        if (animal.baby()) {
            if (food) {
                gameplay::age_up(animal, food->growth);
                state->health = std::min(state->max_health, state->health + food->heal);
                consume();
            }
            return;
        }
        if (click.sneaking && tame.tame) {
            // The inventory screen (saddle, armour, chest): not done, named.
            return;
        }
        if (food) {
            bool used = false;
            if (state->health < state->max_health && food->heal > 0.0F) {
                state->health = std::min(state->max_health, state->health + food->heal);
                used          = true;
            }
            if (!tame.tame && food->temper > 0 && tame.temper < kind->max_temper) {
                tame.temper = std::min(tame.temper + food->temper, kind->max_temper);
                used        = true;
            }
            if (tame.tame && food->love && type != "minecraft:mule" && love_or_grow()) {
                used = true;
            }
            if (used) {
                consume();
                tame.dirty = true;
            }
            return;
        }
        if (item == "minecraft:saddle") {
            if (tame.tame && !tame.saddled && tame.family == F::Horse) {
                tame.saddled = true;
                consume();
                tame.dirty = true;
            }
            return;
        }
        if (item.ends_with("_horse_armor")) {
            if (tame.tame && type == "minecraft:horse" && tame.armour.empty()) {
                tame.armour = std::string{item};
                consume();
                send_equipment(*state, tame, deliver);
            }
            return;
        }
        if (item == "minecraft:chest") {
            if (tame.tame && !tame.chested && chest_bearer(type)) {
                tame.chested = true;
                consume();
                tame.dirty = true;
            }
            return;
        }
        if (tame.family == F::Llama && carpet_colour(item)) {
            if (tame.tame && tame.armour != item) {
                tame.armour = std::string{item};
                consume();
                tame.dirty = true;
            }
            return;
        }
        if (!item.empty()) {
            return;  // measured mounts were all with an empty hand
        }
        if (tame.rider == 0 && !riding_.contains(click.player)) {
            mount(world, handle, click.player, deliver);
            ++stats.mounted;
        }
    });
}

// ── Hits ────────────────────────────────────────────────────────────────────

void Taming::on_player_hit(entity::EntityWorld& world, i32 player, i32 target, i64 tick) {
    const entity::EntityHandle handle = world.find(target);
    Track&                     track  = tracks_[player];
    track.attacked                    = handle;
    track.attacked_tick               = tick;

    gameplay::Mob*             mob   = mob_of(world, handle);
    const entity::EntityState* state = world.state(handle);
    if (mob == nullptr || state == nullptr) {
        return;
    }
    gameplay::TameState& tame = mob->mutable_brain().tame;
    if (!tame.active()) {
        return;
    }
    if (tame.sitting) {
        tame.sitting = false;  // a hurt pet stands up
        tame.dirty   = true;
    }
    if (tame.family != gameplay::TameFamily::Wolf) {
        return;
    }
    const auto who = std::ranges::find(players_, player, &TamingPlayer::network_id);
    if (who == players_.end() || tame.owned_by(who->uuid)) {
        return;  // a wolf does not turn on its owner
    }
    const auto enrage = [&](gameplay::TameState& wolf) {
        wolf.anger    = gameplay::draw_anger_time(random_);
        wolf.angry_at = who->uuid;
        wolf.sitting  = false;
        wolf.dirty    = true;
    };
    if (tame.tame) {
        enrage(tame);
        return;
    }
    // A wild wolf calls its pack: measured, the four wolves standing within
    // six blocks of the one hit were all angry a second later. The reach used
    // is the wolf's follow range, 16 — beyond six it is not measured.
    const Vec3d centre = state->position;
    for (const entity::EntityHandle other : world.handles()) {
        const entity::EntityState* peer = world.state(other);
        if (peer == nullptr || peer->removed || peer->type != state->type) {
            continue;
        }
        if (std::abs(peer->position.x - centre.x) > 16.0 ||
            std::abs(peer->position.y - centre.y) > 16.0 ||
            std::abs(peer->position.z - centre.z) > 16.0) {
            continue;
        }
        if (gameplay::Mob* wolf = mob_of(world, other);
            wolf != nullptr && !wolf->mutable_brain().tame.tame) {
            enrage(wolf->mutable_brain().tame);
        }
    }
}

// ── The tick ────────────────────────────────────────────────────────────────

TamingStats Taming::before_entity_tick(entity::EntityWorld& world, gameplay::MobContext& context,
                                       const TamingHost& host, const TamingDeliver& deliver,
                                       i64 tick) {
    now_ = tick;
    TamingStats stats;
    {
        const std::scoped_lock lock{mutex_};
        working_.swap(pending_);
        off_now_.swap(getting_off_);
    }
    players_.clear();
    if (host.players) {
        host.players(players_);
    }
    owners_.clear();
    for (const TamingPlayer& player : players_) {
        gameplay::Owner owner;
        owner.network_id = player.network_id;
        owner.uuid       = player.uuid;
        owner.feet       = player.feet;
        owner.on_ground  = player.on_ground;
        if (const auto it = tracks_.find(player.network_id); it != tracks_.end()) {
            owner.hurt_by       = it->second.hurt_by;
            owner.hurt_by_tick  = it->second.hurt_by_tick;
            owner.attacked      = it->second.attacked;
            owner.attacked_tick = it->second.attacked_tick;
        }
        owners_.push_back(owner);
    }
    events_.clear();
    world_.owners      = owners_;
    world_.events      = &events_;
    context.tame_world = &world_;

    for (const Click& click : working_) {
        interact(world, click, host, deliver, stats);
    }
    working_.clear();
    for (const i32 player : off_now_) {
        dismount(world, player, "got off", host, deliver);
    }
    off_now_.clear();
    // A rider who left.
    for (auto it = riding_.begin(); it != riding_.end();) {
        const i32 player = it->first;
        ++it;
        if (std::ranges::find(players_, player, &TamingPlayer::network_id) == players_.end()) {
            dismount(world, player, "gone", host, deliver);
        }
    }
    return stats;
}

TamingStats Taming::after_entity_tick(entity::EntityWorld& world,
                                      std::span<const gameplay::MobAttack> attacks,
                                      const TamingHost& host, const TamingDeliver& deliver,
                                      i64 tick) {
    TamingStats stats;
    for (const gameplay::TameEvent& event : events_) {
        const entity::EntityState* state = world.state(event.self);
        if (state == nullptr) {
            continue;
        }
        switch (event.kind) {
        case gameplay::TameEventKind::Tamed:
            deliver(net::clientbound::kEntityEvent,
                    net::encode_entity_event(state->network_id, gameplay::kTamedEventStatus));
            ++stats.tamed;
            OV_LOG_DEBUG("tame: {} tamed by {}", type_name(state->type), event.player);
            break;
        case gameplay::TameEventKind::Threw:
            OV_LOG_DEBUG("tame: tick {}: {} threw {}", tick, type_name(state->type), event.player);
            dismount(world, event.player, "thrown", host, deliver);
            deliver(net::clientbound::kEntityEvent,
                    net::encode_entity_event(state->network_id, gameplay::kRefusedEventStatus));
            ++stats.thrown;
            break;
        case gameplay::TameEventKind::Teleported:
            ++stats.teleported;
            break;
        }
    }
    events_.clear();

    // What hurt a player this tick: a wolf defends its owner from it.
    for (const gameplay::MobAttack& attack : attacks) {
        if (attack.target_is_player) {
            Track& track       = tracks_[attack.target];
            track.hurt_by      = attack.attacker;
            track.hurt_by_tick = tick;
        }
    }

    // The riders: a steered horse goes where its rider's client put it, and
    // every rider is carried along.
    std::unordered_map<i32, Move> moves;
    {
        const std::scoped_lock lock{mutex_};
        moves.swap(moves_);
    }
    for (auto it = riding_.begin(); it != riding_.end();) {
        const i32                  player = it->first;
        const entity::EntityHandle handle = world.find(it->second);
        entity::EntityState*       state  = world.mutable_state(handle);
        gameplay::Mob*             mob    = mob_of(world, handle);
        ++it;
        if (state == nullptr || state->removed || mob == nullptr) {
            riding_.erase(player);
            continue;
        }
        const gameplay::TameState& tame = mob->brain().tame;
        if (!tame.tame || tick % 40 == 0) {
            // Every tick of an untamed ride: its ticks, the tantrum's draws and
            // the last value drawn (0 of kTantrumOdds decides), and the goals
            // running. A tick without a new draw is a tantrum that did not
            // tick; draws that never hit 0 are odds that are off. Seen on the
            // e2e: a ridden wild horse going 506 ticks without a decision.
            mob->goals().running(running_);
            running_names_.clear();  // keeps its capacity: no allocation once grown
            for (const std::string_view name : running_) {
                running_names_ += name;
                running_names_ += ' ';
            }
            OV_LOG_DEBUG("tame: tick {}: {} ridden {} ticks, draw {} = {} (0 of {} decides), "
                         "temper {}, running [{}]",
                         tick, type_name(state->type), tame.ridden_for, tame.tantrum_draws,
                         tame.tantrum_last, gameplay::kTantrumOdds, tame.temper, running_names_);
        }
        if (gameplay::rider_controls(tame)) {
            if (const auto move = moves.find(player); move != moves.end()) {
                // The rider's client simulates its mount and says where it is
                // (Move Vehicle): the server takes it, as vanilla does within
                // its movement checks — which are not implemented, named.
                state->position = move->second.at;
                state->velocity = Vec3d{};
                state->yaw      = move->second.yaw;
                state->head_yaw = move->second.yaw;
                ++stats.steered;
            }
        }
        const Vec3d seat = state->position + Vec3d{0.0, static_cast<f64>(state->height) * 0.75, 0.0};
        if (host.carry_rider && !host.carry_rider(player, seat)) {
            dismount(world, player, "carry", host, deliver);
        }
    }

    // What changed on a tame animal, told; an angry wolf's anger counts down on
    // the wire (measured: 400, 399, 398… one update a tick).
    for (const entity::EntityHandle handle : world.handles()) {
        gameplay::Mob* mob = mob_of(world, handle);
        if (mob == nullptr) {
            continue;
        }
        gameplay::TameState& tame = mob->mutable_brain().tame;
        if (!tame.active()) {
            continue;
        }
        const entity::EntityState* state = world.state(handle);
        if (state == nullptr) {
            continue;
        }
        if (tame.dirty) {
            tame.dirty = false;
            send_metadata(*state, mob->brain(), deliver);
        } else if (tame.family == gameplay::TameFamily::Wolf && tame.anger > 0) {
            net::MetadataWriter fields;
            fields.varint_value(net::metadata::kWolfAnger, tame.anger);
            deliver(net::clientbound::kEntityMetadata,
                    net::encode_entity_metadata(state->network_id, fields.take()));
        }
    }
    return stats;
}

// ── Births ──────────────────────────────────────────────────────────────────

void Taming::on_birth(entity::EntityWorld& world, entity::EntityHandle mother,
                      entity::EntityHandle father, entity::EntityHandle child) {
    const gameplay::MobBrain* a     = gameplay::mob_brain_of(world, mother);
    const gameplay::MobBrain* b     = gameplay::mob_brain_of(world, father);
    gameplay::Mob*            young = mob_of(world, child);
    entity::EntityState*      state = world.mutable_state(child);
    if (a == nullptr || young == nullptr || state == nullptr || !a->tame.active()) {
        return;
    }
    const gameplay::MobBrain& other = b != nullptr ? *b : *a;
    gameplay::TameState&      tame  = young->mutable_brain().tame;
    const std::string_view    type  = type_name(state->type);
    if (pet(a->tame.family) && a->tame.tame) {
        // A pup is its parents' owner's (the owner of the parent that
        // concluded; which parent is not measured).
        tame.tame  = true;
        tame.owner = a->tame.owner ? a->tame.owner : other.tame.owner;
    } else if (equine(a->tame.family)) {
        tame.stats = gameplay::offspring_stats(type, a->tame.stats, other.tame.stats, random_);
        tame.bred  = false;
        if (type == "minecraft:horse") {
            // Coat from a parent or new, markings likewise: the wiki's
            // odds, not measured (named).
            const i32 roll_colour = random_.next_int(9);
            const i32 colour = roll_colour < 4 ? (a->tame.variant & 0xFF)
                               : roll_colour < 8 ? (other.tame.variant & 0xFF)
                                                 : random_.next_int(7);
            const i32 roll_marks = random_.next_int(5);
            const i32 marks      = roll_marks < 2 ? (a->tame.variant >> 8)
                                   : roll_marks < 4 ? (other.tame.variant >> 8)
                                                    : random_.next_int(5);
            tame.variant = colour | (marks << 8);
        } else if (a->tame.family == gameplay::TameFamily::Llama) {
            tame.variant  = random_.next_boolean() ? a->tame.variant : other.tame.variant;
            tame.strength = 1 + random_.next_int(std::max({a->tame.strength, other.tame.strength, 1}));
        }
    }
    state->max_health = 0.0F;  // force the body to be rebuilt from the stats
    gameplay::apply_tame_body(tame, *state);
    state->health = state->max_health;
    tame.dirty    = true;
}

// ── entities/ ───────────────────────────────────────────────────────────────

void Taming::write_nbt(entity::EntityWorld& world, const entity::EntityState& state,
                       nbt::Tag& out) const {
    const gameplay::MobBrain* brain = gameplay::mob_brain_of(world, world.find(state.network_id));
    if (brain == nullptr || !brain->tame.active()) {
        return;
    }
    using F                       = gameplay::TameFamily;
    const gameplay::TameState& t  = brain->tame;
    const std::string_view     ty = type_name(state.type);
    // The ageable fields for the types the storage does not count as
    // creatures (mule, trader llama, camel, bee): the keys vanilla writes.
    (void)out.put("Age", nbt::Tag{brain->animal.age});
    default_to(out, "ForcedAge", nbt::Tag{i32{0}});
    (void)out.put("InLove", nbt::Tag{brain->animal.love});
    switch (t.family) {
    case F::Wolf:
        put_owner(out, t);
        (void)out.put("Sitting", nbt::Tag::make_bool(t.sitting));
        (void)out.put("CollarColor", nbt::Tag{t.collar});
        (void)out.put("AngerTime", nbt::Tag{t.anger});
        if (t.angry_at) {
            (void)out.put("AngryAt", uuid_tag(*t.angry_at));
        } else {
            (void)out.erase("AngryAt");
        }
        break;
    case F::Cat:
        put_owner(out, t);
        (void)out.put("Sitting", nbt::Tag::make_bool(t.sitting));
        (void)out.put("CollarColor", nbt::Tag{t.collar});
        (void)out.put("variant",
                      nbt::Tag{"minecraft:" + std::string{kCatVariants[static_cast<usize>(
                                                  std::clamp(t.variant, 0, 10))]}});
        break;
    case F::Parrot:
        put_owner(out, t);
        (void)out.put("Sitting", nbt::Tag::make_bool(t.sitting));
        (void)out.put("Variant", nbt::Tag{t.variant});
        break;
    case F::Ocelot:
        (void)out.put("Trusting", nbt::Tag::make_bool(t.trusting));
        break;
    case F::Horse:
    case F::Llama: {
        (void)out.put("Tame", nbt::Tag::make_bool(t.tame));
        (void)out.put("Temper", nbt::Tag{t.temper});
        put_owner(out, t);
        (void)out.put("Bred", nbt::Tag::make_bool(t.bred));
        default_to(out, "EatingHaystack", nbt::Tag::make_bool(false));
        if (t.family == F::Horse) {
            if (t.saddled) {
                (void)out.put("SaddleItem", item_compound("minecraft:saddle"));
            } else {
                (void)out.erase("SaddleItem");
            }
        }
        if (ty == "minecraft:horse") {
            (void)out.put("Variant", nbt::Tag{t.variant});
            if (!t.armour.empty()) {
                (void)out.put("ArmorItem", item_compound(t.armour));
            } else {
                (void)out.erase("ArmorItem");
            }
        }
        if (chest_bearer(ty)) {
            (void)out.put("ChestedHorse", nbt::Tag::make_bool(t.chested));
        }
        if (t.family == F::Llama) {
            (void)out.put("Strength", nbt::Tag{t.strength});
            (void)out.put("Variant", nbt::Tag{t.variant});
            if (!t.armour.empty()) {
                (void)out.put("DecorItem", item_compound(t.armour));
            } else {
                (void)out.erase("DecorItem");
            }
        }
        if (t.stats.drawn()) {
            if (out.find("Attributes") == nullptr ||
                out.find("Attributes")->list() == nullptr) {
                (void)out.put("Attributes", nbt::Tag::make_list(nbt::TagType::Compound));
            }
            nbt::Tag& list = *out.find("Attributes");
            put_attribute(list, kMaxHealth, t.stats.max_health);
            put_attribute(list, kSpeed, t.stats.speed);
            put_attribute(list, kJump, t.stats.jump);
        }
        break;
    }
    case F::Rabbit:
        (void)out.put("RabbitType", nbt::Tag{t.variant});
        default_to(out, "MoreCarrotTicks", nbt::Tag{i32{0}});
        break;
    case F::Fox:
        (void)out.put("Type", nbt::Tag{std::string{t.variant == 1 ? "snow" : "red"}});
        (void)out.put("Sleeping", nbt::Tag::make_bool(t.flag));
        default_to(out, "Sitting", nbt::Tag::make_bool(false));
        default_to(out, "Crouching", nbt::Tag::make_bool(false));
        break;
    case F::Turtle:
        (void)out.put("HasEgg", nbt::Tag::make_bool(t.flag));
        break;
    case F::Bee:
        (void)out.put("HasNectar", nbt::Tag::make_bool(t.flag));
        break;
    case F::Goat:
        (void)out.put("IsScreamingGoat", nbt::Tag::make_bool(t.flag));
        (void)out.put("HasLeftHorn", nbt::Tag::make_bool(t.left_horn));
        (void)out.put("HasRightHorn", nbt::Tag::make_bool(t.right_horn));
        break;
    case F::Camel:
        if (brain->animal.saddled) {
            (void)out.put("SaddleItem", item_compound("minecraft:saddle"));
        } else {
            (void)out.erase("SaddleItem");
        }
        break;
    case F::None:
        break;
    }
}

void Taming::read_nbt(entity::EntityWorld& world, entity::EntityState& state,
                      const nbt::Tag& compound) const {
    gameplay::Mob* mob = mob_of(world, world.find(state.network_id));
    if (mob == nullptr || !mob->brain().tame.active()) {
        return;
    }
    using F                        = gameplay::TameFamily;
    gameplay::MobBrain&  brain     = mob->mutable_brain();
    gameplay::TameState& t         = brain.tame;
    const std::string_view ty      = type_name(state.type);
    const auto           read_owner = [&] {
        t.owner = uuid_from(compound.find("Owner"));
    };
    switch (t.family) {
    case F::Wolf:
    case F::Cat:
    case F::Parrot:
        read_owner();
        t.tame    = t.owner.has_value();
        t.sitting = get_bool(compound, "Sitting", false);
        t.collar  = static_cast<i8>(get_i64(compound, "CollarColor", 14));
        if (t.family == F::Wolf) {
            t.anger    = static_cast<i32>(get_i64(compound, "AngerTime", 0));
            t.angry_at = uuid_from(compound.find("AngryAt"));
        } else if (t.family == F::Parrot) {
            t.variant = static_cast<i32>(get_i64(compound, "Variant", 0));
        } else if (const nbt::Tag* variant = compound.find("variant")) {
            std::string_view name = variant->as_string();
            if (name.starts_with("minecraft:")) {
                name.remove_prefix(10);
            }
            for (usize i = 0; i < kCatVariants.size(); ++i) {
                if (kCatVariants[i] == name) {
                    t.variant = static_cast<i32>(i);
                }
            }
        }
        break;
    case F::Ocelot:
        t.trusting = get_bool(compound, "Trusting", false);
        break;
    case F::Horse:
    case F::Llama:
        t.tame    = get_bool(compound, "Tame", false);
        t.temper  = static_cast<i32>(get_i64(compound, "Temper", 0));
        read_owner();
        t.bred    = get_bool(compound, "Bred", false);
        t.saddled = item_id_of(compound.find("SaddleItem")) == "minecraft:saddle";
        t.chested = get_bool(compound, "ChestedHorse", false);
        if (ty == "minecraft:horse") {
            t.variant = static_cast<i32>(get_i64(compound, "Variant", 0));
            t.armour  = std::string{item_id_of(compound.find("ArmorItem"))};
        }
        if (t.family == F::Llama) {
            t.strength = static_cast<i32>(get_i64(compound, "Strength", t.strength));
            t.variant  = static_cast<i32>(get_i64(compound, "Variant", 0));
            t.armour   = std::string{item_id_of(compound.find("DecorItem"))};
        }
        if (const auto health = read_attribute(compound, kMaxHealth)) {
            t.stats.max_health = *health;
        }
        if (const auto speed = read_attribute(compound, kSpeed)) {
            t.stats.speed = *speed;
        }
        if (const auto jump = read_attribute(compound, kJump)) {
            t.stats.jump = *jump;
        }
        break;
    case F::Rabbit:
        t.variant = static_cast<i32>(get_i64(compound, "RabbitType", 0));
        break;
    case F::Fox:
        if (const nbt::Tag* type = compound.find("Type")) {
            t.variant = type->as_string() == "snow" ? 1 : 0;
        }
        t.flag = get_bool(compound, "Sleeping", false);
        break;
    case F::Turtle:
        t.flag = get_bool(compound, "HasEgg", false);
        break;
    case F::Bee:
        t.flag = get_bool(compound, "HasNectar", false);
        break;
    case F::Goat:
        t.flag       = get_bool(compound, "IsScreamingGoat", false);
        t.left_horn  = get_bool(compound, "HasLeftHorn", true);
        t.right_horn = get_bool(compound, "HasRightHorn", true);
        break;
    case F::Camel:
        brain.animal.saddled = item_id_of(compound.find("SaddleItem")) == "minecraft:saddle";
        break;
    case F::None:
        break;
    }
    // The body again, now the stats and the tame flag are known; the stored
    // health, which the storage clamped to the type's registry maximum (8 for
    // a wolf), is read once more against the real one.
    const f32 registry_max = state.max_health;
    state.max_health       = 0.0F;
    gameplay::apply_tame_body(t, state);
    if (state.max_health <= 0.0F) {
        state.max_health = registry_max;  // no drawn body: the type's own stands
    }
    if (const nbt::Tag* health = compound.find("Health"); health != nullptr && state.max_health > 0.0F) {
        state.health = std::clamp(static_cast<f32>(health->as_f64()), 0.0F, state.max_health);
    }
}

}  // namespace ov::server
