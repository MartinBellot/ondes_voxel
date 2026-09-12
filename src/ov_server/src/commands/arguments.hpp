// Argument types: how each piece of a command is read, completed, and
// declared to the client.
//
// Every type carries the parser id of Mojang's `minecraft:command_argument_type`
// registry — `brigadier:integer` is 3, `minecraft:block_state` 12, and the
// client reads the properties that follow by that number, so the numbers are
// resolved from the registry pack and never written here.
//
// A parse failure leaves the reader's cursor exactly where vanilla's does; the
// capture pins a dozen of them (`give @s diamond 0` points at the 0,
// `setblock 1.5 …` at the 1.5, `gamemode foo` after the foo).
#pragma once

#include "context.hpp"
#include "env.hpp"
#include "json.hpp"
#include "selector.hpp"
#include "string_reader.hpp"
#include "suggestions.hpp"
#include "text.hpp"

#include "ov/nbt/tag.hpp"
#include "ov/protocol/play.hpp"

#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace ov::server::cmd {

struct BlockStateArg {
    registry::BlockId          block{};
    registry::BlockStateId     state{};
    std::string                id;
    std::optional<nbt::Tag>    nbt;
};

struct BlockPredicateArg {
    bool                                             tag{false};
    std::string                                      id;
    std::optional<registry::BlockId>                 block;
    std::vector<std::pair<std::string, std::string>> properties;
    std::optional<nbt::Tag>                          nbt;

    [[nodiscard]] bool test(registry::BlockStateId state, const ParseEnv& env) const;
};

struct ItemArg {
    i32                     item{0};
    std::string             id;
    std::optional<nbt::Tag> nbt;
};

struct ItemPredicateArg {
    bool                    tag{false};
    std::string             id;
    i32                     item{-1};
    std::optional<nbt::Tag> nbt;

    [[nodiscard]] bool test(const net::ItemStack& stack, const ParseEnv& env) const;
};

struct GameProfileArg {
    std::optional<EntitySelector> selector;
    std::string                   name;
};

struct MessageArg {
    struct Part {
        usize          start{0};
        usize          end{0};
        EntitySelector selector;
    };
    std::string       text;
    std::vector<Part> selectors;
};

struct GameModeArg {
    u8 mode{0};
};
struct AnchorArg {
    bool eyes{false};
};
struct ResourceArg {
    std::string id;
};
struct TimeArg {
    i32 ticks{0};
};

using ArgValue =
    std::variant<bool, i32, i64, f32, f64, std::string, EntitySelector, GameProfileArg, Coordinates,
                 RotationArg, AngleArg, BlockStateArg, BlockPredicateArg, ItemArg, ItemPredicateArg,
                 Text, nbt::Tag, GameModeArg, AnchorArg, ResourceArg, MessageArg, TimeArg>;

enum class ArgKind : u8 {
    Bool,
    Integer,
    Word,
    Greedy,
    Entity,
    GameProfile,
    BlockPos,
    Vec3,
    Rotation,
    Angle,
    BlockState,
    BlockPredicate,
    ItemStack,
    ItemPredicate,
    Component,
    Message,
    NbtCompound,
    EntityAnchor,
    GameMode,
    Time,
    Resource,
    // ── scoreboard ── Each reads to a string (the text as written), except
    // ScoreHolder, which reads to a GameProfileArg: a selector, or a name —
    // "*" meaning every holder the scoreboard tracks.
    Objective,
    ObjectiveCriteria,
    Operation,
    ScoreboardSlot,
    ScoreHolder,
    Team,
    Color,
};

struct ArgumentType {
    ArgKind     kind{ArgKind::Word};
    i32         min{std::numeric_limits<i32>::min()};
    i32         max{std::numeric_limits<i32>::max()};
    bool        has_min{false};
    bool        has_max{false};
    bool        single{false};
    bool        players_only{false};
    std::string registry;

