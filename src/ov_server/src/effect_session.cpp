#include "effect_session.hpp"

#include "ov/gameplay/food.hpp"
#include "ov/protocol/effect_packets.hpp"
#include "ov/protocol/entity.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>

namespace ov::server {
namespace {

// The metadata the effects own. Every index measured on a real server
// (scripts/measure_effects.py, campaigns `metadata` and `player_meta`):
//   * 10, VarInt — the swirl colour; speed alone on a cow sent 0x33EBFF;
//   * 11, Boolean — "all visible effects are ambient"; a hidden effect alone
//     sent true;
//   * 15, Float, on a **player** — absorption; absorption II sent 8.0;
//   * 0, Byte — invisibility sets 0x20, glowing 0x40.
constexpr u8 kEffectColour  = 10;
constexpr u8 kEffectAmbient = 11;
constexpr u8 kAbsorption    = 15;
constexpr u8 kInvisibleBit  = 0x20;
constexpr u8 kGlowingBit    = 0x40;

/// The player, as the effect rules see one.
class PlayerTarget final : public gameplay::EffectTarget {
public:
    PlayerTarget(SurvivalSession& survival, gameplay::AttributeMap& attributes, const EffectIo& io,
                 const EffectBearer& bearer) noexcept
        : survival_{survival}, attributes_{attributes}, io_{io}, bearer_{bearer} {}

    [[nodiscard]] f32 health() const noexcept override { return survival_.health.health; }

    [[nodiscard]] f32 max_health() const noexcept override {
        return static_cast<f32>(attributes_.value(gameplay::Attribute::MaxHealth).value_or(20.0));
    }

    void heal(f32 amount) override {
        if (survival_.health.dead) {
            return;
        }
        survival_.health.health = std::min(max_health(), survival_.health.health + amount);
    }

    void hurt(gameplay::DamageKind kind, f32 amount,
              const gameplay::DamageConstants& constants) override {
        if (!bearer_.mortal || survival_.health.dead) {
            return;
        }
        // Built only when something hurts, which is at most once per effect
        // per tick: two std::function copies are not free.
        const SurvivalIo io{io_.send, io_.broadcast};
        (void)survival_.hurt(kind, amount, io, bearer_.entity_id, &constants);
    }

    void exhaust(f32 amount) override {
        if (bearer_.mortal) {
            survival_.exhaust(amount);
        }
    }

    void feed(i32 nutrition, f32 saturation_modifier) override {
        gameplay::eat(survival_.food, gameplay::FoodValue{{}, nutrition, saturation_modifier, false});
    }

    [[nodiscard]] f32 absorption() const noexcept override { return survival_.health.absorption; }

    void set_absorption(f32 amount) override {
        survival_.health.absorption = std::max(amount, 0.0F);
    }

    [[nodiscard]] gameplay::AttributeMap* attributes() noexcept override { return &attributes_; }

    void clamp_health() override {
        survival_.health.max_health = max_health();
        survival_.health.health     = std::min(survival_.health.health, survival_.health.max_health);
    }

private:
    SurvivalSession&        survival_;
    gameplay::AttributeMap& attributes_;
    const EffectIo&         io_;
    const EffectBearer&     bearer_;
};

[[nodiscard]] i8 as_stance_amplifier(i32 amplifier) noexcept {
    return static_cast<i8>(std::min(amplifier, 127));
}

}  // namespace

std::optional<gameplay::EffectInstance> parse_effect_spec(std::string_view spec) {
    std::array<std::string_view, 3> parts{};
    usize                           count = 0;
    while (count < parts.size()) {
        const auto colon = spec.find(':');
        // "minecraft:speed" has a colon of its own: keep the namespace with
        // the name when the next piece is not a number.
        if (count == 0 && colon != std::string_view::npos && spec.substr(0, colon) == "minecraft") {
            const auto next = spec.find(':', colon + 1);
            parts[count++]  = spec.substr(0, next);
            spec = next == std::string_view::npos ? std::string_view{} : spec.substr(next + 1);
            if (spec.empty()) {
                break;
            }
            continue;
        }
        parts[count++] = spec.substr(0, colon);
        if (colon == std::string_view::npos) {
            break;
        }
        spec = spec.substr(colon + 1);
    }
    if (count == 0 || parts[0].empty()) {
        return std::nullopt;
    }
    std::string name{parts[0]};
    if (name.find(':') == std::string::npos) {
        name = "minecraft:" + name;
    }
    const auto effect = gameplay::effect_from_name(name);
    if (!effect) {
        return std::nullopt;
    }
    gameplay::EffectInstance instance;
    instance.effect   = *effect;
    instance.duration = 600;
    if (count > 1) {
        i32        amplifier = 0;
        const auto parsed    = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(),
                                               amplifier);
        if (parsed.ec != std::errc{} || amplifier < 0 || amplifier > 255) {
            return std::nullopt;
        }
        instance.amplifier = static_cast<u8>(amplifier);
    }
    if (count > 2) {
        if (parts[2] == "infinite") {
            instance.duration = gameplay::kInfiniteDuration;
        } else {
            const auto parsed = std::from_chars(parts[2].data(), parts[2].data() + parts[2].size(),
                                                instance.duration);
            if (parsed.ec != std::errc{} || instance.duration <= 0) {
                return std::nullopt;
            }
        }
    }
    return instance;
}

