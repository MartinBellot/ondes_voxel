// ── mobs-4 ── A mob's status effects.
//
// The rules have been in ov_gameplay since the effects wave (effects.hpp), and
// a mob implemented `EffectTarget` in its tests; what the server lacked was a
// place to keep a mob's effects between two ticks. This is it: a table beside
// the entity world, keyed by wire id like MobCombat's windows, one row per mob
// carrying an effect. A mob that has none costs nothing here, and a row goes
// when its last effect does.
//
// What a row does for its mob:
//   * the effects tick, through a target whose damage goes through the mob's
//     own window (MobCombat), Resistance first, and whose body — undead,
//     arthropod — is its species' (mob_body.hpp);
//   * the attributes: `max_health` (Health Boost) is written back onto the
//     entity, and `movement_speed` (Speed, Slowness) becomes the brain's
//     `effect_walk`, the square of the ratio (the walk law is quadratic);
//   * the metadata every watcher is sent — the swirl colour (10), "all
//     visible effects are ambient" (11), invisibility and glowing (0), and the
//     health (9) when an effect moved it — measured on a cow (effets.md § 10);
//   * `ActiveEffects` in the mob's Anvil record, in the same format as the
//     player file (effets.md § 13).
//
// What is not sent: Entity Effect / Remove Entity Effect and Update Attributes
// for a mob. Whether vanilla tells watchers about a mob's effects at all is
// what `measure_hostile.py packets` measures; until it has, nothing is sent
// rather than a guess (docs/provenance/mobs-4.md).
//
// Deaths are not this module's: a hit that kills is queued in `hurts()` and
// the server plays the death, draws the loot and removes the body, as it does
// for `/kill`.
#pragma once

#include "mob_combat.hpp"

#include "ov/base/types.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/attributes.hpp"
#include "ov/gameplay/damage.hpp"
#include "ov/gameplay/effects.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/registries.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace ov::server {

/// Where a mob's effect packets go: every client.
using MobEffectSink = std::function<void(i32 packet_id, std::span<const u8> payload)>;

/// A rule run against one mob's effects: a splash, a cloud, a tipped arrow.
using MobEffectRule = std::function<void(gameplay::ActiveEffects&, gameplay::EffectTarget&)>;

/// One hit an effect landed on a mob. The server owes the watchers a Damage
/// Event and a hurt sound — or, when `killed`, the whole death.
struct MobEffectHurt {
    i32                  id{0};
    gameplay::DamageKind kind{gameplay::DamageKind::Magic};
    bool                 killed{false};
};

class MobEffects {
public:
    /// `combat` may be null: then effects heal but never hurt.
    MobEffects(const registry::Registries& registries, MobCombat* combat);
    ~MobEffects();

    MobEffects(const MobEffects&)            = delete;
    MobEffects& operator=(const MobEffects&) = delete;

    /// Which entities are mobs that can bear an effect. Default: anything
    /// with a `gameplay::Mob` brain. The server widens it to every living
    /// thing in its entity world.
    void set_living(std::function<bool(entity::EntityWorld&, entity::EntityHandle)> living);

    /// The bits of index 0 the effects do not own (on fire), so that setting
    /// invisibility does not put a burning mob out on the client.
    void set_other_flags(std::function<u8(const entity::EntityState&)> flags);

    // ── `/effect` on a mob. Nullopt: `id` is no living mob of `world`. ──────

    [[nodiscard]] std::optional<gameplay::AddResult> apply(entity::EntityWorld& world, i32 id,
                                                           const gameplay::EffectInstance& instance,
                                                           const MobEffectSink& broadcast);
    [[nodiscard]] std::optional<bool>  remove(entity::EntityWorld& world, i32 id,
                                              gameplay::Effect effect, const MobEffectSink& broadcast);
    [[nodiscard]] std::optional<usize> clear(entity::EntityWorld& world, i32 id,
                                             const MobEffectSink& broadcast);

    /// Run a rule against a mob's effects. False: no living mob by that id.
    bool with_target(entity::EntityWorld& world, i32 id, const MobEffectRule& rule,
                     const MobEffectSink& broadcast);

