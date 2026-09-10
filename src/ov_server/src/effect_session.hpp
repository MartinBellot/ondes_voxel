// One player's status effects and attributes, and the packets they owe.
//
// The same shape as survival_session.{hpp,cpp} and combat_session.{hpp,cpp}:
// the rules live in ov_gameplay (effects.hpp, attributes.hpp), and what is here
// is the wiring — the state that has to live between two packets, and the
// packets themselves. server.cpp touches it in short delimited blocks marked
// `// ── effects ──`.
//
// ── The API, for whoever writes /effect ─────────────────────────────────────
//
//   gameplay::EffectInstance give{.effect = gameplay::Effect::Speed,
//                                 .duration = 30 * 20, .amplifier = 1};
//   who.effects.apply(give, who.survival, io, who.entity_id, mortal, flags);
//   who.effects.remove(gameplay::Effect::Speed, …);
//   who.effects.clear(…);
//
// `effect give … <seconds> <amplifier> <hideParticles>` maps onto this as:
// duration = seconds * 20 (or `gameplay::kInfiniteDuration` for `infinite`),
// `visible = show_icon = !hideParticles` — measured, hiding the particles from
// the command hides the icon too — and `ambient = false`. `apply` answers the
// same question the command's reply does: `AddResult::Immune` and
// `AddResult::Unchanged` are the two "Unable to apply" cases.
//
// The packets are sent by the call itself: Entity Effect / Remove Entity Effect
// to the player alone (vanilla tells no one else), Update Attributes and the
// metadata to the player and everyone watching.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/attributes.hpp"
#include "ov/gameplay/breaking.hpp"
#include "ov/gameplay/effects.hpp"
#include "ov/math/random.hpp"
#include "survival_session.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace ov::server {

/// The two sinks every effect packet goes to.
struct EffectIo {
    /// To this player's own client.
    std::function<void(i32 packet_id, std::span<const u8> payload)> send;
    /// To every other client.
    std::function<void(i32 packet_id, std::span<const u8> payload)> broadcast;
};

/// Who the effects are on, and what the rest of their shared-flags byte holds.
struct EffectBearer {
    i32 entity_id{0};
    /// False for creative and spectator: an effect still runs and still shows,
    /// but nothing it does can hurt or starve them.
    bool mortal{true};
    /// Index 0's bits the effects do not own — sneaking (0x02), sprinting
    /// (0x08) — so that setting invisibility does not erase them.
    u8 shared_flags{0};
};

/// Parse one `--effect=` item: `name[:amplifier[:ticks]]`, where the name may
/// omit `minecraft:` and ticks may be `infinite`. Defaults: amplifier 0, 600
/// ticks. Nullopt, never a default effect, for a name the registry does not
/// have or a number that does not parse.
[[nodiscard]] std::optional<gameplay::EffectInstance> parse_effect_spec(std::string_view spec);

class EffectSession {
public:
    gameplay::ActiveEffects effects{};
    /// The player's attributes, at the bases measured on a real server.
    gameplay::AttributeMap attributes = gameplay::AttributeMap::player();

    /// Give an effect. See the header comment for the command mapping.
    gameplay::AddResult apply(const gameplay::EffectInstance& instance, SurvivalSession& survival,
                              const EffectIo& io, const EffectBearer& bearer);

    /// Remove one effect. False when it was not there.
    bool remove(gameplay::Effect effect, SurvivalSession& survival, const EffectIo& io,
                const EffectBearer& bearer);

    /// Remove all of them. Returns how many.
    usize clear(SurvivalSession& survival, const EffectIo& io, const EffectBearer& bearer);

    /// One tick. Runs every effect, then sends what changed.
    void tick(SurvivalSession& survival, const EffectIo& io, const EffectBearer& bearer);

    /// An item's use just finished: a food's effects, honey, milk. True when
    /// the item was one of these.
    bool on_consumed(std::string_view item, SurvivalSession& survival, const EffectIo& io,
                     const EffectBearer& bearer);

    /// The player died. Everything goes, silently: the client learns it from
    /// Respawn, which resets its own effects and attributes — measured, no
    /// Remove Entity Effect arrived at a vanilla death.
    void on_death();

    // ── What the other systems read ─────────────────────────────────────────

    /// Haste and mining fatigue for the break timer. Conduit power digs like
    /// haste, and the larger of the two counts — measured.
    [[nodiscard]] gameplay::Stance dig_stance(bool on_ground) const noexcept;

    /// Strength and weakness amplifiers for the melee rules, -1 for none.
    [[nodiscard]] i8 strength() const noexcept;
    [[nodiscard]] i8 weakness() const noexcept;

    /// Blindness disqualifies a critical hit.
    [[nodiscard]] bool blind() const noexcept;

    /// Water breathing or conduit power: the air neither drains nor refills —
    /// measured, two cows under water kept 299 of 300 for five seconds while
    /// a third lost a hundred and seven.
    [[nodiscard]] bool breathes_underwater() const noexcept;

    /// Jump boost's amplifier, -1 for none. A fall costs `amp + 1` less.
    [[nodiscard]] i32 jump_boost() const noexcept;

    /// Slow falling: no fall damage at all, measured from 6, 10 and 20 blocks.
    [[nodiscard]] bool slow_falling() const noexcept;

private:
    void sync(SurvivalSession& survival) noexcept;
    void flush(SurvivalSession& survival, const EffectIo& io, const EffectBearer& bearer);

    /// Food effects' chances. Explicit and per-player, CLAUDE.md principle 5.
    math::XoroshiroRandomSource random_{0x0E77'EC75'0000'0001LL};

    /// What the client was last told, so a packet goes out only on change.
    u32  sent_color_{0};
    bool sent_ambient_{false};
    f32  sent_absorption_{0.0F};
    u8   sent_flags_{0};
    bool sent_valid_{false};
};

}  // namespace ov::server