void EffectSession::sync(SurvivalSession& survival) noexcept {
    // The damage path reads Resistance through the mitigation, and hunger's
    // regeneration reads the maximum through HealthState. Both are copies of
    // what the effects say, refreshed after every change.
    survival.mitigation.resistance = effects.amplifier(gameplay::Effect::Resistance);
    survival.health.max_health =
        static_cast<f32>(attributes.value(gameplay::Attribute::MaxHealth).value_or(20.0));
}

void EffectSession::flush(SurvivalSession& survival, const EffectIo& io,
                          const EffectBearer& bearer) {
    // ── Entity Effect / Remove Entity Effect, to the player alone ───────────
    for (const gameplay::EffectEvent& event : effects.events()) {
        if (!io.send) {
            break;
        }
        const i32 id = gameplay::effect_id(event.effect);
        if (event.change == gameplay::EffectChange::Removed) {
            const auto payload = net::encode_remove_entity_effect(bearer.entity_id, id);
            io.send(net::clientbound::kRemoveEntityEffect, payload);
            continue;
        }
        const gameplay::EffectInstance* instance = effects.get(event.effect);
        if (instance == nullptr) {
            continue;
        }
        net::EntityEffect packet;
        packet.entity_id = bearer.entity_id;
        packet.effect_id = id;
        packet.amplifier = instance->amplifier;
        packet.duration  = instance->duration;
        packet.flags     = static_cast<u8>((instance->ambient ? net::effect_flags::kAmbient : 0) |
                                       (instance->visible ? net::effect_flags::kVisible : 0) |
                                       (instance->show_icon ? net::effect_flags::kShowIcon : 0));
        if (event.effect == gameplay::Effect::Darkness) {
            // Captured: a fresh darkness arrives with its fade state. This
            // server does not run the fade, so what is sent is the start of it.
            packet.factor = net::EffectFactorData{};
        }
        io.send(net::clientbound::kEntityEffect, net::encode_entity_effect(packet));
    }
    effects.clear_events();

    // ── Update Attributes: the ones that changed, with their modifiers ──────
    std::array<net::AttributeProperty, gameplay::kAttributeCount> properties{};
    std::array<std::array<net::WireModifier, gameplay::AttributeInstance::kCapacity>,
               gameplay::kAttributeCount>
          modifiers{};
    usize count = 0;
    for (usize a = 0; a < gameplay::kAttributeCount; ++a) {
        gameplay::AttributeInstance* instance = attributes.get(static_cast<gameplay::Attribute>(a));
        if (instance == nullptr || !instance->dirty()) {
            continue;
        }
        usize n = 0;
        for (const gameplay::AttributeModifier& modifier : instance->modifiers()) {
            modifiers[a][n++] =
                net::WireModifier{modifier.uuid, modifier.amount, static_cast<u8>(modifier.operation)};
        }
        properties[count++] = net::AttributeProperty{
            gameplay::attribute_info(instance->attribute()).name, instance->base(),
            std::span<const net::WireModifier>{modifiers[a].data(), n}};
        instance->clear_dirty();
    }
    if (count > 0) {
        const auto payload = net::encode_update_attributes_full(
            bearer.entity_id, std::span<const net::AttributeProperty>{properties.data(), count});
        if (io.send) {
            io.send(net::clientbound::kUpdateAttributes, payload);
        }
        if (io.broadcast) {
            io.broadcast(net::clientbound::kUpdateAttributes, payload);
        }
    }

    // ── Metadata: colour, ambient, the two flag bits, absorption ────────────
    const u32  colour     = effects.particle_color();
    const bool ambient    = effects.all_ambient();
    const u8   bits       = static_cast<u8>(
        (effects.has(gameplay::Effect::Invisibility) ? kInvisibleBit : 0) |
        (effects.has(gameplay::Effect::Glowing) ? kGlowingBit : 0));
    const f32  absorption = survival.health.absorption;

    net::MetadataWriter fields;
    if (bits != sent_flags_) {
        fields.byte_value(net::metadata::kSharedFlags,
                          static_cast<i8>(static_cast<u8>(bearer.shared_flags | bits)));
    }
    if (colour != sent_color_) {
        fields.varint_value(kEffectColour, static_cast<i32>(colour));
    }
    if (ambient != sent_ambient_) {
        fields.boolean_value(kEffectAmbient, ambient);
    }
    if (std::abs(absorption - sent_absorption_) > 0.0001F) {
        fields.float_value(kAbsorption, absorption);
    }
    sent_flags_      = bits;
    sent_color_      = colour;
    sent_ambient_    = ambient;
    sent_absorption_ = absorption;
    if (!fields.empty()) {
        const auto payload = net::encode_entity_metadata(bearer.entity_id, fields.take());
        if (io.send) {
            io.send(net::clientbound::kEntityMetadata, payload);
        }
        if (io.broadcast) {
            io.broadcast(net::clientbound::kEntityMetadata, payload);
        }
    }
}

