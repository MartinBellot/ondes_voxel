#include "arguments.hpp"

#include "snbt.hpp"

#include "../scoreboard/scoreboard.hpp"  // ── scoreboard ── slots, colours, criteria

#include "ov/io/byte_writer.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ov::server::cmd {

i32 java_round(f32 value) noexcept {
    if (std::isnan(value)) {
        return 0;
    }
    const f32 rounded = std::floor(value + 0.5F);
    if (rounded >= 2147483647.0F) {
        return std::numeric_limits<i32>::max();
    }
    if (rounded <= -2147483648.0F) {
        return std::numeric_limits<i32>::min();
    }
    return static_cast<i32>(rounded);
}

std::string_view ArgumentType::parser_name() const noexcept {
    switch (kind) {
    case ArgKind::Bool:
        return "brigadier:bool";
    case ArgKind::Integer:
        return "brigadier:integer";
    case ArgKind::Word:
    case ArgKind::Greedy:
        return "brigadier:string";
    case ArgKind::Entity:
        return "minecraft:entity";
    case ArgKind::GameProfile:
        return "minecraft:game_profile";
    case ArgKind::BlockPos:
        return "minecraft:block_pos";
    case ArgKind::Vec3:
        return "minecraft:vec3";
    case ArgKind::Rotation:
        return "minecraft:rotation";
    case ArgKind::Angle:
        return "minecraft:angle";
    case ArgKind::BlockState:
        return "minecraft:block_state";
    case ArgKind::BlockPredicate:
        return "minecraft:block_predicate";
    case ArgKind::ItemStack:
        return "minecraft:item_stack";
    case ArgKind::ItemPredicate:
        return "minecraft:item_predicate";
    case ArgKind::Component:
        return "minecraft:component";
    case ArgKind::Message:
        return "minecraft:message";
    case ArgKind::NbtCompound:
        return "minecraft:nbt_compound_tag";
    case ArgKind::EntityAnchor:
        return "minecraft:entity_anchor";
    case ArgKind::GameMode:
        return "minecraft:gamemode";
    case ArgKind::Time:
        return "minecraft:time";
    case ArgKind::Resource:
        return "minecraft:resource";
    // ── scoreboard ──
    case ArgKind::Objective:
        return "minecraft:objective";
    case ArgKind::ObjectiveCriteria:
        return "minecraft:objective_criteria";
    case ArgKind::Operation:
        return "minecraft:operation";
    case ArgKind::ScoreboardSlot:
        return "minecraft:scoreboard_slot";
    case ArgKind::ScoreHolder:
        return "minecraft:score_holder";
    case ArgKind::Team:
        return "minecraft:team";
    case ArgKind::Color:
        return "minecraft:color";
    }
    return "brigadier:string";
}

std::vector<u8> ArgumentType::wire_properties() const {
    io::ByteWriter writer;
    switch (kind) {
    case ArgKind::Integer:
        writer.write_u8(static_cast<u8>((has_min ? 0x01 : 0) | (has_max ? 0x02 : 0)));
        if (has_min) {
            writer.write_i32(min);
        }
        if (has_max) {
            writer.write_i32(max);
        }
        break;
    case ArgKind::Word:
        net::write_varint(writer, 0);
        break;
    case ArgKind::Greedy:
        net::write_varint(writer, 2);
        break;
    case ArgKind::Entity:
        writer.write_u8(static_cast<u8>((single ? 0x01 : 0) | (players_only ? 0x02 : 0)));
        break;
    case ArgKind::Time:
        writer.write_i32(min);
        break;
    case ArgKind::Resource:
        net::write_string(writer, registry);
        break;
    case ArgKind::ScoreHolder:  // ── scoreboard ── 0x01: several holders allowed
        writer.write_u8(single ? 0 : 1);
        break;
    default:
        break;
    }
    return writer.take();
}

