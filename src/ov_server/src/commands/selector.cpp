#include "selector.hpp"

#include "snbt.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>

namespace ov::server::cmd {

bool DoubleBounds::contains_squared(f64 squared) const noexcept {
    if (min && squared < *min * *min) {
        return false;
    }
    return !(max && squared > *max * *max);
}

bool IntBounds::contains(i32 value) const noexcept {
    if (min && value < *min) {
        return false;
    }
    return !(max && value > *max);
}

bool AngleBounds::contains(f32 degrees) const noexcept {
    const f32 value = wrap_degrees(degrees);
    const f32 low   = min ? wrap_degrees(*min) : -180.0F;
    const f32 high  = max ? wrap_degrees(*max) : 180.0F;
    // A range that crosses ±180 is the two ends, not the middle.
    return low > high ? (value >= low || value <= high) : (value >= low && value <= high);
}

std::optional<net::Uuid> parse_java_uuid(std::string_view text) noexcept {
    std::array<u64, 5> parts{};
    usize              part  = 0;
    usize              start = 0;
    for (usize i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '-') {
            if (part >= 5 || i == start || i - start > 16) {
                return std::nullopt;
            }
            u64 value = 0;
            for (usize j = start; j < i; ++j) {
                const char c = text[j];
                u64        digit = 0;
                if (c >= '0' && c <= '9') {
                    digit = static_cast<u64>(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    digit = static_cast<u64>(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    digit = static_cast<u64>(c - 'A' + 10);
                } else {
                    return std::nullopt;
                }
                value = (value << 4U) | digit;
            }
            parts[part++] = value;
            start         = i + 1;
        }
    }
    if (part != 5) {
        return std::nullopt;
    }
    const u64 most  = ((parts[0] & 0xFFFFFFFFULL) << 32U) | ((parts[1] & 0xFFFFULL) << 16U) |
                     (parts[2] & 0xFFFFULL);
    const u64 least = ((parts[3] & 0xFFFFULL) << 48U) | (parts[4] & 0xFFFFFFFFFFFFULL);
    return net::Uuid{most, least};
}

namespace {

constexpr std::array<std::string_view, 21> kOptionNames{
    "name", "distance", "level", "x", "y", "z", "dx", "dy", "dz", "x_rotation", "y_rotation",
    "limit", "sort", "gamemode", "team", "type", "tag", "nbt", "scores", "advancements",
    "predicate"};

[[nodiscard]] std::optional<u8> game_mode_by_name(std::string_view name) {
    if (name == "survival") {
        return 0;
    }
    if (name == "creative") {
        return 1;
    }
    if (name == "adventure") {
        return 2;
    }
    if (name == "spectator") {
        return 3;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_allowed_in_resource_location(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c == '/' ||
           c == '.' || c == '-';
}

/// ResourceLocation.read: the characters an id may hold, then validated.
Parsed<std::string> read_resource_location(StringReader& reader) {
    const usize start = reader.cursor();
    while (reader.can_read() && is_allowed_in_resource_location(reader.peek())) {
        reader.skip();
    }
    const std::string_view text  = reader.string().substr(start, reader.cursor() - start);
    const auto             colon = text.find(':');
    const std::string_view name_space = colon == std::string_view::npos ? "minecraft"
                                                                        : text.substr(0, colon);
    const std::string_view path = colon == std::string_view::npos ? text : text.substr(colon + 1);
    const bool             namespace_ok = std::ranges::all_of(name_space, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == '-';
    });
    const bool path_ok = std::ranges::all_of(path, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == '.' ||
               c == '-' || c == '/';
    });
    if (!namespace_ok || !path_ok) {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.id.invalid")};
    }
    return std::string{name_space} + ":" + std::string{path};
}

bool should_invert(StringReader& reader) {
    reader.skip_whitespace();
    if (reader.can_read() && reader.peek() == '!') {
        reader.skip();
        reader.skip_whitespace();
        return true;
    }
    return false;
}

bool is_tag(StringReader& reader) {
    reader.skip_whitespace();
    if (reader.can_read() && reader.peek() == '#') {
        reader.skip();
        return true;
    }
    return false;
}

/// MinMaxBounds' number alphabet: digits, '-', and a '.' that does not start
/// the `..` separator.
bool range_char(const StringReader& reader) {
    const char c = reader.peek();
    if ((c < '0' || c > '9') && c != '-') {
        return c == '.' && (!reader.can_read(2) || reader.peek(1) != '.');
    }
    return true;
}

template<typename T, typename ParseFn>
Parsed<std::optional<T>> range_number(StringReader& reader, ParseFn parse,
                                      std::string_view invalid_key) {
    const usize start = reader.cursor();
    while (reader.can_read() && range_char(reader)) {
        reader.skip();
    }
    const std::string_view text = reader.string().substr(start, reader.cursor() - start);
    if (text.empty()) {
        return std::optional<T>{};
    }
    const auto value = parse(text);
    if (!value) {
        reader.set_cursor(start);
        return std::unexpected{
            reader.error(std::string{invalid_key}, {Text::raw(std::string{text})})};
    }
    return std::optional<T>{static_cast<T>(*value)};
}

Parsed<DoubleBounds> read_double_bounds(StringReader& reader) {
    if (!reader.can_read()) {
        return std::unexpected{reader.error("argument.range.empty")};
    }
    const usize start = reader.cursor();
    const auto  parse = [](std::string_view t) { return parse_java_double(t); };
    auto        low   = range_number<f64>(reader, parse, "parsing.double.invalid");
    if (!low) {
        return std::unexpected{low.error()};
    }
    DoubleBounds out;
    out.min = *low;
    if (reader.can_read(2) && reader.peek() == '.' && reader.peek(1) == '.') {
        reader.skip();
        reader.skip();
        auto high = range_number<f64>(reader, parse, "parsing.double.invalid");
        if (!high) {
            return std::unexpected{high.error()};
        }
        out.max = *high;
    } else {
        out.max = out.min;
    }
    if (!out.min && !out.max) {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.range.empty")};
    }
    if (out.min && out.max && *out.min > *out.max) {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.range.swapped")};
    }
    return out;
}

Parsed<IntBounds> read_int_bounds(StringReader& reader) {
    if (!reader.can_read()) {
        return std::unexpected{reader.error("argument.range.empty")};
    }
    const usize start = reader.cursor();
    const auto  parse = [](std::string_view t) {
        return parse_java_long(t, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max());
    };
    auto low = range_number<i32>(reader, parse, "parsing.int.invalid");
    if (!low) {
        return std::unexpected{low.error()};
    }
    IntBounds out;
    out.min = *low;
    if (reader.can_read(2) && reader.peek() == '.' && reader.peek(1) == '.') {
        reader.skip();
        reader.skip();
        auto high = range_number<i32>(reader, parse, "parsing.int.invalid");
        if (!high) {
            return std::unexpected{high.error()};
        }
        out.max = *high;
    } else {
        out.max = out.min;
    }
    if (!out.min && !out.max) {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.range.empty")};
    }
    if (out.min && out.max && *out.min > *out.max) {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.range.swapped")};
    }
    return out;
}

Parsed<AngleBounds> read_angle_bounds(StringReader& reader) {
    const auto doubles = read_double_bounds(reader);
    if (!doubles) {
        return std::unexpected{doubles.error()};
    }
    AngleBounds out;
    if (doubles->min) {
        out.min = static_cast<f32>(*doubles->min);
    }
    if (doubles->max) {
        out.max = static_cast<f32>(*doubles->max);
    }
    return out;
}

/// Skip a balanced `{…}` for the options this server parses but cannot
/// evaluate, so the cursor lands where vanilla's would.
Parsed<void> skip_braced(StringReader& reader) {
    if (auto ok = reader.expect('{'); !ok) {
        return ok;
    }
    int depth = 1;
    while (reader.can_read() && depth > 0) {
        const char c = reader.peek();
        if (StringReader::is_quoted_string_start(c)) {
            if (auto s = reader.read_quoted_string(); !s) {
                return std::unexpected{s.error()};
            }
            continue;
        }
        reader.skip();
        depth += c == '{' ? 1 : c == '}' ? -1 : 0;
    }
    if (depth > 0) {
        return std::unexpected{reader.error("parsing.expected", {Text::raw("}")})};
    }
    return {};
}

bool option_applicable(const EntitySelector& s, std::string_view name) {
    if (name == "name") {
        return !s.has_name_equals;
    }
    if (name == "distance") {
        return !s.distance;
    }
    if (name == "level") {
        return !s.level;
    }
    if (name == "x") {
        return !s.x;
    }
    if (name == "y") {
        return !s.y;
    }
    if (name == "z") {
        return !s.z;
    }
    if (name == "dx") {
        return !s.dx;
    }
    if (name == "dy") {
        return !s.dy;
    }
    if (name == "dz") {
        return !s.dz;
    }
    if (name == "x_rotation") {
        return !s.x_rotation;
    }
    if (name == "y_rotation") {
        return !s.y_rotation;
    }
    if (name == "limit") {
        return !s.current_entity && !s.limited;
    }
    if (name == "sort") {
        return !s.current_entity && !s.sorted;
    }
    if (name == "gamemode") {
        return !s.has_game_mode_equals;
    }
    if (name == "team") {
        return !s.has_team_equals;
    }
    if (name == "type") {
        return !s.type_limited;
    }
    return true;  // tag, nbt, scores, advancements, predicate
}

Parsed<void> handle_option(EntitySelector& s, std::string_view name, StringReader& reader,
                           const ParseEnv& env) {
    if (name == "name") {
        const usize i       = reader.cursor();
        const bool  negated = should_invert(reader);
        auto        value   = reader.read_string();
        if (!value) {
            return std::unexpected{value.error()};
        }
        if (s.has_name_not_equals && !negated) {
            reader.set_cursor(i);
            return std::unexpected{
                reader.error("argument.entity.options.inapplicable", {Text::raw("name")})};
        }
        (negated ? s.has_name_not_equals : s.has_name_equals) = true;
        s.names.push_back({std::move(*value), negated, false});
        return {};
    }
    if (name == "distance") {
        const usize i      = reader.cursor();
        auto        bounds = read_double_bounds(reader);
        if (!bounds) {
            return std::unexpected{bounds.error()};
        }
        if ((bounds->min && *bounds->min < 0.0) || (bounds->max && *bounds->max < 0.0)) {
            reader.set_cursor(i);
            return std::unexpected{reader.error("argument.entity.options.distance.negative")};
        }
        s.distance = *bounds;
        return {};
    }
    if (name == "level") {
        const usize i      = reader.cursor();
        auto        bounds = read_int_bounds(reader);
        if (!bounds) {
            return std::unexpected{bounds.error()};
        }
        if ((bounds->min && *bounds->min < 0) || (bounds->max && *bounds->max < 0)) {
            reader.set_cursor(i);
            return std::unexpected{reader.error("argument.entity.options.level.negative")};
        }
        s.level             = *bounds;
        s.includes_entities = false;
        return {};
    }
    const auto coordinate = [&](std::optional<f64>& into) -> Parsed<void> {
        auto value = reader.read_double();
        if (!value) {
            return std::unexpected{value.error()};
        }
        into = *value;
        return {};
    };
    if (name == "x") {
        return coordinate(s.x);
    }
    if (name == "y") {
        return coordinate(s.y);
    }
    if (name == "z") {
        return coordinate(s.z);
    }
    if (name == "dx") {
        return coordinate(s.dx);
    }
    if (name == "dy") {
        return coordinate(s.dy);
    }
    if (name == "dz") {
        return coordinate(s.dz);
    }
    if (name == "x_rotation" || name == "y_rotation") {
        auto bounds = read_angle_bounds(reader);
        if (!bounds) {
            return std::unexpected{bounds.error()};
        }
        (name == "x_rotation" ? s.x_rotation : s.y_rotation) = *bounds;
        return {};
    }
    if (name == "limit") {
        const usize i     = reader.cursor();
        auto        value = reader.read_int();
        if (!value) {
            return std::unexpected{value.error()};
        }
        if (*value < 1) {
            reader.set_cursor(i);
            return std::unexpected{reader.error("argument.entity.options.limit.toosmall")};
        }
        s.max_results = *value;
        s.limited     = true;
        return {};
    }
    if (name == "sort") {
        const usize            i     = reader.cursor();
        const std::string_view value = reader.read_unquoted_string();
        if (value == "nearest") {
            s.order = EntitySelector::Order::Nearest;
        } else if (value == "furthest") {
            s.order = EntitySelector::Order::Furthest;
        } else if (value == "random") {
            s.order = EntitySelector::Order::Random;
        } else if (value == "arbitrary") {
            s.order = EntitySelector::Order::Arbitrary;
        } else {
            reader.set_cursor(i);
            return std::unexpected{reader.error("argument.entity.options.sort.irreversible",
                                                {Text::raw(std::string{value})})};
        }
        s.sorted = true;
        return {};
    }
    if (name == "gamemode") {
        const usize i       = reader.cursor();
        const bool  negated = should_invert(reader);
        if (s.has_game_mode_not_equals && !negated) {
            reader.set_cursor(i);
            return std::unexpected{
                reader.error("argument.entity.options.inapplicable", {Text::raw("gamemode")})};
        }
        const std::string_view value = reader.read_unquoted_string();
        if (!game_mode_by_name(value)) {
            reader.set_cursor(i);
            return std::unexpected{reader.error("argument.entity.options.mode.invalid",
                                                {Text::raw(std::string{value})})};
        }
        s.includes_entities = false;
        (negated ? s.has_game_mode_not_equals : s.has_game_mode_equals) = true;
        s.game_modes.push_back({std::string{value}, negated, false});
        return {};
    }
    if (name == "team") {
        const bool       negated = should_invert(reader);
        const std::string value{reader.read_unquoted_string()};
        (negated ? s.has_team_not_equals : s.has_team_equals) = true;
        s.teams.push_back({value, negated, false});
        return {};
    }
    if (name == "type") {
        const usize i       = reader.cursor();
        const bool  negated = should_invert(reader);
        if (s.type_limited_inversely && !negated) {
            reader.set_cursor(i);
            return std::unexpected{
                reader.error("argument.entity.options.inapplicable", {Text::raw("type")})};
        }
        if (negated) {
            s.type_limited_inversely = true;
        }
        if (is_tag(reader)) {
            auto id = read_resource_location(reader);
            if (!id) {
                return std::unexpected{id.error()};
            }
            s.types.push_back({std::move(*id), negated, true});
            return {};
        }
        auto id = read_resource_location(reader);
        if (!id) {
            return std::unexpected{id.error()};
        }
        if (!env.has_entity_type(*id)) {
            reader.set_cursor(i);
            return std::unexpected{
                reader.error("argument.entity.options.type.invalid", {Text::raw(*id)})};
        }
        if (*id == "minecraft:player" && !negated) {
            s.includes_entities = false;
        }
        if (!negated) {
            s.type_limited = true;
        }
        s.types.push_back({std::move(*id), negated, false});
        return {};
    }
    if (name == "tag") {
        const bool       negated = should_invert(reader);
        const std::string value{reader.read_unquoted_string()};
        s.tags.push_back({value, negated, false});
        return {};
    }
    if (name == "nbt") {
        (void)should_invert(reader);
        auto tag = read_snbt_compound(reader);
        if (!tag) {
            return std::unexpected{tag.error()};
        }
        s.unsupported.emplace_back("nbt");
        return {};
    }
    if (name == "scores") {  // ── scoreboard ── {objective=range, …}
        if (auto ok = reader.expect('{'); !ok) {
            return ok;
        }
        reader.skip_whitespace();
        while (reader.can_read() && reader.peek() != '}') {
            reader.skip_whitespace();
            const std::string objective{reader.read_unquoted_string()};
            reader.skip_whitespace();
            if (auto ok = reader.expect('='); !ok) {
                return ok;
            }
            reader.skip_whitespace();
            auto bounds = read_int_bounds(reader);
            if (!bounds) {
                return std::unexpected{bounds.error()};
            }
            s.scores.emplace_back(objective, *bounds);
            reader.skip_whitespace();
            if (reader.can_read() && reader.peek() == ',') {
                reader.skip();
            }
        }
        return reader.expect('}');
    }
    if (name == "advancements") {
        if (auto ok = skip_braced(reader); !ok) {
            return ok;
        }
        s.unsupported.emplace_back(name);
        return {};
    }
    // predicate
    (void)should_invert(reader);
    auto id = read_resource_location(reader);
    if (!id) {
        return std::unexpected{id.error()};
    }
    s.unsupported.emplace_back("predicate");
    return {};
}

Parsed<void> parse_options(EntitySelector& s, StringReader& reader, const ParseEnv& env) {
    reader.skip_whitespace();
    while (reader.can_read() && reader.peek() != ']') {
        reader.skip_whitespace();
        const usize i    = reader.cursor();
        auto        name = reader.read_string();
        if (!name) {
            return std::unexpected{name.error()};
        }
        if (std::ranges::find(kOptionNames, *name) == kOptionNames.end()) {
            reader.set_cursor(i);
            return std::unexpected{
                reader.error("argument.entity.options.unknown", {Text::raw(*name)})};
        }
        if (!option_applicable(s, *name)) {
            // Not rewound: vanilla leaves the cursor after the name.
            return std::unexpected{
                reader.error("argument.entity.options.inapplicable", {Text::raw(*name)})};
        }
        reader.skip_whitespace();
        if (!reader.can_read() || reader.peek() != '=') {
            reader.set_cursor(i);
            return std::unexpected{
                reader.error("argument.entity.options.valueless", {Text::raw(*name)})};
        }
        reader.skip();
        reader.skip_whitespace();
        if (auto ok = handle_option(s, *name, reader, env); !ok) {
            return ok;
        }
        reader.skip_whitespace();
        if (reader.can_read()) {
            if (reader.peek() != ',') {
                if (reader.peek() != ']') {
                    return std::unexpected{reader.error("argument.entity.options.unterminated")};
                }
                break;
            }
            reader.skip();
        }
    }
    if (!reader.can_read()) {
        return std::unexpected{reader.error("argument.entity.options.unterminated")};
    }
    reader.skip();
    return {};
}

[[nodiscard]] std::string_view game_mode_name(u8 mode) {
    switch (mode) {
    case 0:
        return "survival";
    case 1:
        return "creative";
    case 2:
        return "adventure";
    default:
        return "spectator";
    }
}

bool matches(const EntitySelector& s, const EntityInfo& e, const Vec3d& origin,
             const ParseEnv* env) {
    for (const auto& m : s.names) {
        if ((e.name == m.value) == m.negated) {
            return false;
        }
    }
    for (const auto& m : s.types) {
        const bool is = m.tag ? (env != nullptr && env->entity_in_tag(m.value, e.type))
                              : e.type == m.value;
        if (is == m.negated) {
            return false;
        }
    }
    for (const auto& m : s.tags) {
        // No entity carries scoreboard tags on this server, so `tag=` (no
        // tags) is true of everything and `tag=x` of nothing.
        const bool has = m.value.empty();
        if (has == m.negated) {
            return false;
        }
    }
    for (const auto& m : s.teams) {
        // ── scoreboard ── `team=` is "on no team", `team=!` "on some team",
        // `team=x` "on x".
        const bool on = m.value.empty() ? e.team.empty() : e.team == m.value;
        if (on == m.negated) {
            return false;
        }
    }
    for (const auto& [objective, bounds] : s.scores) {  // ── scoreboard ── no score, no match
        const auto held = std::ranges::find_if(
            e.scores, [&](const std::pair<std::string, i32>& score) { return score.first == objective; });
        if (held == e.scores.end() || !bounds.contains(held->second)) {
            return false;
        }
    }
    for (const auto& m : s.game_modes) {
        if (!e.player || (game_mode_name(e.game_mode) == m.value) == m.negated) {
            return false;
        }
    }
    if (s.level && (!e.player || !s.level->contains(e.experience_level))) {
        return false;
    }
    if (s.x_rotation && !s.x_rotation->contains(e.pitch)) {
        return false;
    }
    if (s.y_rotation && !s.y_rotation->contains(e.yaw)) {
        return false;
    }
    if (s.dx || s.dy || s.dz) {
        const f64 ddx = s.dx.value_or(0.0);
        const f64 ddy = s.dy.value_or(0.0);
        const f64 ddz = s.dz.value_or(0.0);
        const f64 min_x = origin.x + std::min(0.0, ddx);
        const f64 min_y = origin.y + std::min(0.0, ddy);
        const f64 min_z = origin.z + std::min(0.0, ddz);
        const f64 max_x = origin.x + std::max(0.0, ddx) + 1.0;
        const f64 max_y = origin.y + std::max(0.0, ddy) + 1.0;
        const f64 max_z = origin.z + std::max(0.0, ddz) + 1.0;
        const f64 half  = static_cast<f64>(e.width) * 0.5;
        const bool hit = e.position.x - half < max_x && e.position.x + half > min_x &&
                         e.position.y < max_y && e.position.y + static_cast<f64>(e.height) > min_y &&
                         e.position.z - half < max_z && e.position.z + half > min_z;
        if (!hit) {
            return false;
        }
    }
    if (s.distance) {
        const f64 ddx = e.position.x - origin.x;
        const f64 ddy = e.position.y - origin.y;
        const f64 ddz = e.position.z - origin.z;
        if (!s.distance->contains_squared(ddx * ddx + ddy * ddy + ddz * ddz)) {
            return false;
        }
    }
    return true;
}

}  // namespace

Parsed<EntitySelector> parse_entity_selector(StringReader& reader, const ParseEnv& env) {
    EntitySelector s;
    if (reader.can_read() && reader.peek() == '@') {
        s.uses_selector = true;
        reader.skip();
        if (!reader.can_read()) {
            return std::unexpected{reader.error("argument.entity.selector.missing")};
        }
        const char c = reader.read();
        switch (c) {
        case 'p':
            s.max_results       = 1;
            s.includes_entities = false;
            s.order             = EntitySelector::Order::Nearest;
            s.type_limited      = true;
            break;
        case 'a':
            s.max_results       = std::numeric_limits<i32>::max();
            s.includes_entities = false;
            s.type_limited      = true;
            break;
        case 'r':
            s.max_results       = 1;
            s.includes_entities = false;
            s.order             = EntitySelector::Order::Random;
            s.type_limited      = true;
            break;
        case 's':
            s.max_results       = 1;
            s.includes_entities = true;
            s.current_entity    = true;
            break;
        case 'e':
            s.max_results       = std::numeric_limits<i32>::max();
            s.includes_entities = true;
            s.alive_only        = true;
            break;
        default:
            reader.set_cursor(reader.cursor() - 1);
            return std::unexpected{reader.error("argument.entity.selector.unknown",
                                                {Text::raw(std::string{"@"} + c)})};
        }
        if (reader.can_read() && reader.peek() == '[') {
            reader.skip();
            if (auto ok = parse_options(s, reader, env); !ok) {
                return std::unexpected{ok.error()};
            }
        }
        return s;
    }
    const usize i    = reader.cursor();
    auto        text = reader.read_string();
    if (!text) {
        return std::unexpected{text.error()};
    }
    if (const auto uuid = parse_java_uuid(*text)) {
        s.uuid              = *uuid;
        s.includes_entities = true;
    } else {
        if (text->empty() || text->size() > 16) {
            reader.set_cursor(i);
            return std::unexpected{reader.error("argument.entity.invalid")};
        }
        s.player_name       = std::move(*text);
        s.includes_entities = false;
    }
    s.max_results = 1;
    return s;
}

Parsed<std::vector<const EntityInfo*>> find_entities(const EntitySelector& selector,
                                                     const CommandSource&  source,
                                                     std::span<const EntityInfo> world,
                                                     const std::function<u32(u32)>& random_below,
                                                     const ParseEnv*                env) {
    if (selector.uses_selector && !source.has_permission(kPermissionGameMaster)) {
        return std::unexpected{
            CommandError::plain(Text::translatable("argument.entity.selector.not_allowed"))};
    }
    if (!selector.unsupported.empty()) {
        std::string names;
        for (const std::string& name : selector.unsupported) {
            names += names.empty() ? name : ", " + name;
        }
        return std::unexpected{CommandError::plain(Text::literal(
            "Ondes VOXEL does not evaluate the selector option(s) " + names + " yet"))};
    }
    std::vector<const EntityInfo*> out;
    if (selector.player_name) {
        for (const EntityInfo& e : world) {
            if (e.player && e.name.size() == selector.player_name->size() &&
                std::ranges::equal(e.name, *selector.player_name, [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                })) {
                out.push_back(&e);
                break;
            }
        }
        return out;
    }
    if (selector.uuid) {
        for (const EntityInfo& e : world) {
            if (e.uuid == *selector.uuid && (selector.includes_entities || e.player)) {
                out.push_back(&e);
                break;
            }
        }
        return out;
    }
    const Vec3d origin{selector.x.value_or(source.position.x), selector.y.value_or(source.position.y),
                       selector.z.value_or(source.position.z)};
    if (selector.current_entity) {
        if (!source.is_player()) {
            return out;
        }
        for (const EntityInfo& e : world) {
            if (e.player && e.id == source.entity_id) {
                if ((selector.includes_entities || e.player) && matches(selector, e, origin, env)) {
                    out.push_back(&e);
                }
                break;
            }
        }
        return out;
    }
    for (const EntityInfo& e : world) {
        if ((!selector.includes_entities && !e.player) || (selector.alive_only && !e.alive)) {
            continue;
        }
        if (matches(selector, e, origin, env)) {
            out.push_back(&e);
        }
    }
    if (out.size() > 1) {
        const auto distance = [&](const EntityInfo* e) {
            const f64 ddx = e->position.x - origin.x;
            const f64 ddy = e->position.y - origin.y;
            const f64 ddz = e->position.z - origin.z;
            return ddx * ddx + ddy * ddy + ddz * ddz;
        };
        switch (selector.order) {
        case EntitySelector::Order::Nearest:
            std::ranges::stable_sort(out, [&](auto* a, auto* b) { return distance(a) < distance(b); });
            break;
        case EntitySelector::Order::Furthest:
            std::ranges::stable_sort(out, [&](auto* a, auto* b) { return distance(a) > distance(b); });
            break;
        case EntitySelector::Order::Random:
            for (usize i = out.size(); i > 1; --i) {
                const u32 j = random_below(static_cast<u32>(i));
                std::swap(out[i - 1], out[j]);
            }
            break;
        case EntitySelector::Order::Arbitrary:
            break;
        }
    }
    if (out.size() > static_cast<usize>(selector.max_results)) {
        out.resize(static_cast<usize>(selector.max_results));
    }
    return out;
}

void suggest_entity_selector(SuggestionsBuilder& builder, std::span<const std::string> player_names,
                             const ParseEnv& env) {
    const std::string_view typed = builder.remaining();
    const auto bracket = typed.find('[');
    if (typed.starts_with("@") && bracket != std::string_view::npos) {
        // Inside the options: the last key, or the value of `type=`.
        const std::string_view options = typed.substr(bracket + 1);
        const auto             last_sep = options.find_last_of(",");
        const usize            key_start =
            bracket + 1 + (last_sep == std::string_view::npos ? 0 : last_sep + 1);
        const std::string_view current = typed.substr(key_start);
        const auto             equals  = current.find('=');
        if (equals == std::string_view::npos) {
            SuggestionsBuilder keys = builder.at(builder.start() + key_start);
            const std::string  lower = keys.remaining_lower();
            for (const std::string_view option : kOptionNames) {
                if (std::string_view{option}.starts_with(lower)) {
                    keys.suggest(std::string{option} + "=",
                                 Text::translatable("argument.entity.options." + std::string{option} +
                                                    ".description"));
                }
            }
            builder.add(keys);
            return;
        }
        if (current.substr(0, equals) == "type") {
            SuggestionsBuilder values = builder.at(builder.start() + key_start + equals + 1);
            std::vector<std::string> tags;
            for (const std::string& tag : env.entity_tags()) {
                tags.push_back("#" + tag);
            }
            suggest_resources(values, tags, "!");
            suggest_resources(values, env.entity_ids(), "!");
            suggest_resources(values, tags);
            suggest_resources(values, env.entity_ids());
            builder.add(values);
        }
        return;
    }
    static const std::array<std::pair<std::string_view, std::string_view>, 5> kSelectors{{
        {"@p", "argument.entity.selector.nearestPlayer"},
        {"@a", "argument.entity.selector.allPlayers"},
        {"@r", "argument.entity.selector.randomPlayer"},
        {"@s", "argument.entity.selector.self"},
        {"@e", "argument.entity.selector.allEntities"},
    }};
    const std::string lower = builder.remaining_lower();
    for (const auto& [selector, key] : kSelectors) {
        if (std::string_view{selector}.starts_with(lower)) {
            builder.suggest(std::string{selector}, Text::translatable(std::string{key}));
        }
    }
    for (const std::string& name : player_names) {
        std::string lower_name = name;
        for (char& c : lower_name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lower_name.starts_with(lower)) {
            builder.suggest(name);
        }
    }
}

}  // namespace ov::server::cmd
