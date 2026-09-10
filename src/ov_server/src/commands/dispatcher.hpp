// The command tree: Brigadier's model, re-specified from what it does.
//
// Nodes are literals and arguments under one root, each with an optional
// executor, a permission level and an optional redirect (`tp` is a redirect to
// `teleport`, `xp` to `experience`). The parse is Brigadier's:
//
//   * at each node, if the next word is one of its literals, only that literal
//     is tried; otherwise only its *arguments* are;
//   * every child that parses is followed recursively, and of all the
//     branches that got anywhere, the first that consumed the whole input
//     wins, then the first with no error — in child order;
//   * a failure is reported as the single error of the only branch tried, or
//     as "unknown command" if nothing parsed at all, or "incorrect argument"
//     at the furthest point reached.
//
// None of this is a guess. `tp @s 0 -60 0 foo` answers *incorrect argument at
// column 6* — not at the `foo` — and only this rule produces that: the
// `destination` branch (`@s` taken as the destination) wins because it has no
// error, and it stopped at 6. The capture records it.
//
// The graph is sent to a client as the Commands packet, pruned to the nodes
// its permission level can use and numbered breadth-first from the root, as
// vanilla numbers it.
#pragma once

#include "arguments.hpp"
#include "context.hpp"
#include "string_reader.hpp"
#include "suggestions.hpp"

#include "ov/protocol/chat.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::server::cmd {

class CommandContext;

/// Runs a command. The result is vanilla's "success count"; an error is a
/// failure the player sees in red.
using Executor = std::function<Parsed<i32>(const CommandContext&)>;

/// Custom completions for one argument node.
using SuggestFn = std::function<void(const CommandContext&, SuggestionsBuilder&)>;

struct CommandNode {
    enum class Kind : u8 { Root, Literal, Argument };

    Kind                 kind{Kind::Root};
    std::string          name;
    ArgumentType         type;
    std::vector<u32>     children;
    std::optional<u32>   redirect;
    Executor             command;
    i32                  permission{0};
    SuggestFn            suggest;
    /// What the Commands packet names as this argument's suggestion source:
    /// "minecraft:ask_server", "minecraft:summonable_entities", or nothing.
    std::string          suggestion_type;
};

struct ParsedArgument {
    std::string name;
    usize       start{0};
    usize       end{0};
    ArgValue    value;
};

struct ParsedNode {
    u32   node{0};
    usize start{0};
    usize end{0};
};

/// What a parse built: the nodes taken, the arguments read, the command at the
/// end, and — across a redirect — the context of the target.
struct ContextData {
    u32                          root{0};
    usize                        range_start{0};
    usize                        range_end{0};
    std::vector<ParsedNode>      nodes;
    std::vector<ParsedArgument>  arguments;
    std::optional<u32>           command;
    std::shared_ptr<ContextData> child;

    void add_node(u32 node, usize start, usize end);
};

class CommandContext {
public:
    CommandContext(const CommandSource& source, std::string_view input, const ContextData& data)
        : source_{&source}, input_{input}, data_{&data} {}

    [[nodiscard]] const CommandSource& source() const noexcept { return *source_; }
    [[nodiscard]] std::string_view     input() const noexcept { return input_; }

    template<typename T>
    [[nodiscard]] const T* find(std::string_view name) const noexcept {
        for (const ParsedArgument& argument : data_->arguments) {
            if (argument.name == name) {
                return std::get_if<T>(&argument.value);
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool has(std::string_view name) const noexcept {
        for (const ParsedArgument& argument : data_->arguments) {
            if (argument.name == name) {
                return true;
            }
        }
        return false;
    }

    /// The literal that was taken at a depth of the chain, e.g. which of
    /// `day`/`noon`/… came after `time set`.
    [[nodiscard]] const ContextData& data() const noexcept { return *data_; }

private:
    const CommandSource* source_;
    std::string_view     input_;
    const ContextData*   data_;
};

struct ParseResults {
    ContextData                                context;
    usize                                      cursor{0};
    std::vector<std::pair<u32, CommandError>>  errors;
};

class Dispatcher {
public:
    explicit Dispatcher(const ParseEnv& env);

    [[nodiscard]] u32 root() const noexcept { return 0; }
    [[nodiscard]] const CommandNode& node(u32 index) const { return nodes_[index]; }
    [[nodiscard]] usize node_count() const noexcept { return nodes_.size(); }

    u32 literal(u32 parent, std::string name, Executor command = {});
    u32 argument(u32 parent, std::string name, ArgumentType type, Executor command = {});
    void redirect(u32 node, u32 target) { nodes_[node].redirect = target; }
    void executes(u32 node, Executor command) { nodes_[node].command = std::move(command); }
    void requires_permission(u32 node, i32 level) { nodes_[node].permission = level; }
    void suggests(u32 node, SuggestFn suggest, std::string wire_type);

    /// The top-level literal of that name, if any.
    [[nodiscard]] std::optional<u32> find_command(std::string_view name) const;

    /// Parse `input` from `start` (1 to skip a leading slash).
    [[nodiscard]] ParseResults parse(std::string_view input, usize start,
                                     const CommandSource& source) const;

    /// Run a parse. Every failure — syntax or execution — comes back as the
    /// error the player sees.
    [[nodiscard]] Parsed<i32> execute(const ParseResults& parse, std::string_view input,
                                      const CommandSource& source) const;

    /// Completions for the whole of `input`.
    [[nodiscard]] MergedSuggestions suggest(std::string_view input, const CommandSource& source,
                                            std::span<const std::string> player_names) const;

    /// Brigadier's "smart usage" of every child of `node`, as `help` prints it.
    [[nodiscard]] std::vector<std::string> smart_usage(u32 node, const CommandSource& source) const;

    /// The graph a client with this permission level may see.
    [[nodiscard]] net::CommandGraphWire wire_graph(i32 permission) const;

    [[nodiscard]] bool can_use(u32 node, const CommandSource& source) const noexcept {
        return source.permission >= nodes_[node].permission;
    }

private:
    [[nodiscard]] ParseResults parse_nodes(u32 node, StringReader reader, const ContextData& so_far,
                                           const CommandSource& source) const;
    [[nodiscard]] std::vector<u32> relevant(u32 node, const StringReader& reader) const;
    [[nodiscard]] std::optional<std::string> smart_usage(u32 node, const CommandSource& source,
                                                         bool optional, bool deep) const;
    [[nodiscard]] std::string usage_text(u32 node) const;

    const ParseEnv*          env_;
    std::vector<CommandNode> nodes_;
};

/// `minecraft:command_argument_type` in id order — the fallback when no
/// registry pack is loaded, and what the tests hold against the report.
[[nodiscard]] std::span<const std::string_view> parser_id_table() noexcept;

/// The two lines vanilla prints for a failed command: the error in red, and —
/// when the error has a place — the input up to it, the rest underlined, and
/// `<--[HERE]`. `command` is the input without its slash.
[[nodiscard]] std::vector<Text> error_lines(const CommandError& error);

}  // namespace ov::server::cmd