namespace {

[[nodiscard]] bool is_allowed_in_resource_location(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c == '/' ||
           c == '.' || c == '-';
}

Parsed<std::string> read_id(StringReader& reader) {
    const usize start = reader.cursor();
    while (reader.can_read() && is_allowed_in_resource_location(reader.peek())) {
        reader.skip();
    }
    const std::string_view text  = reader.string().substr(start, reader.cursor() - start);
    const auto             colon = text.find(':');
    const std::string_view name_space = colon == std::string_view::npos ? "minecraft"
                                                                        : text.substr(0, colon);
    const std::string_view path = colon == std::string_view::npos ? text : text.substr(colon + 1);
    const bool             ok =
        std::ranges::all_of(name_space,
                            [](char c) {
                                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                       c == '_' || c == '.' || c == '-';
                            }) &&
        path.find(':') == std::string_view::npos;
    if (!ok) {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.id.invalid")};
    }
    return std::string{name_space} + ":" + std::string{path};
}

// ── Coordinates ─────────────────────────────────────────────────────────────

Parsed<Coordinate> world_int(StringReader& reader) {
    if (reader.can_read() && reader.peek() == '^') {
        return std::unexpected{reader.error("argument.pos.mixed")};
    }
    if (!reader.can_read()) {
        return std::unexpected{reader.error("argument.pos.missing.int")};
    }
    Coordinate out;
    if (reader.peek() == '~') {
        out.relative = true;
        reader.skip();
    }
    if (reader.can_read() && reader.peek() != ' ') {
        if (out.relative) {
            auto value = reader.read_double();
            if (!value) {
                return std::unexpected{value.error()};
            }
            out.value = *value;
        } else {
            auto value = reader.read_int();
            if (!value) {
                return std::unexpected{value.error()};
            }
            out.value = *value;
        }
    }
    return out;
}

Parsed<Coordinate> world_double(StringReader& reader, bool center) {
    if (reader.can_read() && reader.peek() == '^') {
        return std::unexpected{reader.error("argument.pos.mixed")};
    }
    if (!reader.can_read()) {
        return std::unexpected{reader.error("argument.pos.missing.double")};
    }
    Coordinate out;
    if (reader.peek() == '~') {
        out.relative = true;
        reader.skip();
    }
    const usize start = reader.cursor();
    if (reader.can_read() && reader.peek() != ' ') {
        auto value = reader.read_double();
        if (!value) {
            return std::unexpected{value.error()};
        }
        out.value = *value;
    }
    const std::string_view written = reader.string().substr(start, reader.cursor() - start);
    if (out.relative && written.empty()) {
        return out;
    }
    // A whole-number x or z names the middle of the block, as vanilla's does:
    // `tp 10 -60 10` lands on 10.5, -60, 10.5.
    if (written.find('.') == std::string_view::npos && !out.relative && center) {
        out.value += 0.5;
    }
    return out;
}

Parsed<f64> local_value(StringReader& reader, usize start) {
    if (!reader.can_read()) {
        return std::unexpected{reader.error("argument.pos.missing.double")};
    }
    if (reader.peek() != '^') {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.pos.mixed")};
    }
    reader.skip();
    if (reader.can_read() && reader.peek() != ' ') {
        return reader.read_double();
    }
    return 0.0;
}

Parsed<Coordinates> local_coordinates(StringReader& reader) {
    const usize start = reader.cursor();
    Coordinates out;
    out.local = true;
    for (usize axis = 0; axis < 3; ++axis) {
        if (axis > 0) {
            if (!reader.can_read() || reader.peek() != ' ') {
                reader.set_cursor(start);
                return std::unexpected{reader.error("argument.pos3d.incomplete")};
            }
            reader.skip();
        }
        auto value = local_value(reader, start);
        if (!value) {
            return std::unexpected{value.error()};
        }
        out.axes[axis] = Coordinate{true, *value};
    }
    return out;
}

Parsed<Coordinates> world_coordinates(StringReader& reader, bool integers) {
    const usize start = reader.cursor();
    Coordinates out;
    for (usize axis = 0; axis < 3; ++axis) {
        if (axis > 0) {
            if (!reader.can_read() || reader.peek() != ' ') {
                reader.set_cursor(start);
                return std::unexpected{reader.error("argument.pos3d.incomplete")};
            }
            reader.skip();
        }
        auto value = integers ? world_int(reader) : world_double(reader, axis != 1);
        if (!value) {
            return std::unexpected{value.error()};
        }
        out.axes[axis] = *value;
    }
    return out;
}

// ── Blocks ──────────────────────────────────────────────────────────────────

Parsed<std::vector<std::pair<std::string, std::string>>> read_properties(
    StringReader& reader, const ParseEnv& env, std::optional<registry::BlockId> block,
    std::string_view block_id) {
    std::vector<std::pair<std::string, std::string>> out;
    reader.skip();  // '['
    reader.skip_whitespace();
    while (reader.can_read() && reader.peek() != ']') {
        reader.skip_whitespace();
        const usize start = reader.cursor();
        auto        name  = reader.read_string();
        if (!name) {
            return std::unexpected{name.error()};
        }
        std::optional<registry::PropertyView> property;
        if (block && env.blocks() != nullptr) {
            property = env.blocks()->find_property(*block, *name);
            if (!property) {
                reader.set_cursor(start);
                return std::unexpected{reader.error(
                    "argument.block.property.unknown",
                    {Text::raw(std::string{block_id}), Text::raw(*name)})};
            }
        }
        if (std::ranges::any_of(out, [&](const auto& p) { return p.first == *name; })) {
            reader.set_cursor(start);
            return std::unexpected{reader.error(
                "argument.block.property.duplicate",
                {Text::raw(*name), Text::raw(std::string{block_id})})};
        }
        reader.skip_whitespace();
        if (!reader.can_read() || reader.peek() != '=') {
            return std::unexpected{reader.error(
                "argument.block.property.novalue",
                {Text::raw(*name), Text::raw(std::string{block_id})})};
        }
        reader.skip();
        reader.skip_whitespace();
        const usize value_start = reader.cursor();
        auto        value       = reader.read_string();
        if (!value) {
            return std::unexpected{value.error()};
        }
        if (property &&
            std::ranges::find(property->values, std::string_view{*value}) == property->values.end()) {
            reader.set_cursor(value_start);
            return std::unexpected{reader.error(
                "argument.block.property.invalid",
                {Text::raw(std::string{block_id}), Text::raw(*value), Text::raw(*name)})};
        }
        out.emplace_back(std::move(*name), std::move(*value));
        reader.skip_whitespace();
        if (!reader.can_read()) {
            continue;
        }
        if (reader.peek() == ',') {
            reader.skip();
            continue;
        }
        if (reader.peek() != ']') {
            return std::unexpected{reader.error("argument.block.property.unclosed")};
        }
    }
    if (!reader.can_read()) {
        return std::unexpected{reader.error("argument.block.property.unclosed")};
    }
    reader.skip();  // ']'
    return out;
}

Parsed<BlockStateArg> parse_block_state(StringReader& reader, const ParseEnv& env) {
    const usize start = reader.cursor();
    if (reader.can_read() && reader.peek() == '#') {
        return std::unexpected{reader.error("argument.block.tag.disallowed")};
    }
    auto id = read_id(reader);
    if (!id) {
        return std::unexpected{id.error()};
    }
    const auto block = env.find_block(*id);
    if (!block) {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.block.id.invalid", {Text::raw(*id)})};
    }
    BlockStateArg out;
    out.block = *block;
    out.id    = *id;
    out.state = env.blocks()->default_state(*block);
    if (reader.can_read() && reader.peek() == '[') {
        auto properties = read_properties(reader, env, block, *id);
        if (!properties) {
            return std::unexpected{properties.error()};
        }
        // One property at a time from the default state. Asking for the state
        // matching a partial set resolved the unnamed ones to whatever came
        // first — waterlogged chests on dry land, in a previous wave.
        for (const auto& [name, value] : *properties) {
            const auto property = env.blocks()->find_property(*block, name);
            const auto index    = static_cast<u16>(std::distance(
                property->values.begin(), std::ranges::find(property->values, std::string_view{value})));
            out.state = env.blocks()->with_property(out.state, *property, index);
        }
    }
    if (reader.can_read() && reader.peek() == '{') {
        auto tag = read_snbt_compound(reader);
        if (!tag) {
            return std::unexpected{tag.error()};
        }
        out.nbt = std::move(*tag);
    }
    return out;
}

Parsed<BlockPredicateArg> parse_block_predicate(StringReader& reader, const ParseEnv& env) {
    BlockPredicateArg out;
    const usize       start = reader.cursor();
    std::optional<registry::BlockId> block;
    if (reader.can_read() && reader.peek() == '#') {
        reader.skip();
        auto id = read_id(reader);
        if (!id) {
            return std::unexpected{id.error()};
        }
        if (!env.has_block_tag(*id)) {
            reader.set_cursor(start);
            return std::unexpected{reader.error("arguments.block.tag.unknown", {Text::raw(*id)})};
        }
        out.tag = true;
        out.id  = *id;
    } else {
        auto id = read_id(reader);
        if (!id) {
            return std::unexpected{id.error()};
        }
        block = env.find_block(*id);
        if (!block) {
            reader.set_cursor(start);
            return std::unexpected{reader.error("argument.block.id.invalid", {Text::raw(*id)})};
        }
        out.id    = *id;
        out.block = block;
    }
    if (reader.can_read() && reader.peek() == '[') {
        auto properties = read_properties(reader, env, block, out.id);
        if (!properties) {
            return std::unexpected{properties.error()};
        }
        out.properties = std::move(*properties);
    }
    if (reader.can_read() && reader.peek() == '{') {
        auto tag = read_snbt_compound(reader);
        if (!tag) {
            return std::unexpected{tag.error()};
        }
        out.nbt = std::move(*tag);
    }
    return out;
}

// ── Items ───────────────────────────────────────────────────────────────────

Parsed<ItemArg> parse_item(StringReader& reader, const ParseEnv& env) {
    const usize start = reader.cursor();
    if (reader.can_read() && reader.peek() == '#') {
        return std::unexpected{reader.error("argument.item.tag.disallowed")};
    }
    auto id = read_id(reader);
    if (!id) {
        return std::unexpected{id.error()};
    }
    const auto item = env.find_item(*id);
    if (!item) {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.item.id.invalid", {Text::raw(*id)})};
    }
    ItemArg out;
    out.item = *item;
    out.id   = *id;
    if (reader.can_read() && reader.peek() == '{') {
        auto tag = read_snbt_compound(reader);
        if (!tag) {
            return std::unexpected{tag.error()};
        }
        out.nbt = std::move(*tag);
    }
    return out;
}

Parsed<ItemPredicateArg> parse_item_predicate(StringReader& reader, const ParseEnv& env) {
    ItemPredicateArg out;
    const usize      start = reader.cursor();
    if (reader.can_read() && reader.peek() == '#') {
        reader.skip();
        auto id = read_id(reader);
        if (!id) {
            return std::unexpected{id.error()};
        }
        if (!env.has_item_tag(*id)) {
            reader.set_cursor(start);
            return std::unexpected{reader.error("arguments.item.tag.unknown", {Text::raw(*id)})};
        }
        out.tag = true;
        out.id  = *id;
    } else {
        auto id = read_id(reader);
        if (!id) {
            return std::unexpected{id.error()};
        }
        const auto item = env.find_item(*id);
        if (!item) {
            reader.set_cursor(start);
            return std::unexpected{reader.error("argument.item.id.invalid", {Text::raw(*id)})};
        }
        out.id   = *id;
        out.item = *item;
    }
    if (reader.can_read() && reader.peek() == '{') {
        auto tag = read_snbt_compound(reader);
        if (!tag) {
            return std::unexpected{tag.error()};
        }
        out.nbt = std::move(*tag);
    }
    return out;
}

// ── The rest ────────────────────────────────────────────────────────────────

Parsed<MessageArg> parse_message(StringReader& reader, const ParseEnv& env) {
    MessageArg out;
    const usize base = reader.cursor();
    out.text         = std::string{reader.remaining()};
    // Selectors inside a message are parsed now, so a bad one is an error at
    // the right place; whether they are *resolved* depends on who runs it.
    for (usize i = 0; i + 1 < out.text.size(); ++i) {
        const char next = out.text[i + 1];
        if (out.text[i] != '@' ||
            (next != 'p' && next != 'a' && next != 'r' && next != 's' && next != 'e')) {
            continue;
        }
        StringReader inner{reader.string(), base + i};
        auto         selector = parse_entity_selector(inner, env);
        if (!selector) {
            continue;  // vanilla leaves text it cannot read as a selector alone
        }
        const usize end = inner.cursor() - base;
        out.selectors.push_back({i, end, std::move(*selector)});
        i = end - 1;
    }
    reader.set_cursor(reader.string().size());
    return out;
}

Parsed<Text> parse_component(StringReader& reader) {
    const usize start    = reader.cursor();
    usize       consumed = 0;
    const auto  json     = parse_json(reader.remaining(), consumed);
    if (!json) {
        reader.set_cursor(start);
        return std::unexpected{
            reader.error("argument.component.invalid", {Text::raw(json.error().message)})};
    }
    auto text = text_from_json(*json);
    if (!text) {
        reader.set_cursor(start);
        return std::unexpected{reader.error("argument.component.invalid", {Text::raw(text.error())})};
    }
    reader.set_cursor(start + consumed);
    return std::move(*text);
}

Parsed<TimeArg> parse_time(StringReader& reader, i32 min) {
    auto amount = reader.read_float();
    if (!amount) {
        return std::unexpected{amount.error()};
    }
    const std::string_view unit       = reader.read_unquoted_string();
    i32                    multiplier = 0;
    if (unit.empty() || unit == "t") {
        multiplier = 1;
    } else if (unit == "s") {
        multiplier = 20;
    } else if (unit == "d") {
        multiplier = 24000;
    }
    if (multiplier == 0) {
        // No cursor: vanilla raises this one without a context line.
        return std::unexpected{CommandError::plain(Text::translatable("argument.time.invalid_unit"))};
    }
    const i32 ticks = java_round(*amount * static_cast<f32>(multiplier));
    if (ticks < min) {
        return std::unexpected{CommandError::plain(Text::translatable(
            "argument.time.tick_count_too_low",
            {Text::raw(std::to_string(min)), Text::raw(std::to_string(ticks))}))};
    }
    return TimeArg{ticks};
}

}  // namespace

