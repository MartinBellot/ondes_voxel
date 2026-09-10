#include "dispatcher.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>

namespace ov::server::cmd {
namespace {

/// `minecraft:command_argument_type` in id order, for a server started
/// without the registry pack. With the pack loaded the ids come from it, and
/// `test_commands` checks this list against the data generator's report.
constexpr std::array<std::string_view, 49> kParserIds{
    "brigadier:bool",           "brigadier:float",
    "brigadier:double",         "brigadier:integer",
    "brigadier:long",           "brigadier:string",
    "minecraft:entity",         "minecraft:game_profile",
    "minecraft:block_pos",      "minecraft:column_pos",
    "minecraft:vec3",           "minecraft:vec2",
    "minecraft:block_state",    "minecraft:block_predicate",
    "minecraft:item_stack",     "minecraft:item_predicate",
    "minecraft:color",          "minecraft:component",
    "minecraft:message",        "minecraft:nbt_compound_tag",
    "minecraft:nbt_tag",        "minecraft:nbt_path",
    "minecraft:objective",      "minecraft:objective_criteria",
    "minecraft:operation",      "minecraft:particle",
    "minecraft:angle",          "minecraft:rotation",
    "minecraft:scoreboard_slot", "minecraft:score_holder",
    "minecraft:swizzle",        "minecraft:team",
    "minecraft:item_slot",      "minecraft:resource_location",
    "minecraft:function",       "minecraft:entity_anchor",
    "minecraft:int_range",      "minecraft:float_range",
    "minecraft:dimension",      "minecraft:gamemode",
    "minecraft:time",           "minecraft:resource_or_tag",
    "minecraft:resource_or_tag_key", "minecraft:resource",
    "minecraft:resource_key",   "minecraft:template_mirror",
    "minecraft:template_rotation", "minecraft:heightmap",
    "minecraft:uuid"};

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

std::span<const std::string_view> parser_id_table() noexcept { return kParserIds; }

void ContextData::add_node(u32 node, usize start, usize end) {
    nodes.push_back(ParsedNode{node, start, end});
    range_start = std::min(range_start, start);
    range_end   = std::max(range_end, end);
}

Dispatcher::Dispatcher(const ParseEnv& env) : env_{&env} {
    nodes_.push_back(CommandNode{});
}

u32 Dispatcher::literal(u32 parent, std::string name, Executor command) {
    CommandNode node;
    node.kind    = CommandNode::Kind::Literal;
    node.name    = std::move(name);
    node.command = std::move(command);
    const auto index = static_cast<u32>(nodes_.size());
    nodes_.push_back(std::move(node));
    nodes_[parent].children.push_back(index);
    return index;
}

u32 Dispatcher::argument(u32 parent, std::string name, ArgumentType type, Executor command) {
    CommandNode node;
    node.kind    = CommandNode::Kind::Argument;
    node.name    = std::move(name);
    node.type    = std::move(type);
    node.command = std::move(command);
    const auto index = static_cast<u32>(nodes_.size());
    nodes_.push_back(std::move(node));
    nodes_[parent].children.push_back(index);
    return index;
}

void Dispatcher::suggests(u32 node, SuggestFn suggest, std::string wire_type) {
    nodes_[node].suggest         = std::move(suggest);
    nodes_[node].suggestion_type = std::move(wire_type);
}

std::optional<u32> Dispatcher::find_command(std::string_view name) const {
    for (const u32 child : nodes_[0].children) {
        if (nodes_[child].kind == CommandNode::Kind::Literal && nodes_[child].name == name) {
            return child;
        }
    }
    return std::nullopt;
}

std::vector<u32> Dispatcher::relevant(u32 node, const StringReader& reader) const {
    const CommandNode& parent = nodes_[node];
    bool               has_literals = false;
    for (const u32 child : parent.children) {
        has_literals = has_literals || nodes_[child].kind == CommandNode::Kind::Literal;
    }
    std::vector<u32> out;
    if (has_literals) {
        const std::string_view rest = reader.remaining();
        const std::string_view word = rest.substr(0, std::min(rest.find(' '), rest.size()));
        for (const u32 child : parent.children) {
            if (nodes_[child].kind == CommandNode::Kind::Literal && nodes_[child].name == word) {
                return {child};
            }
        }
    }
    for (const u32 child : parent.children) {
        if (nodes_[child].kind == CommandNode::Kind::Argument) {
            out.push_back(child);
        }
    }
    return out;
}

ParseResults Dispatcher::parse_nodes(u32 node, StringReader original, const ContextData& so_far,
                                     const CommandSource& source) const {
    std::vector<std::pair<u32, CommandError>> errors;
    std::vector<ParseResults>                 potentials;
    for (const u32 child : relevant(node, original)) {
        if (!can_use(child, source)) {
            continue;
        }
        ContextData        context = so_far;
        StringReader       reader  = original;
        const CommandNode& c       = nodes_[child];
        std::optional<CommandError> failure;
        if (c.kind == CommandNode::Kind::Literal) {
            const usize start  = reader.cursor();
            const usize length = c.name.size();
            if (reader.can_read(length) && reader.string().substr(start, length) == c.name &&
                (!reader.can_read(length + 1) || reader.peek(length) == ' ')) {
                reader.set_cursor(start + length);
                context.add_node(child, start, start + length);
            } else {
                failure = reader.error("argument.literal.incorrect", {Text::raw(c.name)});
            }
        } else {
            const usize start = reader.cursor();
            auto        value = parse_argument(c.type, reader, *env_);
            if (!value) {
                failure = std::move(value.error());
            } else {
                context.arguments.push_back(
                    ParsedArgument{c.name, start, reader.cursor(), std::move(*value)});
                context.add_node(child, start, reader.cursor());
            }
        }
        if (!failure && reader.can_read() && reader.peek() != ' ') {
            failure = reader.error("command.expected.separator");
        }
        if (failure) {
            errors.emplace_back(child, std::move(*failure));
            continue;
        }
        context.command = c.command ? std::optional<u32>{child} : std::nullopt;
        if (reader.can_read(c.redirect ? 1 : 2)) {
            reader.skip();
            if (c.redirect) {
                ContextData target;
                target.root        = *c.redirect;
                target.range_start = reader.cursor();
                target.range_end   = reader.cursor();
                ParseResults parsed = parse_nodes(*c.redirect, reader, target, source);
                context.child       = std::make_shared<ContextData>(std::move(parsed.context));
                return ParseResults{std::move(context), parsed.cursor, std::move(parsed.errors)};
            }
            potentials.push_back(parse_nodes(child, reader, context, source));
        } else {
            potentials.push_back(ParseResults{std::move(context), reader.cursor(), {}});
        }
    }
    if (!potentials.empty()) {
        const usize end  = original.string().size();
        const auto  rank = [end](const ParseResults& r) {
            return (r.cursor < end ? 2 : 0) + (r.errors.empty() ? 0 : 1);
        };
        std::ranges::stable_sort(potentials, [&](const ParseResults& a, const ParseResults& b) {
            return rank(a) < rank(b);
        });
        return std::move(potentials.front());
    }
    return ParseResults{so_far, original.cursor(), std::move(errors)};
}

ParseResults Dispatcher::parse(std::string_view input, usize start,
                               const CommandSource& source) const {
    ContextData root;
    root.root        = 0;
    root.range_start = start;
    root.range_end   = start;
    return parse_nodes(0, StringReader{input, start}, root, source);
}

Parsed<i32> Dispatcher::execute(const ParseResults& parse, std::string_view input,
                                const CommandSource& source) const {
    const auto at = [&](std::string key) {
        return CommandError{Text::translatable(std::move(key)), parse.cursor, std::string{input}};
    };
    if (parse.cursor < input.size()) {
        if (parse.errors.size() == 1) {
            return std::unexpected{parse.errors.front().second};
        }
        if (parse.context.range_start == parse.context.range_end) {
            return std::unexpected{at("command.unknown.command")};
        }
        return std::unexpected{at("command.unknown.argument")};
    }
    const ContextData* deepest = &parse.context;
    while (deepest->child) {
        deepest = deepest->child.get();
    }
    if (!deepest->command) {
        return std::unexpected{at("command.unknown.command")};
    }
    const CommandContext context{source, input, *deepest};
    return nodes_[*deepest->command].command(context);
}

MergedSuggestions Dispatcher::suggest(std::string_view input, const CommandSource& source,
                                      std::span<const std::string> player_names) const {
    const usize        start  = !input.empty() && input.front() == '/' ? 1 : 0;
    const ParseResults parsed = parse(input, start, source);
    const usize        cursor = input.size();

    const ContextData* context    = &parsed.context;
    u32                parent     = 0;
    usize              node_start = start;
    while (true) {
        if (context->range_start > cursor) {
            break;
        }
        if (context->range_end < cursor) {
            if (context->child) {
                context = context->child.get();
                continue;
            }
            if (!context->nodes.empty()) {
                parent     = context->nodes.back().node;
                node_start = context->nodes.back().end + 1;
            } else {
                parent     = context->root;
                node_start = context->range_start;
            }
            break;
        }
        u32  previous = context->root;
        bool found    = false;
        for (const ParsedNode& n : context->nodes) {
            if (n.start <= cursor && cursor <= n.end) {
                parent     = previous;
                node_start = n.start;
                found      = true;
                break;
            }
            previous = n.node;
        }
        if (!found) {
            parent     = previous;
            node_start = context->range_start;
        }
        break;
    }
    const usize                  begin     = std::min(node_start, cursor);
    const std::string_view       truncated = input.substr(0, cursor);
    const CommandContext         view{source, truncated, *context};
    std::vector<SuggestionEntry> entries;
    for (const u32 child : nodes_[parent].children) {
        if (!can_use(child, source)) {
            continue;
        }
        SuggestionsBuilder builder{truncated, begin};
        const CommandNode& c = nodes_[child];
        if (c.kind == CommandNode::Kind::Literal) {
            if (lower(c.name).starts_with(builder.remaining_lower())) {
                builder.suggest(c.name);
            }
        } else if (c.suggest) {
            c.suggest(view, builder);
        } else {
            suggest_argument(c.type, builder, *env_, player_names);
        }
        entries.insert(entries.end(), builder.entries().begin(), builder.entries().end());
    }
    return merge_suggestions(truncated, entries);
}

std::string Dispatcher::usage_text(u32 node) const {
    const CommandNode& n = nodes_[node];
    return n.kind == CommandNode::Kind::Argument ? "<" + n.name + ">" : n.name;
}

std::optional<std::string> Dispatcher::smart_usage(u32 node, const CommandSource& source,
                                                   bool optional, bool deep) const {
    if (!can_use(node, source)) {
        return std::nullopt;
    }
    const CommandNode& n    = nodes_[node];
    const std::string  self = optional ? "[" + usage_text(node) + "]" : usage_text(node);
    const bool         child_optional = static_cast<bool>(n.command);
    const std::string  open           = child_optional ? "[" : "(";
    const std::string  close          = child_optional ? "]" : ")";
    if (deep) {
        return self;
    }
    if (n.redirect) {
        const std::string target = *n.redirect == root() ? "..." : "-> " + usage_text(*n.redirect);
        return self + " " + target;
    }
    std::vector<u32> children;
    for (const u32 child : n.children) {
        if (can_use(child, source)) {
            children.push_back(child);
        }
    }
    if (children.size() == 1) {
        if (auto usage = smart_usage(children.front(), source, child_optional, child_optional)) {
            return self + " " + *usage;
        }
    } else if (children.size() > 1) {
        std::vector<std::string> distinct;
        for (const u32 child : children) {
            if (auto usage = smart_usage(child, source, child_optional, true)) {
                if (std::ranges::find(distinct, *usage) == distinct.end()) {
                    distinct.push_back(std::move(*usage));
                }
            }
        }
        if (distinct.size() == 1) {
            return self + " " + (child_optional ? "[" + distinct.front() + "]" : distinct.front());
        }
        if (distinct.size() > 1) {
            std::string builder = open;
            for (usize i = 0; i < children.size(); ++i) {
                builder += (i == 0 ? "" : "|") + usage_text(children[i]);
            }
            return self + " " + builder + close;
        }
    }
    return self;
}

std::vector<std::string> Dispatcher::smart_usage(u32 node, const CommandSource& source) const {
    std::vector<std::string> out;
    const bool               optional = static_cast<bool>(nodes_[node].command);
    for (const u32 child : nodes_[node].children) {
        if (auto usage = smart_usage(child, source, optional, false)) {
            out.push_back(std::move(*usage));
        }
    }
    return out;
}

net::CommandGraphWire Dispatcher::wire_graph(i32 permission) const {
    const auto usable = [&](u32 n) { return permission >= nodes_[n].permission; };
    std::vector<i32> index(nodes_.size(), -1);
    std::vector<u32> order;
    std::deque<u32>  queue{0};
    while (!queue.empty()) {
        const u32 n = queue.front();
        queue.pop_front();
        if (index[n] >= 0) {
            continue;
        }
        index[n] = static_cast<i32>(order.size());
        order.push_back(n);
        for (const u32 child : nodes_[n].children) {
            if (usable(child)) {
                queue.push_back(child);
            }
        }
        if (nodes_[n].redirect) {
            queue.push_back(*nodes_[n].redirect);
        }
    }
    net::CommandGraphWire graph;
    graph.root = 0;
    graph.nodes.reserve(order.size());
    for (const u32 n : order) {
        const CommandNode&   node = nodes_[n];
        net::CommandNodeWire wire;
        switch (node.kind) {
        case CommandNode::Kind::Root:
            wire.flags = net::command_flags::kRoot;
            break;
        case CommandNode::Kind::Literal:
            wire.flags = net::command_flags::kLiteral;
            break;
        case CommandNode::Kind::Argument:
            wire.flags = net::command_flags::kArgument;
            break;
        }
        if (node.command) {
            wire.flags |= net::command_flags::kExecutable;
        }
        if (node.redirect) {
            wire.flags |= net::command_flags::kHasRedirect;
            wire.redirect = index[*node.redirect];
        }
        for (const u32 child : node.children) {
            if (usable(child)) {
                wire.children.push_back(index[child]);
            }
        }
        if (node.kind != CommandNode::Kind::Root) {
            wire.name = node.name;
        }
        if (node.kind == CommandNode::Kind::Argument) {
            const std::string_view parser = node.type.parser_name();
            wire.parser                   = env_->parser_id(parser).value_or(-1);
            if (wire.parser < 0) {
                const auto found = std::ranges::find(kParserIds, parser);
                wire.parser      = static_cast<i32>(std::distance(kParserIds.begin(), found));
            }
            wire.properties = node.type.wire_properties();
            if (!node.suggestion_type.empty()) {
                wire.flags |= net::command_flags::kHasSuggestions;
                wire.suggestions = node.suggestion_type;
            }
        }
        graph.nodes.push_back(std::move(wire));
    }
    return graph;
}

std::vector<Text> error_lines(const CommandError& error) {
    std::vector<Text> lines;
    lines.push_back(Text::wrap(error.message).color("red"));
    if (!error.cursor) {
        return lines;
    }
    const std::string_view input  = error.input;
    const usize            cursor = std::min(*error.cursor, input.size());
    Text                   context = Text::literal("");
    context.color("gray");
    context.style.click = ClickEvent{"suggest_command", "/" + std::string{input}};
    if (cursor > 10) {
        context.append(Text::literal("..."));
    }
    const usize from = cursor > 10 ? cursor - 10 : 0;
    context.append(Text::literal(std::string{input.substr(from, cursor - from)}));
    if (cursor < input.size()) {
        Text rest = Text::literal(std::string{input.substr(cursor)});
        rest.style.underlined = true;
        rest.color("red");
        context.append(std::move(rest));
    }
    Text here = Text::translatable("command.context.here");
    here.style.italic = true;
    here.color("red");
    context.append(std::move(here));
    lines.push_back(Text::wrap(std::move(context)).color("red"));
    return lines;
}

}  // namespace ov::server::cmd
