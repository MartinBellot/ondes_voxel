// ── mobs-4 ── See mob_effects.hpp.
#include "mob_effects.hpp"

#include "ov/gameplay/mob_body.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/protocol/effect_packets.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/play.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>
#include <vector>

namespace ov::server {
namespace {

// The metadata the effects own on a living entity, measured on a cow in the
// effects wave (scripts/measure_effects.py `metadata`, effets.md § 10).
constexpr u8 kEffectColour  = 10;
constexpr u8 kEffectAmbient = 11;
constexpr u8 kInvisibleBit  = 0x20;
constexpr u8 kGlowingBit    = 0x40;

[[nodiscard]] u8 effect_bits(const gameplay::ActiveEffects& effects) noexcept {
    return static_cast<u8>((effects.has(gameplay::Effect::Invisibility) ? kInvisibleBit : 0) |
                           (effects.has(gameplay::Effect::Glowing) ? kGlowingBit : 0));
}

/// Update Attributes for `movement_speed` with its modifiers — the one
/// attribute a real server was seen telling a cow's watchers about
/// (`measure_hostile.py packets`: on Speed, on its clearing, and to a client
/// that joined later). Empty when the mob does not own it.
[[nodiscard]] std::vector<u8> speed_packet(i32 entity_id, const gameplay::AttributeMap& attributes) {
    const gameplay::AttributeInstance* speed = attributes.get(gameplay::Attribute::MovementSpeed);
    if (speed == nullptr) {
        return {};
    }
    std::array<net::WireModifier, gameplay::AttributeInstance::kCapacity> modifiers{};
    usize                                                                n = 0;
    for (const gameplay::AttributeModifier& modifier : speed->modifiers()) {
        modifiers[n++] =
            net::WireModifier{modifier.uuid, modifier.amount, static_cast<u8>(modifier.operation)};
    }
    const net::AttributeProperty property{
        gameplay::attribute_info(gameplay::Attribute::MovementSpeed).name, speed->base(),
        std::span<const net::WireModifier>{modifiers.data(), n}};
    return net::encode_update_attributes_full(entity_id,
                                              std::span<const net::AttributeProperty>{&property, 1});
}

}  // namespace

/// The mob, as the effect rules see one.
class MobEffects::Target final : public gameplay::EffectTarget {
public:
    Target(entity::EntityState& state, Row& row, MobCombat* combat,
           std::vector<MobEffectHurt>& hurts) noexcept
        : state_{state}, row_{row}, combat_{combat}, hurts_{hurts} {}

    [[nodiscard]] Body body() const noexcept override { return row_.body; }
    [[nodiscard]] f32  health() const noexcept override { return state_.health; }
    [[nodiscard]] f32  max_health() const noexcept override {
        return static_cast<f32>(
            row_.attributes.value(gameplay::Attribute::MaxHealth)
                .value_or(static_cast<f64>(state_.max_health)));
    }

    void heal(f32 amount) override {
        if (alive()) {
            state_.health = std::min(max_health(), state_.health + amount);
        }
    }

    void hurt(gameplay::DamageKind kind, f32 amount,
              const gameplay::DamageConstants& constants) override {
        if (!alive() || combat_ == nullptr) {
            return;
        }
        const MobHurt result =
            combat_->hurt(state_, kind, row_.effects.after_resistance(kind, amount), constants);
        if (result.applied) {
            row_.died = result.killed;
            hurts_.push_back(MobEffectHurt{state_.network_id, kind, result.killed});
        }
    }

    [[nodiscard]] f32 absorption() const noexcept override {
        return combat_ != nullptr ? combat_->absorption(state_.network_id) : 0.0F;
    }
    void set_absorption(f32 amount) override {
        if (combat_ != nullptr) {
            combat_->set_absorption(state_, amount);
        }
    }

    [[nodiscard]] gameplay::AttributeMap* attributes() noexcept override { return &row_.attributes; }

    void clamp_health() override {
        state_.max_health = max_health();
        state_.health     = std::min(state_.health, state_.max_health);
    }

private:
    [[nodiscard]] bool alive() const noexcept {
        return !state_.removed && !row_.died && state_.health > 0.0F;
    }