bool BlockPredicateArg::test(registry::BlockStateId state, const ParseEnv& env) const {
    const registry::BlockRegistry* blocks = env.blocks();
    if (blocks == nullptr) {
        return false;
    }
    const registry::BlockId actual = blocks->block_of(state);
    if (tag ? !env.block_in_tag(id, actual) : (block && actual != *block)) {
        return false;
    }
    for (const auto& [name, value] : properties) {
        const auto property = blocks->find_property(actual, name);
        if (!property || blocks->property_value(state, *property) != value) {
            return false;
        }
    }
    // A block entity's NBT is not compared: the chunk's block entities are not
    // reachable from a state id. Stated in docs/provenance/commandes.md.
    return true;
}

bool ItemPredicateArg::test(const net::ItemStack& stack, const ParseEnv& env) const {
    if (stack.empty()) {
        return false;
    }
    if (tag ? !env.item_in_tag(id, stack.item_id) : stack.item_id != item) {
        return false;
    }
    if (!nbt) {
        return true;
    }
    if (stack.nbt.empty()) {
        return nbt->empty();
    }
    const auto document = nbt::read(stack.nbt);
    return document && snbt_matches(*nbt, document->root);
}

Parsed<ArgValue> parse_argument(const ArgumentType& type, StringReader& reader,
                                const ParseEnv& env) {
    switch (type.kind) {
    case ArgKind::Bool: {
        auto value = reader.read_boolean();
        if (!value) {
            return std::unexpected{value.error()};
        }
        return ArgValue{*value};
    }
    case ArgKind::Integer: {
        const usize start = reader.cursor();
        auto        value = reader.read_int();
        if (!value) {
            return std::unexpected{value.error()};
        }
        if (type.has_min && *value < type.min) {
            reader.set_cursor(start);
            return std::unexpected{reader.error(
                "argument.integer.low",
                {Text::raw(std::to_string(type.min)), Text::raw(std::to_string(*value))})};
        }
        if (type.has_max && *value > type.max) {
            reader.set_cursor(start);
            return std::unexpected{reader.error(
                "argument.integer.big",
                {Text::raw(std::to_string(type.max)), Text::raw(std::to_string(*value))})};
        }
        return ArgValue{*value};
    }
    case ArgKind::Word:
        return ArgValue{std::string{reader.read_unquoted_string()}};
    case ArgKind::Greedy: {
        std::string text{reader.remaining()};
        reader.set_cursor(reader.string().size());
        return ArgValue{std::move(text)};
    }
    case ArgKind::Entity: {
        auto selector = parse_entity_selector(reader, env);
        if (!selector) {
            return std::unexpected{selector.error()};
        }
        // These two reset the cursor to the very start of the command, which
        // is why vanilla's `gamemode creative @e` underlines all of it.
        if (selector->max_results > 1 && type.single) {
            reader.set_cursor(0);
            return std::unexpected{reader.error(type.players_only ? "argument.player.toomany"
                                                                  : "argument.entity.toomany")};
        }
        if (selector->includes_entities && type.players_only && !selector->is_self()) {
            reader.set_cursor(0);
            return std::unexpected{reader.error("argument.player.entities")};
        }
        return ArgValue{std::move(*selector)};
    }
    case ArgKind::GameProfile: {
        GameProfileArg out;
        if (reader.can_read() && reader.peek() == '@') {
            auto selector = parse_entity_selector(reader, env);
            if (!selector) {
                return std::unexpected{selector.error()};
            }
            if (selector->includes_entities) {
                return std::unexpected{
                    CommandError::plain(Text::translatable("argument.player.entities"))};
            }
            out.selector = std::move(*selector);
        } else {
            const usize start = reader.cursor();
            while (reader.can_read() && reader.peek() != ' ') {
                reader.skip();
            }
            out.name = std::string{reader.string().substr(start, reader.cursor() - start)};
        }
        return ArgValue{std::move(out)};
    }
    case ArgKind::BlockPos: {
        auto coords = reader.can_read() && reader.peek() == '^' ? local_coordinates(reader)
                                                                : world_coordinates(reader, true);
        if (!coords) {
            return std::unexpected{coords.error()};
        }
        return ArgValue{*coords};
    }
    case ArgKind::Vec3: {
        auto coords = reader.can_read() && reader.peek() == '^' ? local_coordinates(reader)
                                                                : world_coordinates(reader, false);
        if (!coords) {
            return std::unexpected{coords.error()};
        }
        return ArgValue{*coords};
    }
    case ArgKind::Rotation: {
        const usize start = reader.cursor();
        if (!reader.can_read()) {
            return std::unexpected{reader.error("argument.rotation.incomplete")};
        }
        RotationArg out;
        auto        yaw = world_double(reader, false);
        if (!yaw) {
            return std::unexpected{yaw.error()};
        }
        if (!reader.can_read() || reader.peek() != ' ') {
            reader.set_cursor(start);
            return std::unexpected{reader.error("argument.rotation.incomplete")};
        }
        reader.skip();
        auto pitch = world_double(reader, false);
        if (!pitch) {
            return std::unexpected{pitch.error()};
        }
        out.axes = {*yaw, *pitch};
        return ArgValue{out};
    }
    case ArgKind::Angle: {
        if (!reader.can_read()) {
            return std::unexpected{reader.error("argument.angle.incomplete")};
        }
        AngleArg out;
        if (reader.peek() == '~') {
            out.relative = true;
            reader.skip();
        }
        if (reader.can_read() && reader.peek() != ' ') {
            auto value = reader.read_float();
            if (!value) {
                return std::unexpected{value.error()};
            }
            out.value = *value;
        }
        if (std::isnan(out.value) || std::isinf(out.value)) {
            return std::unexpected{reader.error("argument.angle.invalid")};
        }
        return ArgValue{out};
    }
    case ArgKind::BlockState: {
        auto state = parse_block_state(reader, env);
        if (!state) {
            return std::unexpected{state.error()};
        }
        return ArgValue{std::move(*state)};
    }
    case ArgKind::BlockPredicate: {
        auto predicate = parse_block_predicate(reader, env);
        if (!predicate) {
            return std::unexpected{predicate.error()};
        }
        return ArgValue{std::move(*predicate)};
    }
    case ArgKind::ItemStack: {
        auto item = parse_item(reader, env);
        if (!item) {
            return std::unexpected{item.error()};
        }
        return ArgValue{std::move(*item)};
    }
    case ArgKind::ItemPredicate: {
        auto predicate = parse_item_predicate(reader, env);
        if (!predicate) {
            return std::unexpected{predicate.error()};
        }
        return ArgValue{std::move(*predicate)};
    }
    case ArgKind::Component: {
        auto text = parse_component(reader);
        if (!text) {
            return std::unexpected{text.error()};
        }
        return ArgValue{std::move(*text)};
    }
    case ArgKind::Message: {
        auto message = parse_message(reader, env);
        if (!message) {
            return std::unexpected{message.error()};
        }
        return ArgValue{std::move(*message)};
    }
    case ArgKind::NbtCompound: {
        auto tag = read_snbt_compound(reader);
        if (!tag) {
            return std::unexpected{tag.error()};
        }
        return ArgValue{std::move(*tag)};
    }
    case ArgKind::EntityAnchor: {
        const usize            start = reader.cursor();
        const std::string_view name  = reader.read_unquoted_string();
        if (name == "feet" || name == "eyes") {
            return ArgValue{AnchorArg{name == "eyes"}};
        }
        reader.set_cursor(start);
        return std::unexpected{
            reader.error("argument.anchor.invalid", {Text::raw(std::string{name})})};
    }
    case ArgKind::GameMode: {
        const std::string_view name = reader.read_unquoted_string();
        u8                     mode = 0xFF;
        if (name == "survival") {
            mode = 0;
        } else if (name == "creative") {
            mode = 1;
        } else if (name == "adventure") {
            mode = 2;
        } else if (name == "spectator") {
            mode = 3;
        }
        if (mode == 0xFF) {
            // Not rewound: vanilla's context line ends after the word.
            return std::unexpected{
                reader.error("argument.gamemode.invalid", {Text::raw(std::string{name})})};
        }
        return ArgValue{GameModeArg{mode}};
    }
    case ArgKind::Time: {
        auto ticks = parse_time(reader, type.min);
        if (!ticks) {
            return std::unexpected{ticks.error()};
        }
        return ArgValue{*ticks};
    }
    case ArgKind::Resource: {
        auto id = read_id(reader);
        if (!id) {
            return std::unexpected{id.error()};
        }
        return ArgValue{ResourceArg{std::move(*id)}};
    }
    // ── scoreboard ── The objective and the team are only read here; whether
    // they exist is the command's to say, at execution, with no place in the
    // input — the capture's `arguments.objective.notFound` has no second
    // line. The slot, the colour, the operation and the criterion are checked
    // now, and fail the same way: a message and nothing under it.
    case ArgKind::Objective:
    case ArgKind::Team:
        return ArgValue{std::string{reader.read_unquoted_string()}};
    case ArgKind::ScoreboardSlot: {
        std::string name{reader.read_unquoted_string()};
        if (!display_slot_from_name(name)) {
            return std::unexpected{CommandError::plain(
                Text::translatable("argument.scoreboardDisplaySlot.invalid", {Text::raw(name)}))};
        }
        return ArgValue{std::move(name)};
    }
    case ArgKind::Color: {
        std::string name{reader.read_unquoted_string()};
        if (!color_from_name(name)) {
            return std::unexpected{
                CommandError::plain(Text::translatable("argument.color.invalid", {Text::raw(name)}))};
        }
        return ArgValue{std::move(name)};
    }
    case ArgKind::Operation:
    case ArgKind::ObjectiveCriteria: {
        const usize start = reader.cursor();
        while (reader.can_read() && reader.peek() != ' ') {
            reader.skip();
        }
        std::string text{reader.string().substr(start, reader.cursor() - start)};
        if (type.kind == ArgKind::Operation) {
            static constexpr std::array<std::string_view, 9> kOperations{"=",  "+=", "-=", "*=", "/=",
                                                                         "%=", "<",  ">",  "><"};
            if (std::ranges::find(kOperations, text) == kOperations.end()) {
                return std::unexpected{
                    CommandError::plain(Text::translatable("arguments.operation.invalid"))};
            }
        } else if (!parse_criterion(text, env.registries())) {
            return std::unexpected{
                CommandError::plain(Text::translatable("argument.criteria.invalid", {Text::raw(text)}))};
        }
        return ArgValue{std::move(text)};
    }
    case ArgKind::ScoreHolder: {
        if (reader.can_read() && reader.peek() == '@') {
            auto selector = parse_entity_selector(reader, env);
            if (!selector) {
                return std::unexpected{selector.error()};
            }
            if (type.single && selector->max_results > 1) {
                return std::unexpected{
                    CommandError::plain(Text::translatable("argument.entity.toomany"))};
            }
            return ArgValue{GameProfileArg{std::move(*selector), {}}};
        }
        const usize start = reader.cursor();
        while (reader.can_read() && reader.peek() != ' ') {
            reader.skip();
        }
        return ArgValue{
            GameProfileArg{std::nullopt, std::string{reader.string().substr(start, reader.cursor() - start)}}};
    }
    }
    return std::unexpected{reader.error("command.unknown.argument")};
}