gameplay::AddResult EffectSession::apply(const gameplay::EffectInstance& instance,
                                         SurvivalSession& survival, const EffectIo& io,
                                         const EffectBearer& bearer) {
    PlayerTarget target{survival, attributes, io, bearer};
    const gameplay::AddResult result = effects.add(instance, target);
    sync(survival);
    flush(survival, io, bearer);
    return result;
}

bool EffectSession::remove(gameplay::Effect effect, SurvivalSession& survival, const EffectIo& io,
                           const EffectBearer& bearer) {
    PlayerTarget target{survival, attributes, io, bearer};
    const bool   removed = effects.remove(effect, target);
    sync(survival);
    flush(survival, io, bearer);
    return removed;
}

usize EffectSession::clear(SurvivalSession& survival, const EffectIo& io,
                           const EffectBearer& bearer) {
    PlayerTarget target{survival, attributes, io, bearer};
    const usize  removed = effects.clear(target);
    sync(survival);
    flush(survival, io, bearer);
    return removed;
}

void EffectSession::tick(SurvivalSession& survival, const EffectIo& io,
                         const EffectBearer& bearer) {
    if (survival.health.dead || survival.awaiting_respawn) {
        return;
    }
    PlayerTarget target{survival, attributes, io, bearer};
    effects.tick(target);
    sync(survival);
    flush(survival, io, bearer);
}

bool EffectSession::on_consumed(std::string_view item, SurvivalSession& survival,
                                const EffectIo& io, const EffectBearer& bearer) {
    PlayerTarget target{survival, attributes, io, bearer};
    const bool   handled = gameplay::consume_effects(effects, item, target, random_);
    sync(survival);
    flush(survival, io, bearer);
    return handled;
}

// ── brewing ──
void EffectSession::with_target(
    SurvivalSession& survival, const EffectIo& io, const EffectBearer& bearer,
    const std::function<void(gameplay::ActiveEffects&, gameplay::EffectTarget&)>& rule) {
    PlayerTarget target{survival, attributes, io, bearer};
    rule(effects, target);
    sync(survival);
    flush(survival, io, bearer);
}

void EffectSession::on_death() {
    effects.forget();
    attributes = gameplay::AttributeMap::player();
    // The client resets its own copy on Respawn, so what it holds afterwards is
    // the defaults; the next flush re-sends the attributes, which is harmless.
    sent_color_      = 0;
    sent_ambient_    = false;
    sent_absorption_ = 0.0F;
    sent_flags_      = 0;
}

void EffectSession::announce(SurvivalSession& survival, const EffectIo& io,
                             const EffectBearer& bearer) {
    sync(survival);
    flush(survival, io, bearer);
}

gameplay::Stance EffectSession::dig_stance(bool on_ground) const noexcept {
    gameplay::Stance stance;
    stance.on_ground = on_ground;
    const i32 haste  = std::max(effects.amplifier(gameplay::Effect::Haste),
                                effects.amplifier(gameplay::Effect::ConduitPower));
    stance.haste          = as_stance_amplifier(haste);
    stance.mining_fatigue = as_stance_amplifier(effects.amplifier(gameplay::Effect::MiningFatigue));
    return stance;
}

i8 EffectSession::strength() const noexcept {
    return as_stance_amplifier(effects.amplifier(gameplay::Effect::Strength));
}

i8 EffectSession::weakness() const noexcept {
    return as_stance_amplifier(effects.amplifier(gameplay::Effect::Weakness));
}

bool EffectSession::blind() const noexcept { return effects.has(gameplay::Effect::Blindness); }

bool EffectSession::breathes_underwater() const noexcept {
    return effects.has(gameplay::Effect::WaterBreathing) ||
           effects.has(gameplay::Effect::ConduitPower);
}

i32 EffectSession::jump_boost() const noexcept {
    return effects.amplifier(gameplay::Effect::JumpBoost);
}

bool EffectSession::slow_falling() const noexcept {
    return effects.has(gameplay::Effect::SlowFalling);
}

}  // namespace ov::server