    /// One tick of every mob carrying effects: they act, count down and end.
    /// Before the entity tick, so the walk of this tick is the walk the
    /// effects left, as a living entity ticks its effects before it travels.
    void tick(entity::EntityWorld& world, const MobEffectSink& broadcast);

    /// The hits since `clear_hurts`, in the order they landed.
    [[nodiscard]] std::span<const MobEffectHurt> hurts() const noexcept { return hurts_; }
    void                                         clear_hurts() noexcept { hurts_.clear(); }

    /// What a client that starts watching this mob needs besides its spawn:
    /// the metadata its effects set. Nothing for a mob without effects.
    void pairing(const entity::EntityState& state, const MobEffectSink& send) const;

    /// The mob's effects, or null when it carries none.
    [[nodiscard]] const gameplay::ActiveEffects* effects_of(i32 id) const noexcept;

    /// A hit after the mob's Resistance, for the damage paths that are not an
    /// effect's (a sword, an arrow). Unchanged for a mob without effects.
    [[nodiscard]] f32 after_resistance(i32 id, gameplay::DamageKind kind, f32 amount) const noexcept;

    /// The mob's `attack_damage` with its effects' modifiers — Strength,
    /// Weakness. Nullopt for a mob without effects or without the attribute:
    /// the caller's base stands.
    [[nodiscard]] std::optional<f64> attack_damage(i32 id) const noexcept;

    // ── The Anvil record ────────────────────────────────────────────────────

    /// Write `ActiveEffects`, or take away a stale one a compound read from
    /// disk still carries.
    void write(const entity::EntityState& state, nbt::Tag& out) const;
    /// Read it back: the effects, their modifiers, and the health the file
    /// holds within the maximum those modifiers give.
    void read(entity::EntityWorld& world, entity::EntityState& state, const nbt::Tag& compound);

    /// The mob left the world.
    void forget(i32 id);

    [[nodiscard]] usize carriers() const noexcept { return rows_.size(); }

private:
    /// One mob's effects. Defined here, not in the source: a `std::map` of an
    /// incomplete type is not guaranteed by the standard.
    struct Row {
        gameplay::ActiveEffects      effects{};
        gameplay::AttributeMap       attributes{};
        gameplay::EffectTarget::Body body{gameplay::EffectTarget::Body::Ordinary};
        /// The species' `movement_speed`, which the walk factor is a ratio to.
        f64 base_speed{0.0};
        /// An effect took the last point; the server has the death to play.
        bool died{false};
        /// What the watchers were last told.
        u32  sent_colour{0};
        bool sent_ambient{false};
        u8   sent_flags{0};
        f32  sent_health{0.0F};
    };
    class Target;

    struct Located {
        entity::EntityHandle handle{entity::kNoEntity};
        entity::EntityState* state{nullptr};
    };
    [[nodiscard]] std::optional<Located> locate(entity::EntityWorld& world, i32 id) const;
    Row& row_of(entity::EntityWorld& world, entity::EntityHandle handle, entity::EntityState& state);
    /// After any change: the attributes onto the entity and the brain, the
    /// metadata to the watchers, and the row gone if it is empty.
    void finish(entity::EntityWorld& world, const Located& at, const MobEffectSink& broadcast);
    void settle(entity::EntityWorld& world, entity::EntityHandle handle, entity::EntityState& state,
                Row& row) const;
    void flush(const entity::EntityState& state, Row& row, const MobEffectSink& broadcast) const;

    const registry::Registries*         registries_{nullptr};
    MobCombat*                          combat_{nullptr};
    std::optional<registry::RegistryId> entity_registry_;
    i32                                 movement_speed_id_{-1};
    i32                                 attack_damage_id_{-1};

    std::function<bool(entity::EntityWorld&, entity::EntityHandle)> living_;
    std::function<u8(const entity::EntityState&)>                   other_flags_;

    /// Ordered by wire id: the order the rows tick in is the order their hits
    /// are queued, and the deaths draw loot from one shared generator in that
    /// order. A hash map would make it depend on the standard library.
    std::map<i32, Row>         rows_;
    std::vector<MobEffectHurt> hurts_;
};

}  // namespace ov::server
