#include "ov/gameplay/effects.hpp"

#include <algorithm>
#include <charconv>

namespace ov::gameplay {
namespace {

[[nodiscard]] constexpr net::Uuid uuid(u64 most, u64 least) noexcept {
    return net::Uuid{most, least};
}

[[nodiscard]] constexpr EffectModifier modifier(Attribute attribute, net::Uuid id, f64 per_level,
                                                AttributeOperation operation) noexcept {
    return EffectModifier{attribute, id, per_level, operation};
}

// The thirty-three effects.
//
// Colours: the living-entity metadata index 10 a cow sent with that effect
// alone at amplifier 0, measured one effect at a time (campaign `metadata`).
//
// Modifiers: read off `data get entity … Attributes` under each effect at
// amplifiers 0, 1 and 4, on a zombie and on a player (campaign `modifiers`).
// Exactly these nine effects carried one; the other twenty-four carried none
// on either. The four percentage amounts are **floats** — speed's 0.2 came
// back as 0.20000000298023224 — so they are written as a float cast, and the
// three-level check (amplifier 4 is `amount * 5` in double) is in the tests.
constexpr std::array<EffectInfo, kEffectCount> kEffects{{
    {"minecraft:speed", 1, 0x33EBFF, false,
     modifier(Attribute::MovementSpeed, uuid(0x91AEAA56376B4498ULL, 0x935B2F7F68070635ULL),
              static_cast<f64>(0.2F), AttributeOperation::MultiplyTotal)},
    {"minecraft:slowness", 2, 0x8BAFE0, false,
     modifier(Attribute::MovementSpeed, uuid(0x7107DE5E7CE84030ULL, 0x940E514C1F160890ULL),
              static_cast<f64>(-0.15F), AttributeOperation::MultiplyTotal)},
    {"minecraft:haste", 3, 0xD9C043, false,
     modifier(Attribute::AttackSpeed, uuid(0xAF8B6E3F33284C0AULL, 0xAA365BA2BB9DBEF3ULL),
              static_cast<f64>(0.1F), AttributeOperation::MultiplyTotal)},
    {"minecraft:mining_fatigue", 4, 0x4A4217, false,
     modifier(Attribute::AttackSpeed, uuid(0x55FCED67E92A486EULL, 0x9800B47F202C4386ULL),
              static_cast<f64>(-0.1F), AttributeOperation::MultiplyTotal)},
    {"minecraft:strength", 5, 0xFFC700, false,
     modifier(Attribute::AttackDamage, uuid(0x648D70646A604F59ULL, 0x8ABEC2C23A6DD7A9ULL), 3.0,
              AttributeOperation::Addition)},
    {"minecraft:instant_health", 6, 0, true, std::nullopt},  // never shown: applied, not kept
    {"minecraft:instant_damage", 7, 0, true, std::nullopt},  // never shown: applied, not kept
    {"minecraft:jump_boost", 8, 0xFDFF84, false, std::nullopt},
    {"minecraft:nausea", 9, 0x551D4A, false, std::nullopt},
    {"minecraft:regeneration", 10, 0xCD5CAB, false, std::nullopt},
    {"minecraft:resistance", 11, 0x9146F0, false, std::nullopt},
    {"minecraft:fire_resistance", 12, 0xFF9900, false, std::nullopt},
    {"minecraft:water_breathing", 13, 0x98DAC0, false, std::nullopt},
    {"minecraft:invisibility", 14, 0xF6F6F6, false, std::nullopt},
    {"minecraft:blindness", 15, 0x1F1F23, false, std::nullopt},
    {"minecraft:night_vision", 16, 0xC2FF66, false, std::nullopt},
    {"minecraft:hunger", 17, 0x587653, false, std::nullopt},
    {"minecraft:weakness", 18, 0x484D48, false,
     modifier(Attribute::AttackDamage, uuid(0x22653B89116E49DCULL, 0x9B6B9971489B5BE5ULL), -4.0,
              AttributeOperation::Addition)},
    {"minecraft:poison", 19, 0x87A363, false, std::nullopt},
    {"minecraft:wither", 20, 0x736156, false, std::nullopt},
    {"minecraft:health_boost", 21, 0xF87D23, false,
     modifier(Attribute::MaxHealth, uuid(0x5D6F0BA2118646ACULL, 0xB896C61C5CEE99CCULL), 4.0,
              AttributeOperation::Addition)},
    {"minecraft:absorption", 22, 0x2552A5, false, std::nullopt},
    {"minecraft:saturation", 23, 0xF82423, false, std::nullopt},
    {"minecraft:glowing", 24, 0x94A061, false, std::nullopt},
    {"minecraft:levitation", 25, 0xCEFFFF, false, std::nullopt},
    {"minecraft:luck", 26, 0x59C106, false,
     modifier(Attribute::Luck, uuid(0x03C3C89D70374B42ULL, 0x869FB146BCB64D2EULL), 1.0,
              AttributeOperation::Addition)},
    {"minecraft:unluck", 27, 0xC0A44D, false,
     modifier(Attribute::Luck, uuid(0xCC5AF1422BD24215ULL, 0xB6362605AED11727ULL), -1.0,
              AttributeOperation::Addition)},
    {"minecraft:slow_falling", 28, 0xF3CFB9, false, std::nullopt},
    {"minecraft:conduit_power", 29, 0x1DC2D1, false, std::nullopt},
    {"minecraft:dolphins_grace", 30, 0x88A3BE, false, std::nullopt},
    {"minecraft:bad_omen", 31, 0x0B6138, false, std::nullopt},
    {"minecraft:hero_of_the_village", 32, 0x44FF44, false, std::nullopt},
    {"minecraft:darkness", 33, 0x292721, false, std::nullopt},
}};

/// The description id the modifier names are built from: "effect.minecraft.speed".
constexpr std::array<std::string_view, kEffectCount> kDescriptions{
    "effect.minecraft.speed",          "effect.minecraft.slowness",
    "effect.minecraft.haste",          "effect.minecraft.mining_fatigue",
    "effect.minecraft.strength",       "effect.minecraft.instant_health",
    "effect.minecraft.instant_damage", "effect.minecraft.jump_boost",
    "effect.minecraft.nausea",         "effect.minecraft.regeneration",
    "effect.minecraft.resistance",     "effect.minecraft.fire_resistance",
    "effect.minecraft.water_breathing", "effect.minecraft.invisibility",
    "effect.minecraft.blindness",      "effect.minecraft.night_vision",
    "effect.minecraft.hunger",         "effect.minecraft.weakness",
    "effect.minecraft.poison",         "effect.minecraft.wither",
    "effect.minecraft.health_boost",   "effect.minecraft.absorption",
    "effect.minecraft.saturation",     "effect.minecraft.glowing",
    "effect.minecraft.levitation",     "effect.minecraft.luck",
    "effect.minecraft.unluck",         "effect.minecraft.slow_falling",
    "effect.minecraft.conduit_power",  "effect.minecraft.dolphins_grace",
    "effect.minecraft.bad_omen",       "effect.minecraft.hero_of_the_village",
    "effect.minecraft.darkness",
};

[[nodiscard]] constexpr usize index_of(Effect effect) noexcept {
    return static_cast<usize>(effect) - 1;
}

/// Java's `int << n`: the count is taken modulo 32 and the result wraps.
/// Amplifier 32 is amplifier 0 again, and instant damage at 29 is negative.
[[nodiscard]] i32 java_shl(i32 value, u32 count) noexcept {
    return static_cast<i32>(static_cast<u32>(value) << (count & 31U));
}

[[nodiscard]] i32 java_shr(i32 value, u32 count) noexcept { return value >> (count & 31U); }

/// "a runs out before b": finite, and shorter or facing an infinite one.
[[nodiscard]] bool shorter(const EffectInstance& a, const EffectInstance& b) noexcept {
    return !a.infinite() && (a.duration < b.duration || b.infinite());
}

/// What an effect does on a tick it acts on — or once, for an instant one.
void act(Effect effect, u8 amplifier, EffectTarget& target, const DamageConstants& window) {
    const bool undead = target.body() == EffectTarget::Body::Undead;
    switch (effect) {
        case Effect::Regeneration:
            // Only when hurt, one point. 174 of 174 cows.
            if (target.health() < target.max_health()) {
                target.heal(1.0F);
            }
            break;
        case Effect::Poison:
            // Never the last point: three cows at 3 health under poison I, IV
            // and VI all ended at exactly 1.0.
            if (target.health() > 1.0F) {
                target.hurt(DamageKind::Magic, 1.0F, window);
            }
            break;
        case Effect::Wither:
            // And this one does take it: the cow under wither IV at 3 health
            // did not survive.
            target.hurt(DamageKind::Wither, 1.0F, window);
            break;
        case Effect::Hunger:
            // 0.5 exhaustion over five seconds at level I, measured.
            target.exhaust(0.005F * static_cast<f32>(amplifier + 1));
            break;
        case Effect::Saturation:
            // amplifier + 1 food and twice that in saturation per application,
            // measured at amplifiers 0, 1, 3 and 9.
            target.feed(static_cast<i32>(amplifier) + 1, 1.0F);
            break;
        case Effect::InstantHealth:
        case Effect::InstantDamage: {
            // 4 << amp healed and 6 << amp dealt on cows at amplifiers 0..5;
            // inverted on zombies.
            const bool heals = (effect == Effect::InstantHealth) != undead;
            if (heals) {
                target.heal(static_cast<f32>(std::max(java_shl(4, amplifier), 0)));
            } else {
                target.hurt(DamageKind::Magic, static_cast<f32>(java_shl(6, amplifier)), window);
            }
            break;
        }
        default: break;
    }
}

}  // namespace

const EffectInfo& effect_info(Effect effect) noexcept {
    const usize index = index_of(effect);
    return kEffects[index < kEffects.size() ? index : 0];
}

std::optional<Effect> effect_from_id(i32 id) noexcept {
    if (id < 1 || id > static_cast<i32>(kEffectCount)) {
        return std::nullopt;
    }
    return static_cast<Effect>(id);
}

std::optional<Effect> effect_from_name(std::string_view name) noexcept {
    for (const EffectInfo& info : kEffects) {
        if (info.name == name) {
            return static_cast<Effect>(info.id);
        }
    }
    return std::nullopt;
}

usize modifier_name(Effect effect, u8 amplifier, char* out, usize capacity) noexcept {
    const std::string_view description = kDescriptions[index_of(effect)];
    if (capacity < description.size() + 5) {
        return 0;
    }
    std::copy(description.begin(), description.end(), out);
    usize length  = description.size();
    out[length++] = ' ';
    const auto [end, error] =
        std::to_chars(out + length, out + capacity, static_cast<i32>(amplifier));
    if (error != std::errc{}) {
        return 0;
    }
    return static_cast<usize>(end - out);
}

f64 modifier_amount(const EffectModifier& modifier, u8 amplifier) noexcept {
    return modifier.per_level * static_cast<f64>(static_cast<i32>(amplifier) + 1);
}

DamageConstants effect_damage_constants(const DamageConstants& base) noexcept {
    // "More than ten" instead of "ten or more": the measured gap between two
    // effect hits is ten ticks (poison IV and V: 1 hit up to D = 10, 2 at
    // D = 11), where two console hits need eleven.
    DamageConstants out          = base;
    out.partial_damage_threshold = base.partial_damage_threshold + 1;
    return out;
}

bool accepts(EffectTarget::Body body, Effect effect) noexcept {
    switch (body) {
        case EffectTarget::Body::Undead:
            return effect != Effect::Regeneration && effect != Effect::Poison;
        case EffectTarget::Body::Arthropod: return effect != Effect::Poison;
        case EffectTarget::Body::Ordinary: return true;
    }
    return true;
}

bool acts_on(Effect effect, u8 amplifier, i64 t) noexcept {
    i32 interval = 0;
    switch (effect) {
        case Effect::Regeneration: interval = java_shr(50, amplifier); break;
        case Effect::Poison: interval = java_shr(25, amplifier); break;
        case Effect::Wither: interval = java_shr(40, amplifier); break;
        case Effect::Hunger: return true;
        // Once every twenty ticks: one application over one second, three over
        // three, at every amplifier measured. See effets.md for what that
        // does and does not pin down.
        case Effect::Saturation: return t % 20 == 0;
        default: return false;
    }
    return interval > 0 ? t % interval == 0 : true;
}

// ── ActiveEffects ───────────────────────────────────────────────────────────

void ActiveEffects::record(Effect effect, EffectChange change) noexcept {
    if (event_count_ < events_.size()) {
        events_[event_count_++] = EffectEvent{effect, change};
    }
}

void ActiveEffects::started(const EffectInstance& instance, EffectTarget& target) {
    const EffectInfo& info = effect_info(instance.effect);
    if (info.modifier) {
        if (AttributeMap* map = target.attributes()) {
            if (AttributeInstance* attribute = map->get(info.modifier->attribute)) {
                // AlreadyPresent is not an error here: a refresh removes and
                // re-adds, and a load re-adds what a save may have kept.
                (void)attribute->add_modifier(AttributeModifier{
                    info.modifier->uuid, kDescriptions[index_of(instance.effect)],
                    modifier_amount(*info.modifier, instance.amplifier),
                    info.modifier->operation});
            }
        }
    }
    if (instance.effect == Effect::Absorption) {
        target.set_absorption(target.absorption() +
                              static_cast<f32>(4 * (static_cast<i32>(instance.amplifier) + 1)));
    }
}

void ActiveEffects::ended(const EffectInstance& instance, EffectTarget& target) {
    const EffectInfo& info = effect_info(instance.effect);
    if (info.modifier) {
        if (AttributeMap* map = target.attributes()) {
            if (AttributeInstance* attribute = map->get(info.modifier->attribute)) {
                attribute->remove_modifier(info.modifier->uuid);
            }
        }
    }
    if (instance.effect == Effect::Absorption) {
        // Measured: a pool drawn down to one point by a hit, then cleared,
        // read zero — the subtraction is clamped rather than going negative.
        target.set_absorption(std::max(
            target.absorption() -
                static_cast<f32>(4 * (static_cast<i32>(instance.amplifier) + 1)),
            0.0F));
    }
    if (instance.effect == Effect::HealthBoost) {
        target.clamp_health();
    }
}

void ActiveEffects::hide(Slot& slot, usize at, const EffectInstance& instance) {
    if (at >= kHiddenDepth) {
        ++dropped_hidden_;
        return;
    }
    if (slot.depth == kHiddenDepth) {
        // The deepest is the weakest and the longest-lived of the chain; it is
        // the one given up, and counted.
        ++dropped_hidden_;
        --slot.depth;
    }
    for (usize i = slot.depth; i > at; --i) {
        slot.chain[i] = slot.chain[i - 1];
    }
    slot.chain[at] = instance;
    ++slot.depth;
}

bool ActiveEffects::merge(Slot& slot, usize at, const EffectInstance& other) {
    bool changed = false;
    if (other.amplifier > slot.chain[at].amplifier) {
        // Stronger. The weaker is kept beneath it only if it would have
        // outlasted it — measured both ways: speed I for 30 s under speed III
        // for 5 s was kept, speed I for 5 s under speed III for 30 s was not.
        if (shorter(other, slot.chain[at])) {
            const EffectInstance previous = slot.chain[at];
            hide(slot, at + 1, previous);
        }
        slot.chain[at].amplifier = other.amplifier;
        slot.chain[at].duration  = other.duration;
        changed                  = true;
    } else if (shorter(slot.chain[at], other)) {
        if (other.amplifier == slot.chain[at].amplifier) {
            // Equal and longer: the duration is extended. Equal and shorter is
            // the branch not taken — measured, speed II for 5 s after 30 s left
            // the 30 s alone.
            slot.chain[at].duration = other.duration;
            changed                 = true;
        } else if (at + 1 >= slot.depth) {
            // Weaker but longer: into hiding. Measured.
            hide(slot, at + 1, other);
        } else {
            (void)merge(slot, at + 1, other);
        }
    }
    EffectInstance& current = slot.chain[at];
    if ((!other.ambient && current.ambient) || changed) {
        current.ambient = other.ambient;
        changed         = true;
    }
    if (other.visible != current.visible) {
        current.visible = other.visible;
        changed         = true;
    }
    if (other.show_icon != current.show_icon) {
        current.show_icon = other.show_icon;
        changed           = true;
    }
    return changed;
}

AddResult ActiveEffects::add(const EffectInstance& instance, EffectTarget& target) {
    if (!accepts(target.body(), instance.effect)) {
        return AddResult::Immune;
    }
    const EffectInfo& info = effect_info(instance.effect);
    if (info.instantaneous) {
        // Applied once, as it arrives. The hit is a console-phase hit, not an
        // effect-phase one — it lands where the command runs — so it uses the
        // survival window, not `effect_damage_constants`.
        act(instance.effect, instance.amplifier, target, base_window_);
        return AddResult::Applied;
    }
    Slot& slot = slots_[index_of(instance.effect)];
    if (slot.depth == 0) {
        slot.chain[0] = instance;
        slot.depth    = 1;
        started(instance, target);
        record(instance.effect, EffectChange::Added);
        return AddResult::Added;
    }
    if (!merge(slot, 0, instance)) {
        // Including when only the hidden chain changed: the client is never
        // told about a hidden effect, and nothing it can see moved.
        return AddResult::Unchanged;
    }
    // A changed effect is refreshed: its modifier comes off and goes back on
    // at the new amplifier.
    ended(slot.chain[0], target);
    started(slot.chain[0], target);
    record(instance.effect, EffectChange::Updated);
    return AddResult::Updated;
}

bool ActiveEffects::remove(Effect effect, EffectTarget& target) {
    Slot& slot = slots_[index_of(effect)];
    if (slot.depth == 0) {
        return false;
    }
    const EffectInstance top = slot.chain[0];
    slot.depth               = 0;
    ended(top, target);
    record(effect, EffectChange::Removed);
    return true;
}

usize ActiveEffects::clear(EffectTarget& target) {
    usize removed = 0;
    for (usize i = 0; i < kEffectCount; ++i) {
        if (remove(static_cast<Effect>(i + 1), target)) {
            ++removed;
        }
    }
    return removed;
}

void ActiveEffects::forget() noexcept {
    for (Slot& slot : slots_) {
        slot.depth = 0;
    }
    event_count_ = 0;
}

void ActiveEffects::tick(EffectTarget& target) {
    ++age_;
    const DamageConstants window = effect_damage_constants(base_window_);
    for (usize i = 0; i < kEffectCount; ++i) {
        Slot& slot = slots_[i];
        if (slot.depth == 0) {
            continue;
        }
        // The counter the interval is read against: the duration left, or,
        // with none to count down, the holder's own age.
        const EffectInstance top = slot.chain[0];
        const i64            t   = top.infinite() ? age_ : static_cast<i64>(top.duration);
        if (top.duration != 0 && acts_on(top.effect, top.amplifier, t)) {
            act(top.effect, top.amplifier, target, window);
        }
        // Every instance of the chain counts down together — measured, a
        // hidden speed I went from 599 to 588 while the speed III over it went
        // from 99 to 88.
        for (usize d = 0; d < slot.depth; ++d) {
            EffectInstance& instance = slot.chain[d];
            if (!instance.infinite() && instance.duration > 0) {
                --instance.duration;
            }
        }
        if (slot.chain[0].infinite() || slot.chain[0].duration > 0) {
            continue;
        }
        if (slot.depth > 1) {
            // The hidden one comes back, with what it has left.
            for (usize d = 1; d < slot.depth; ++d) {
                slot.chain[d - 1] = slot.chain[d];
            }
            --slot.depth;
            ended(slot.chain[0], target);
            started(slot.chain[0], target);
            record(slot.chain[0].effect, EffectChange::Updated);
        } else {
            slot.depth = 0;
            ended(top, target);
            record(top.effect, EffectChange::Removed);
        }
    }
}

const EffectInstance* ActiveEffects::get(Effect effect) const noexcept {
    const Slot& slot = slots_[index_of(effect)];
    return slot.depth > 0 ? &slot.chain[0] : nullptr;
}

bool ActiveEffects::has(Effect effect) const noexcept { return get(effect) != nullptr; }

i32 ActiveEffects::amplifier(Effect effect) const noexcept {
    const EffectInstance* instance = get(effect);
    return instance != nullptr ? static_cast<i32>(instance->amplifier) : -1;
}

const EffectInstance* ActiveEffects::hidden(Effect effect, usize depth) const noexcept {
    const Slot& slot = slots_[index_of(effect)];
    return depth + 1 < slot.depth + 1 && depth < slot.depth ? &slot.chain[depth] : nullptr;
}

usize ActiveEffects::size() const noexcept {
    usize count = 0;
    for (const Slot& slot : slots_) {
        count += slot.depth > 0 ? 1 : 0;
    }
    return count;
}

u32 ActiveEffects::particle_color() const noexcept {
    // A weighted mean of the visible effects' colours, weighted by level, in
    // float, in exactly this order: the integer product of weight and channel,
    // over 255, summed; then over the total weight, times 255, truncated.
    //
    // The order is measured, not chosen. Four orderings of the same mean were
    // tried against 64 measured cases (31 colours alone, 27 single effects at
    // amplifiers 2, 4 and 6, six mixtures): this one reproduces all 64, the
    // others fail 15 to 20. It is also why the game's colours *drift*: speed V
    // alone sends 0x33EAFF, not speed's own 0x33EBFF, and so does this.
    f32 red   = 0.0F;
    f32 green = 0.0F;
    f32 blue  = 0.0F;
    i32 total = 0;
    for (const Slot& slot : slots_) {
        if (slot.depth == 0 || !slot.chain[0].visible) {
            continue;
        }
        const u32 color  = effect_info(slot.chain[0].effect).color;
        const i32 weight = static_cast<i32>(slot.chain[0].amplifier) + 1;
        red += static_cast<f32>(weight * static_cast<i32>((color >> 16U) & 0xFFU)) / 255.0F;
        green += static_cast<f32>(weight * static_cast<i32>((color >> 8U) & 0xFFU)) / 255.0F;
        blue += static_cast<f32>(weight * static_cast<i32>(color & 0xFFU)) / 255.0F;
        total += weight;
    }
    if (total == 0) {
        return 0;
    }
    const f32 scale = static_cast<f32>(total);
    const auto channel = [&](f32 sum) {
        return static_cast<u32>(static_cast<i32>(sum / scale * 255.0F)) & 0xFFU;
    };
    return (channel(red) << 16U) | (channel(green) << 8U) | channel(blue);
}

bool ActiveEffects::all_ambient() const noexcept {
    bool any = false;
    for (const Slot& slot : slots_) {
        if (slot.depth == 0) {
            continue;
        }
        any = true;
        if (slot.chain[0].visible && !slot.chain[0].ambient) {
            return false;
        }
    }
    // No effect at all is not "all ambient": the metadata goes back to false
    // when the last effect is cleared, measured.
    return any;
}

f32 ActiveEffects::after_resistance(DamageKind kind, f32 amount) const noexcept {
    return gameplay::after_resistance(kind, amount, amplifier(Effect::Resistance));
}

// ── Food ────────────────────────────────────────────────────────────────────

namespace {

// Measured by the `food` campaign: the bot ate each item and its ActiveEffects
// were read back. Every duration came back exactly 17 ticks short of a round
// number — 83, 2383, 5983, 283, 583, 1183 — the same 17 for every item, which
// is the gap between the use completing and the read. So the durations are
// pinned to the tick, relative to each other.
//
// The three chances are **not** pinned. They are the values consistent with
// the frequencies counted over repeated eating (effets.md § « aliments »),
// within their intervals; a campaign cannot tell 0.8 from 0.79.
constexpr std::array<FoodEffect, 13> kFoodEffects{{
    {"minecraft:golden_apple", Effect::Regeneration, 100, 1, 1.0F},
    {"minecraft:golden_apple", Effect::Absorption, 2400, 0, 1.0F},
    {"minecraft:enchanted_golden_apple", Effect::Regeneration, 400, 1, 1.0F},
    {"minecraft:enchanted_golden_apple", Effect::Resistance, 6000, 0, 1.0F},
    {"minecraft:enchanted_golden_apple", Effect::FireResistance, 6000, 0, 1.0F},
    {"minecraft:enchanted_golden_apple", Effect::Absorption, 2400, 3, 1.0F},
    {"minecraft:pufferfish", Effect::Poison, 1200, 1, 1.0F},
    {"minecraft:pufferfish", Effect::Hunger, 300, 2, 1.0F},
    {"minecraft:pufferfish", Effect::Nausea, 300, 0, 1.0F},
    {"minecraft:spider_eye", Effect::Poison, 100, 0, 1.0F},
    {"minecraft:rotten_flesh", Effect::Hunger, 600, 0, 0.8F},
    {"minecraft:chicken", Effect::Hunger, 600, 0, 0.3F},
    {"minecraft:poisonous_potato", Effect::Poison, 100, 0, 0.6F},
}};

}  // namespace

std::span<const FoodEffect> food_effects(std::string_view item) noexcept {
    usize first = kFoodEffects.size();
    usize count = 0;
    for (usize i = 0; i < kFoodEffects.size(); ++i) {
        if (kFoodEffects[i].item == item) {
            if (count == 0) {
                first = i;
            }
            ++count;
        }
    }
    return std::span<const FoodEffect>{kFoodEffects.data() + first, count};
}

bool consume_effects(ActiveEffects& effects, std::string_view item, EffectTarget& target,
                     math::XoroshiroRandomSource& random) {
    if (item == "minecraft:milk_bucket") {
        (void)effects.clear(target);
        return true;
    }
    if (item == "minecraft:honey_bottle") {
        (void)effects.remove(Effect::Poison, target);
        return true;
    }
    const std::span<const FoodEffect> list = food_effects(item);
    if (list.empty()) {
        return false;
    }
    for (const FoodEffect& food : list) {
        // One draw per listed effect, certain ones included, so the stream
        // does not depend on which chances happen to be 1.
        const f32 draw = random.next_float();
        if (draw < food.probability) {
            EffectInstance instance;
            instance.effect    = food.effect;
            instance.duration  = food.duration;
            instance.amplifier = food.amplifier;
            (void)effects.add(instance, target);
        }
    }
    return true;
}

// ── Save format ─────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] nbt::Tag save_one(const std::array<EffectInstance, ActiveEffects::kHiddenDepth>& chain,
                                usize depth, usize at) {
    const EffectInstance& instance = chain[at];
    nbt::Tag              compound = nbt::Tag::make_compound();
    compound.put("Id", nbt::Tag{effect_id(instance.effect)});
    // A byte: amplifier 255 is saved as -1, exactly as the real file would.
    compound.put("Amplifier", nbt::Tag{static_cast<i8>(instance.amplifier)});
    compound.put("Duration", nbt::Tag{instance.duration});
    compound.put("Ambient", nbt::Tag::make_bool(instance.ambient));
    compound.put("ShowParticles", nbt::Tag::make_bool(instance.visible));
    compound.put("ShowIcon", nbt::Tag::make_bool(instance.show_icon));
    if (at + 1 < depth) {
        compound.put("HiddenEffect", save_one(chain, depth, at + 1));
    }
    if (instance.effect == Effect::Darkness) {
        // The fade state the real file carries for darkness. This server does
        // not run the fade, so what is written is the state of a fresh
        // darkness — the one the Entity Effect capture carried.
        nbt::Tag factor = nbt::Tag::make_compound();
        factor.put("padding_duration", nbt::Tag{i32{22}});
        factor.put("factor_start", nbt::Tag{0.0F});
        factor.put("factor_target", nbt::Tag{1.0F});
        factor.put("factor_current", nbt::Tag{0.0F});
        factor.put("ticks_active", nbt::Tag{i32{0}});
        factor.put("factor_previous_frame", nbt::Tag{0.0F});
        factor.put("had_effect_last_tick", nbt::Tag::make_bool(false));
        compound.put("FactorCalculationData", std::move(factor));
    }
    return compound;
}