    [[nodiscard]] static ArgumentType of(ArgKind kind) {
        ArgumentType t;
        t.kind = kind;
        return t;
    }
    [[nodiscard]] static ArgumentType boolean() { return of(ArgKind::Bool); }
    [[nodiscard]] static ArgumentType integer() { return of(ArgKind::Integer); }
    [[nodiscard]] static ArgumentType integer_at_least(i32 min) {
        ArgumentType t = of(ArgKind::Integer);
        t.min     = min;
        t.has_min = true;
        return t;
    }
    [[nodiscard]] static ArgumentType integer_between(i32 min, i32 max) {
        ArgumentType t = integer_at_least(min);
        t.max     = max;
        t.has_max = true;
        return t;
    }
    [[nodiscard]] static ArgumentType word() { return of(ArgKind::Word); }
    [[nodiscard]] static ArgumentType greedy() { return of(ArgKind::Greedy); }
    [[nodiscard]] static ArgumentType entity(bool single, bool players_only) {
        ArgumentType t = of(ArgKind::Entity);
        t.single       = single;
        t.players_only = players_only;
        return t;
    }
    [[nodiscard]] static ArgumentType game_profile() { return of(ArgKind::GameProfile); }
    [[nodiscard]] static ArgumentType block_pos() { return of(ArgKind::BlockPos); }
    [[nodiscard]] static ArgumentType vec3() { return of(ArgKind::Vec3); }
    [[nodiscard]] static ArgumentType rotation() { return of(ArgKind::Rotation); }
    [[nodiscard]] static ArgumentType angle() { return of(ArgKind::Angle); }
    [[nodiscard]] static ArgumentType block_state() { return of(ArgKind::BlockState); }
    [[nodiscard]] static ArgumentType block_predicate() { return of(ArgKind::BlockPredicate); }
    [[nodiscard]] static ArgumentType item_stack() { return of(ArgKind::ItemStack); }
    [[nodiscard]] static ArgumentType item_predicate() { return of(ArgKind::ItemPredicate); }
    [[nodiscard]] static ArgumentType component() { return of(ArgKind::Component); }
    [[nodiscard]] static ArgumentType message() { return of(ArgKind::Message); }
    [[nodiscard]] static ArgumentType nbt_compound() { return of(ArgKind::NbtCompound); }
    [[nodiscard]] static ArgumentType entity_anchor() { return of(ArgKind::EntityAnchor); }
    [[nodiscard]] static ArgumentType game_mode() { return of(ArgKind::GameMode); }
    [[nodiscard]] static ArgumentType time(i32 min) {
        ArgumentType t = of(ArgKind::Time);
        t.min     = min;
        t.has_min = true;
        return t;
    }
    [[nodiscard]] static ArgumentType resource(std::string registry) {
        ArgumentType t = of(ArgKind::Resource);
        t.registry = std::move(registry);
        return t;
    }
    // ── scoreboard ──
    [[nodiscard]] static ArgumentType objective() { return of(ArgKind::Objective); }
    [[nodiscard]] static ArgumentType objective_criteria() { return of(ArgKind::ObjectiveCriteria); }
    [[nodiscard]] static ArgumentType operation() { return of(ArgKind::Operation); }
    [[nodiscard]] static ArgumentType scoreboard_slot() { return of(ArgKind::ScoreboardSlot); }
    [[nodiscard]] static ArgumentType score_holder(bool multiple) {
        ArgumentType t = of(ArgKind::ScoreHolder);
        t.single       = !multiple;
        return t;
    }
    [[nodiscard]] static ArgumentType team() { return of(ArgKind::Team); }
    [[nodiscard]] static ArgumentType color() { return of(ArgKind::Color); }

    /// The registry name of the parser, e.g. "brigadier:integer".
    [[nodiscard]] std::string_view parser_name() const noexcept;

    /// The properties as the Commands packet carries them after the parser id.
    [[nodiscard]] std::vector<u8> wire_properties() const;
};

[[nodiscard]] Parsed<ArgValue> parse_argument(const ArgumentType& type, StringReader& reader,
                                              const ParseEnv& env);

/// Offer completions for `type` at the builder's start.
void suggest_argument(const ArgumentType& type, SuggestionsBuilder& builder, const ParseEnv& env,
                      std::span<const std::string> player_names);

/// Java's `Math.round(float)`.
[[nodiscard]] i32 java_round(f32 value) noexcept;

}  // namespace ov::server::cmd