namespace {

void suggest_coordinates(SuggestionsBuilder& builder, usize count) {
    const std::string_view typed = builder.remaining();
    const char             glyph = !typed.empty() && typed.front() == '^' ? '^' : '~';
    std::string            candidate;
    for (usize i = 0; i < count; ++i) {
        candidate += i == 0 ? std::string(1, glyph) : std::string{" "} + glyph;
        if (std::string_view{candidate}.starts_with(typed)) {
            builder.suggest(candidate);
        }
    }
}

void suggest_block(SuggestionsBuilder& builder, const ParseEnv& env, bool allow_tags) {
    const std::string_view typed   = builder.remaining();
    const auto             bracket = typed.find('[');
    if (bracket == std::string_view::npos) {
        if (allow_tags) {
            std::vector<std::string> tags;
            for (const std::string& tag : env.block_tags()) {
                tags.push_back("#" + tag);
            }
            suggest_resources(builder, tags);
        }
        suggest_resources(builder, env.block_ids());
        return;
    }
    const std::string id    = full_id(typed.substr(0, bracket));
    const auto        block = env.find_block(id);
    if (!block || env.blocks() == nullptr) {
        return;
    }
    const std::string_view inside   = typed.substr(bracket + 1);
    const auto             last_sep = inside.find_last_of(',');
    const usize            token_start =
        bracket + 1 + (last_sep == std::string_view::npos ? 0 : last_sep + 1);
    const std::string_view token  = typed.substr(token_start);
    const auto             equals = token.find('=');
    SuggestionsBuilder     at     = builder.at(builder.start() + token_start);
    if (equals == std::string_view::npos) {
        if (token.empty()) {
            at.suggest("]");
        }
        for (const registry::PropertyView& property : env.blocks()->properties(*block)) {
            const std::string name{property.name};
            if (inside.find(name + "=") != std::string_view::npos) {
                continue;
            }
            if (name.starts_with(token)) {
                at.suggest(name + "=");
            }
        }
    } else {
        const auto property = env.blocks()->find_property(*block, token.substr(0, equals));
        if (property) {
            SuggestionsBuilder values = builder.at(builder.start() + token_start + equals + 1);
            for (const std::string_view value : property->values) {
                if (value.starts_with(token.substr(equals + 1))) {
                    values.suggest(std::string{value});
                }
            }
            builder.add(values);
        }
        return;
    }
    builder.add(at);
}

}  // namespace

