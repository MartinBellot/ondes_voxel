// Entity selectors: `@a`, `@e[type=!cow,distance=..5,limit=1,sort=nearest]`,
// a player's name, a UUID.
//
// Parsing and choosing are two functions, both pure: the parser reads the
// text and leaves the cursor exactly where vanilla's does when it refuses
// (`@r[type` stops *before* the `=`, because `type` is inapplicable to a
// selector already limited to players, while `@e[foo` goes back to `foo`,
// because `foo` is unknown); the chooser takes a snapshot of the world and a
// source and returns entities, in vanilla's order and up to its limit.
//
// Options this server cannot evaluate — `nbt`, `scores`, `advancements`,
// `predicate` — are parsed in full so the cursor stays honest, and then
// **refused by name** when the selector is used. Matching nothing instead
// would look exactly like a correct answer.
#pragma once

#include "context.hpp"
#include "env.hpp"
#include "string_reader.hpp"
#include "suggestions.hpp"

#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::server::cmd {

struct DoubleBounds {
    std::optional<f64> min;
    std::optional<f64> max;

    [[nodiscard]] bool contains_squared(f64 squared) const noexcept;
};

struct IntBounds {
    std::optional<i32> min;
    std::optional<i32> max;

    [[nodiscard]] bool contains(i32 value) const noexcept;
};

struct AngleBounds {
    std::optional<f32> min;
    std::optional<f32> max;

    [[nodiscard]] bool contains(f32 degrees) const noexcept;
};

struct EntitySelector {
    enum class Order : u8 { Arbitrary, Nearest, Furthest, Random };

    struct Match {
        std::string value;
        bool        negated{false};
        bool        tag{false};  // `type=#minecraft:skeletons`
    };

    i32  max_results{1};
    bool includes_entities{false};
    bool current_entity{false};
    bool uses_selector{false};
    Order order{Order::Arbitrary};

    std::optional<std::string> player_name;
    std::optional<net::Uuid>   uuid;

    std::optional<f64> x, y, z, dx, dy, dz;
    std::optional<DoubleBounds> distance;
    std::optional<IntBounds>    level;
    std::optional<AngleBounds>  x_rotation;
    std::optional<AngleBounds>  y_rotation;

    std::vector<Match> names;
    std::vector<Match> types;
    std::vector<Match> tags;
    std::vector<Match> teams;
    std::vector<Match> game_modes;

    /// The options that parsed and that this server does not evaluate.
    std::vector<std::string> unsupported;

    // Parse state, for "this option is inapplicable here".
    bool type_limited{false};
    bool type_limited_inversely{false};
    bool limited{false};
    bool sorted{false};
    bool has_name_equals{false};
    bool has_name_not_equals{false};
    bool has_game_mode_equals{false};
    bool has_game_mode_not_equals{false};
    bool has_team_equals{false};
    bool has_team_not_equals{false};

    [[nodiscard]] bool is_self() const noexcept { return current_entity; }
};

[[nodiscard]] Parsed<EntitySelector> parse_entity_selector(StringReader& reader,
                                                           const ParseEnv& env);

/// Choose. `random_below(n)` returns a value in [0, n) and is only called for
/// `@r` and `sort=random`.
[[nodiscard]] Parsed<std::vector<const EntityInfo*>> find_entities(
    const EntitySelector& selector, const CommandSource& source, std::span<const EntityInfo> world,
    const std::function<u32(u32)>& random_below, const ParseEnv* env = nullptr);

/// Completions for an entity argument at `builder`'s start.
void suggest_entity_selector(SuggestionsBuilder& builder, std::span<const std::string> player_names,
                             const ParseEnv& env);

/// Java's `UUID.fromString`: five hex groups, lengths not enforced.
[[nodiscard]] std::optional<net::Uuid> parse_java_uuid(std::string_view text) noexcept;

}  // namespace ov::server::cmd