[[nodiscard]] std::optional<EffectInstance> load_one(const nbt::Tag& compound) {
    const nbt::Tag* id = compound.find("Id");
    if (id == nullptr) {
        return std::nullopt;
    }
    const auto effect = effect_from_id(static_cast<i32>(id->as_i64(-1)));
    if (!effect) {
        return std::nullopt;
    }
    EffectInstance out;
    out.effect = *effect;
    if (const nbt::Tag* tag = compound.find("Amplifier")) {
        out.amplifier = static_cast<u8>(static_cast<i8>(tag->as_i64(0)));
    }
    if (const nbt::Tag* tag = compound.find("Duration")) {
        out.duration = static_cast<i32>(tag->as_i64(0));
    }
    if (const nbt::Tag* tag = compound.find("Ambient")) {
        out.ambient = tag->as_bool();
    }
    out.visible = true;
    if (const nbt::Tag* tag = compound.find("ShowParticles")) {
        out.visible = tag->as_bool(true);
    }
    // Absent means "same as the particles", which is also what the command
    // does when it hides them.
    out.show_icon = out.visible;
    if (const nbt::Tag* tag = compound.find("ShowIcon")) {
        out.show_icon = tag->as_bool(out.visible);
    }
    return out;
}

}  // namespace

std::optional<nbt::Tag> save_effects(const ActiveEffects& effects) {
    if (effects.empty()) {
        return std::nullopt;
    }
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Compound);
    for (usize i = 0; i < kEffectCount; ++i) {
        const auto effect = static_cast<Effect>(i + 1);
        if (!effects.has(effect)) {
            continue;
        }
        std::array<EffectInstance, ActiveEffects::kHiddenDepth> chain{};
        usize                                                   depth = 0;
        while (depth < ActiveEffects::kHiddenDepth) {
            const EffectInstance* instance = effects.hidden(effect, depth);
            if (instance == nullptr) {
                break;
            }
            chain[depth++] = *instance;
        }
        list.push(save_one(chain, depth, 0));
    }
    return list;
}

usize load_effects(ActiveEffects& effects, const nbt::Tag& list, EffectTarget& target) {
    const std::vector<nbt::Tag>* items = list.list();
    if (items == nullptr) {
        return 0;
    }
    usize loaded = 0;
    for (const nbt::Tag& compound : *items) {
        // The top instance first, then each HiddenEffect beneath it. Adding
        // them in that order through `add` rebuilds the same chain: each
        // hidden one is weaker and longer than the one above it, which is
        // exactly the case `add` puts into hiding.
        const nbt::Tag* at = &compound;
        bool            first = true;
        while (at != nullptr) {
            const auto instance = load_one(*at);
            if (!instance) {
                break;
            }
            (void)effects.add(*instance, target);
            if (first) {
                ++loaded;
                first = false;
            }
            at = at->find("HiddenEffect");
        }
    }
    return loaded;
}

}  // namespace ov::gameplay