void suggest_argument(const ArgumentType& type, SuggestionsBuilder& builder, const ParseEnv& env,
                      std::span<const std::string> player_names) {
    switch (type.kind) {
    case ArgKind::Bool:
        for (const std::string_view value : {"true", "false"}) {
            if (value.starts_with(builder.remaining_lower())) {
                builder.suggest(std::string{value});
            }
        }
        break;
    case ArgKind::Entity:
        suggest_entity_selector(builder, player_names, env);
        break;
    case ArgKind::GameProfile:
        for (const std::string& name : player_names) {
            if (std::string_view{name}.starts_with(builder.remaining())) {
                builder.suggest(name);
            }
        }
        break;
    case ArgKind::BlockPos:
    case ArgKind::Vec3:
        suggest_coordinates(builder, 3);
        break;
    case ArgKind::Rotation:
        suggest_coordinates(builder, 2);
        break;
    case ArgKind::BlockState:
        suggest_block(builder, env, false);
        break;
    case ArgKind::BlockPredicate:
        suggest_block(builder, env, true);
        break;
    case ArgKind::ItemStack:
        suggest_resources(builder, env.item_ids());
        break;
    case ArgKind::ItemPredicate: {
        std::vector<std::string> tags;
        for (const std::string& tag : env.item_tags()) {
            tags.push_back("#" + tag);
        }
        suggest_resources(builder, tags);
        suggest_resources(builder, env.item_ids());
        break;
    }
    case ArgKind::EntityAnchor:
        for (const std::string_view value : {"eyes", "feet"}) {
            if (value.starts_with(builder.remaining_lower())) {
                builder.suggest(std::string{value});
            }
        }
        break;
    case ArgKind::GameMode:
        for (const std::string_view value : {"survival", "creative", "adventure", "spectator"}) {
            if (value.starts_with(builder.remaining_lower())) {
                builder.suggest(std::string{value});
            }
        }
        break;
    case ArgKind::Time: {
        StringReader probe{builder.remaining()};
        if (!probe.read_float()) {
            break;
        }
        SuggestionsBuilder units = builder.at(builder.start() + probe.cursor());
        for (const std::string_view unit : {"d", "s", "t"}) {
            if (unit.starts_with(units.remaining_lower())) {
                units.suggest(std::string{unit});
            }
        }
        builder.add(units);
        break;
    }
    case ArgKind::Resource: {
        if (type.registry != "minecraft:entity_type") {
            suggest_resources(builder, env.registry_ids(type.registry));
            break;
        }
        // `summon`: every type but the two that cannot be summoned.
        std::vector<std::string>          ids;
        std::vector<std::optional<Text>>  tooltips;
        for (const std::string& id : env.entity_ids()) {
            if (id == "minecraft:player" || id == "minecraft:fishing_bobber") {
                continue;
            }
            ids.push_back(id);
            const auto colon = id.find(':');
            tooltips.emplace_back(Text::translatable(
                "entity." + id.substr(0, colon) + "." + id.substr(colon + 1)));
        }
        suggest_resources(builder, ids, {}, &tooltips);
        break;
    }
    // ── scoreboard ── The fixed vocabularies. Objectives and teams are the
    // scoreboard's, which the network thread may not read: not suggested.
    case ArgKind::ScoreHolder:
        suggest_entity_selector(builder, player_names, env);
        break;
    case ArgKind::ScoreboardSlot:
    case ArgKind::Color:
    case ArgKind::Operation:
    case ArgKind::ObjectiveCriteria: {
        std::vector<std::string> words;
        if (type.kind == ArgKind::ScoreboardSlot) {
            for (u8 slot = 0; slot < net::scoreboard::kSlotCount; ++slot) {
                words.push_back(display_slot_name(slot));
            }
        } else if (type.kind == ArgKind::Color) {
            for (u8 color = 0; color < kColorCount; ++color) {
                words.emplace_back(color_name(color));
            }
            words.emplace_back("reset");
        } else if (type.kind == ArgKind::Operation) {
            words = {"=", "+=", "-=", "*=", "/=", "%=", "<", ">", "><"};
        } else {
            words = {"dummy", "trigger", "deathCount", "playerKillCount", "totalKillCount",
                     "health", "food", "air", "armor", "xp", "level"};
            for (u8 color = 0; color < kColorCount; ++color) {
                words.push_back("teamkill." + std::string{color_name(color)});
                words.push_back("killedByTeam." + std::string{color_name(color)});
            }
        }
        for (const std::string& word : words) {
            if (std::string_view{word}.starts_with(builder.remaining())) {
                builder.suggest(word);
            }
        }
        break;
    }
    default:
        break;
    }
}

}  // namespace ov::server::cmd
