// ── tame ── What a wolf, a cat, a horse or a llama carries that a cow does
// not: an owner, an order to sit, a collar, a temper, a saddle, the stats a
// foal inherits.
//
// Plain structs in a header of their own, for the reason animal.hpp and
// villager_state.hpp are: `MobBrain` (goals.hpp) holds a TameState, and the
// rules that act on it (tame.hpp) need the goals — a header each way would be a
// cycle. Every number that gives these fields a meaning is measured and lives
// in tame.hpp with its provenance; see docs/provenance/apprivoisement.md.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/entity.hpp"
#include "ov/math/vec.hpp"
#include "ov/protocol/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::gameplay {

/// Which rules a mob's TameState follows. `None` on every mob that is not one
/// of the species in tame.cpp's table.
enum class TameFamily : u8 {
    None,
    Wolf,
    Cat,
    Ocelot,
    Parrot,
    /// Horse, donkey, mule: ridden to be tamed, saddled to be steered.
    Horse,
    /// Llama and trader llama: ridden to be tamed, never steered.
    Llama,
    Rabbit,
    Fox,
    Turtle,
    Bee,
    Goat,
    Camel,
};

/// A horse's three drawn attributes, as its `Attributes` list stores them.
/// Zero: not drawn — the registry's base value stands.
struct HorseStats {
    f64 max_health{0.0};
    f64 speed{0.0};
    f64 jump{0.0};

    [[nodiscard]] constexpr bool drawn() const noexcept { return max_health > 0.0; }
};

struct TameState {
    TameFamily family{TameFamily::None};

    bool tame{false};
    /// `Owner`, as the four ints of a UUID. A tame animal read from a world
    /// may have an owner who is not online; it is still theirs.
    std::optional<net::Uuid> owner;
    /// Ordered to sit (`Sitting`). A sitting animal does not move, follow or
    /// fight until it is hurt or told to stand.
    bool sitting{false};
    /// `CollarColor`, a dye index. Red (14) is the colour a collar starts at.
    i8 collar{14};

    /// `AngerTime`, in ticks, and the player it is for (`AngryAt`).
    i32                      anger{0};
    std::optional<net::Uuid> angry_at;

    /// `Variant` (horse: colour | markings << 8; llama 0..3; parrot 0..4),
    /// `RabbitType`, a fox's `Type` (0 red, 1 snow), a cat's `variant` as its
    /// index in `minecraft:cat_variant`.
    i32 variant{0};

    // ── the horse family ──
    /// `Temper`: raised by 5 each time the animal throws its rider, and by
    /// food; the animal is tamed when a draw falls under it.
    i32  temper{0};
    bool bred{false};
    bool chested{false};
    bool saddled{false};
    /// `ArmorItem` (a horse) or `DecorItem` (a llama's carpet): a registry
    /// name, empty for none.
    std::string armour;
    /// A llama's `Strength`, 1..5: three slots of chest per point.
    i32        strength{0};
    HorseStats stats{};
    /// The player riding this animal, by wire id; 0 for none.
    i32 rider{0};
    /// Ticks the rider has been on an untamed animal, for the tantrum.
    i32 ridden_for{0};

    // ── the others ──
    /// Ocelot `Trusting`.
    bool trusting{false};
    /// Fox `Sleeping`, turtle `HasEgg`, bee `HasNectar`, goat
    /// `IsScreamingGoat`: one flag each family reads as its own.
    bool flag{false};
    /// Goat horns (`HasLeftHorn`, `HasRightHorn`).
    bool left_horn{true};
    bool right_horn{true};

    /// Something a client shows changed: the caller resends the metadata.
    bool dirty{false};

    [[nodiscard]] bool active() const noexcept { return family != TameFamily::None; }
    [[nodiscard]] bool owned_by(const net::Uuid& uuid) const noexcept {
        return owner.has_value() && *owner == uuid;
    }
};

/// A player as a tame animal sees one: who, where, and what they last hit and
/// were last hit by — which is what a wolf defends and attacks for.
struct Owner {
    i32       network_id{0};
    net::Uuid uuid{};
    Vec3d     feet{};
    bool      on_ground{true};
    /// The last mob that hurt this player, and the tick it did; kNoEntity
    /// for none.
    entity::EntityHandle hurt_by{entity::kNoEntity};
    i64                  hurt_by_tick{-1};
    /// The last mob this player hurt, and the tick.
    entity::EntityHandle attacked{entity::kNoEntity};
    i64                  attacked_tick{-1};
};

/// Something a tame animal did this tick that only the caller can finish.
enum class TameEventKind : u8 {
    /// A ridden animal decided: tamed by its rider (`player`).
    Tamed,
    /// A ridden animal threw its rider (`player`) off, angry.
    Threw,
    /// A tame animal too far from its owner was put back beside them.
    Teleported,
};

struct TameEvent {
    TameEventKind        kind{TameEventKind::Tamed};
    entity::EntityHandle self{entity::kNoEntity};
    i32                  player{0};
    Vec3d                at{};
};

/// What a tame animal's goals are handed each tick, by the caller.
struct TameWorld {
    /// Every player an animal may belong to, online, in this dimension.
    std::span<const Owner> owners{};
    /// Where decisions go for the caller to finish. Null: nobody hears.
    std::vector<TameEvent>* events{nullptr};
    /// The protocol ids a goal needs by type: a creeper keeps away from cats
    /// and ocelots; a wild wolf hunts sheep, rabbits and foxes.
    i32 cat_type{-1};
    i32 ocelot_type{-1};
    i32 creeper_type{-1};
    i32 sheep_type{-1};
    i32 rabbit_type{-1};
    i32 fox_type{-1};
    i32 skeleton_type{-1};
    i32 llama_type{-1};
    i32 trader_llama_type{-1};
    i32 wolf_type{-1};
};

[[nodiscard]] const Owner* find_owner(std::span<const Owner> owners, const net::Uuid& uuid) noexcept;
[[nodiscard]] const Owner* find_owner(std::span<const Owner> owners, i32 network_id) noexcept;

}  // namespace ov::gameplay