    entity::EntityState&        state_;
    Row&                        row_;
    MobCombat*                  combat_;
    std::vector<MobEffectHurt>& hurts_;
};

MobEffects::MobEffects(const registry::Registries& registries, MobCombat* combat)
    : registries_{&registries},
      combat_{combat},
      entity_registry_{registries.find("minecraft:entity_type")} {
    if (const auto attributes = registries.find("minecraft:attribute")) {
        movement_speed_id_ =
            registries.protocol_id(*attributes, "minecraft:generic.movement_speed").value_or(-1);
        attack_damage_id_ =
            registries.protocol_id(*attributes, "minecraft:generic.attack_damage").value_or(-1);
    }
    living_ = [](entity::EntityWorld& world, entity::EntityHandle handle) {
        return dynamic_cast<gameplay::Mob*>(world.logic(handle)) != nullptr;
    };
    // Room for a splash over a crowd without the tick allocating.
    hurts_.reserve(64);
}

MobEffects::~MobEffects() = default;

void MobEffects::set_living(std::function<bool(entity::EntityWorld&, entity::EntityHandle)> living) {
    living_ = std::move(living);
}

void MobEffects::set_other_flags(std::function<u8(const entity::EntityState&)> flags) {
    other_flags_ = std::move(flags);
}

std::optional<MobEffects::Located> MobEffects::locate(entity::EntityWorld& world, i32 id) const {
    const entity::EntityHandle handle = world.find(id);
    if (handle == entity::kNoEntity) {
        return std::nullopt;
    }
    entity::EntityState* state = world.mutable_state(handle);
    if (state == nullptr || state->removed || state->health <= 0.0F ||
        (living_ && !living_(world, handle))) {
        return std::nullopt;
    }
    return Located{handle, state};
}

MobEffects::Row& MobEffects::row_of(entity::EntityWorld& world, entity::EntityHandle handle,
                                    entity::EntityState& state) {
    auto [it, inserted] = rows_.try_emplace(state.network_id);
    Row& row            = it->second;
    if (inserted) {
        const std::string_view type =
            entity_registry_ ? registries_->entry_of(*entity_registry_, state.type) : std::string_view{};
        row.body = gameplay::effect_body(type);
        // The entity's own maximum, not the type's: a slime's depends on its
        // size, and no effect is on the mob yet to have moved it.
        row.attributes.own(gameplay::Attribute::MaxHealth, static_cast<f64>(state.max_health));
        if (movement_speed_id_ >= 0) {
            if (const auto speed = world.attribute(handle, movement_speed_id_)) {
                row.attributes.own(gameplay::Attribute::MovementSpeed, *speed);
                row.base_speed = *speed;
            }
        }
        // Owned only by the types that have it: a cow has no attack damage,
        // and Strength on a cow modifies nothing.
        if (attack_damage_id_ >= 0) {
            if (const auto damage = world.attribute(handle, attack_damage_id_)) {
                row.attributes.own(gameplay::Attribute::AttackDamage, *damage);
            }
        }
        // The watchers already hold the bases from the spawn: owning them here
        // changes nothing they see. Measured: a poison alone sent no attributes.
        for (usize a = 0; a < gameplay::kAttributeCount; ++a) {
            if (gameplay::AttributeInstance* owned =
                    row.attributes.get(static_cast<gameplay::Attribute>(a))) {
                owned->clear_dirty();
            }
        }
        row.sent_health = state.health;
    }
    return row;
}

void MobEffects::settle(entity::EntityWorld& world, entity::EntityHandle handle,
                        entity::EntityState& state, Row& row) const {
    state.max_health = static_cast<f32>(
        row.attributes.value(gameplay::Attribute::MaxHealth)
            .value_or(static_cast<f64>(state.max_health)));
    if (auto* mob = dynamic_cast<gameplay::Mob*>(world.logic(handle)); mob != nullptr) {
        const f64 speed = row.attributes.value(gameplay::Attribute::MovementSpeed).value_or(row.base_speed);
        mob->mutable_brain().effect_walk = gameplay::walk_factor(speed, row.base_speed);
    }
}

void MobEffects::flush(const entity::EntityState& state, Row& row,
                       const MobEffectSink& broadcast) const {
    // No Entity Effect for a mob: see the header.
    row.effects.clear_events();

    const u32  colour  = row.effects.particle_color();
    const bool ambient = row.effects.all_ambient();
    const u8   bits    = effect_bits(row.effects);

    net::MetadataWriter fields;
    if (bits != row.sent_flags) {
        const u8 other = other_flags_ ? other_flags_(state) : u8{0};
        fields.byte_value(net::metadata::kSharedFlags, static_cast<i8>(static_cast<u8>(other | bits)));
    }
    if (colour != row.sent_colour) {
        fields.varint_value(kEffectColour, static_cast<i32>(colour));
    }
    if (ambient != row.sent_ambient) {
        fields.boolean_value(kEffectAmbient, ambient);
    }
    if (state.health != row.sent_health) {
        fields.float_value(net::metadata::kHealth, state.health);
    }
    row.sent_flags   = bits;
    row.sent_colour  = colour;
    row.sent_ambient = ambient;
    row.sent_health  = state.health;
    if (!fields.empty() && broadcast) {
        broadcast(net::clientbound::kEntityMetadata,
                  net::encode_entity_metadata(state.network_id, fields.take()));
    }

    // Update Attributes: `movement_speed` when Speed or Slowness moved it.
    // The others an effect touches (`max_health`, `attack_damage`) were not
    // seen on the wire for a mob and are not sent (mobs-4.md § 2).
    bool speed_moved = false;
    for (usize a = 0; a < gameplay::kAttributeCount; ++a) {
        gameplay::AttributeInstance* owned = row.attributes.get(static_cast<gameplay::Attribute>(a));
        if (owned == nullptr || !owned->dirty()) {
            continue;
        }
        speed_moved = speed_moved || owned->attribute() == gameplay::Attribute::MovementSpeed;
        owned->clear_dirty();
    }
    if (speed_moved && broadcast) {
        const std::vector<u8> payload = speed_packet(state.network_id, row.attributes);
        if (!payload.empty()) {
            broadcast(net::clientbound::kUpdateAttributes, payload);
        }
    }
}

void MobEffects::finish(entity::EntityWorld& world, const Located& at,
                        const MobEffectSink& broadcast) {
    const auto it = rows_.find(at.state->network_id);
    if (it == rows_.end()) {
        return;
    }
    settle(world, at.handle, *at.state, it->second);
    flush(*at.state, it->second, broadcast);
    if (it->second.effects.empty() && !it->second.died) {
        rows_.erase(it);
    }
}

std::optional<gameplay::AddResult> MobEffects::apply(entity::EntityWorld& world, i32 id,
                                                     const gameplay::EffectInstance& instance,
                                                     const MobEffectSink& broadcast) {
    const auto at = locate(world, id);
    if (!at) {
        return std::nullopt;
    }
    Row&   row = row_of(world, at->handle, *at->state);
    Target target{*at->state, row, combat_, hurts_};
    const gameplay::AddResult result = row.effects.add(instance, target);
    finish(world, *at, broadcast);
    return result;
}

std::optional<bool> MobEffects::remove(entity::EntityWorld& world, i32 id, gameplay::Effect effect,
                                       const MobEffectSink& broadcast) {
    const auto at = locate(world, id);
    if (!at) {
        return std::nullopt;
    }
    const auto it = rows_.find(id);
    if (it == rows_.end()) {
        return false;
    }
    Target     target{*at->state, it->second, combat_, hurts_};
    const bool removed = it->second.effects.remove(effect, target);
    finish(world, *at, broadcast);
    return removed;
}

std::optional<usize> MobEffects::clear(entity::EntityWorld& world, i32 id,
                                       const MobEffectSink& broadcast) {
    const auto at = locate(world, id);
    if (!at) {
        return std::nullopt;
    }
    const auto it = rows_.find(id);
    if (it == rows_.end()) {
        return usize{0};
    }
    Target      target{*at->state, it->second, combat_, hurts_};
    const usize removed = it->second.effects.clear(target);
    finish(world, *at, broadcast);
    return removed;
}

bool MobEffects::with_target(entity::EntityWorld& world, i32 id, const MobEffectRule& rule,
                             const MobEffectSink& broadcast) {
    const auto at = locate(world, id);
    if (!at) {
        return false;
    }
    Row&   row = row_of(world, at->handle, *at->state);
    Target target{*at->state, row, combat_, hurts_};
    rule(row.effects, target);
    finish(world, *at, broadcast);
    return true;
}

void MobEffects::tick(entity::EntityWorld& world, const MobEffectSink& broadcast) {
    for (auto it = rows_.begin(); it != rows_.end();) {
        const auto at = locate(world, it->first);
        if (!at) {
            // Gone, or dead: a death the server has not played yet keeps its
            // row until then, and a body with no health ticks nothing.
            it = it->second.died ? std::next(it) : rows_.erase(it);
            continue;
        }
        Row&   row = it->second;
        Target target{*at->state, row, combat_, hurts_};
        row.effects.tick(target);
        settle(world, at->handle, *at->state, row);
        flush(*at->state, row, broadcast);
        it = row.effects.empty() && !row.died ? rows_.erase(it) : std::next(it);
    }
}

void MobEffects::pairing(const entity::EntityState& state, const MobEffectSink& send) const {
    const auto it = rows_.find(state.network_id);
    if (it == rows_.end() || !send) {
        return;
    }
    const Row&          row = it->second;
    net::MetadataWriter fields;
    if (row.sent_flags != 0) {
        const u8 other = other_flags_ ? other_flags_(state) : u8{0};
        fields.byte_value(net::metadata::kSharedFlags,
                          static_cast<i8>(static_cast<u8>(other | row.sent_flags)));
    }
    if (row.sent_colour != 0) {
        fields.varint_value(kEffectColour, static_cast<i32>(row.sent_colour));
    }
    if (row.sent_ambient) {
        fields.boolean_value(kEffectAmbient, true);
    }
    if (!fields.empty()) {
        send(net::clientbound::kEntityMetadata,
             net::encode_entity_metadata(state.network_id, fields.take()));
    }
    // A late joiner was told the speed with its modifier (measured): after the
    // spawn's own attributes, which carry the bases alone.
    if (const gameplay::AttributeInstance* speed =
            row.attributes.get(gameplay::Attribute::MovementSpeed);
        speed != nullptr && !speed->modifiers().empty()) {
        const std::vector<u8> payload = speed_packet(state.network_id, row.attributes);
        if (!payload.empty()) {
            send(net::clientbound::kUpdateAttributes, payload);
        }
    }
}

const gameplay::ActiveEffects* MobEffects::effects_of(i32 id) const noexcept {
    const auto it = rows_.find(id);
    return it != rows_.end() ? &it->second.effects : nullptr;
}

f32 MobEffects::after_resistance(i32 id, gameplay::DamageKind kind, f32 amount) const noexcept {
    const auto it = rows_.find(id);
    return it != rows_.end() ? it->second.effects.after_resistance(kind, amount) : amount;
}

std::optional<f64> MobEffects::attack_damage(i32 id) const noexcept {
    const auto it = rows_.find(id);
    if (it == rows_.end()) {
        return std::nullopt;
    }
    return it->second.attributes.value(gameplay::Attribute::AttackDamage);
}

void MobEffects::write(const entity::EntityState& state, nbt::Tag& out) const {
    (void)out.erase("ActiveEffects");
    const auto it = rows_.find(state.network_id);
    if (it == rows_.end()) {
        return;
    }
    if (auto list = gameplay::save_effects(it->second.effects)) {
        (void)out.put("ActiveEffects", std::move(*list));
    }
}

void MobEffects::read(entity::EntityWorld& world, entity::EntityState& state,
                      const nbt::Tag& compound) {
    const nbt::Tag* list = compound.find("ActiveEffects");
    if (list == nullptr || list->list() == nullptr || list->list()->empty()) {
        return;
    }
    const entity::EntityHandle handle = world.find(state.network_id);
    if (handle == entity::kNoEntity) {
        return;
    }
    Row&   row = row_of(world, handle, state);
    Target target{state, row, combat_, hurts_};
    (void)gameplay::load_effects(row.effects, *list, target);
    settle(world, handle, state, row);
    // The storage clamped the health to the maximum before the effects were
    // back: a mob saved under Health Boost keeps what the file says it had.
    if (const nbt::Tag* health = compound.find("Health")) {
        state.health = std::clamp(static_cast<f32>(health->as_f64()), 0.0F, state.max_health);
    }
    // The spawn that follows carries all of it (`pairing`): nothing is owed.
    row.effects.clear_events();
    row.sent_flags   = effect_bits(row.effects);
    row.sent_colour  = row.effects.particle_color();
    row.sent_ambient = row.effects.all_ambient();
    row.sent_health  = state.health;
    if (row.effects.empty()) {
        rows_.erase(state.network_id);
    }
}

void MobEffects::forget(i32 id) { rows_.erase(id); }

}  // namespace ov::server
