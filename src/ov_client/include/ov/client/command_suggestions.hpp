// Command completion, from the Commands packet the server sends at login.
//
// The tree is the server's: every command this player may run, pruned by
// permission, as Brigadier nodes (root, literals, arguments, redirects). What
// the client does with it was **measured on the running 1.20.1 client**
// (scripts/chat_screen_oracle.java, docs/provenance/chat-client.md):
//
//   • "/ti" lists `time` and `title`, sorted, replacing from after the slash;
//     the first one's missing part is drawn grey after the cursor ("me");
//   • "/time set " lists day, midnight, night, noon — a position where an
//     *argument* may stand, which the client cannot complete alone: it asks
//     the server (Command Suggestions Request) and shows the answer;
//   • "/" alone lists nothing until Tab;
//   • under the list, an argument position shows its usage: "<time>",
//     "<gamemode> [<target>]".
//
// The client here completes **literals locally** and asks the server wherever
// an argument child exists. That is a superset of what vanilla asks for (it
// completes some argument types itself, and asks only for `ask_server`
// nodes); the server answers both, measured identical to the vanilla server
// on 21 of 22 probes (docs/provenance/commandes.md). Named rather than hidden.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/chat.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ov::client {

class CommandTree {
public:
    CommandTree() = default;
    explicit CommandTree(net::CommandGraphWire graph);

    [[nodiscard]] bool empty() const noexcept { return graph_.nodes.empty(); }
    [[nodiscard]] usize node_count() const noexcept { return graph_.nodes.size(); }

    /// What can complete `text` — the input up to the cursor, slash included.
    struct Completion {
        /// Byte offset in `text` where a chosen match replaces to the end.
        usize start{0};
        /// Local literal matches, sorted as vanilla lists them.
        std::vector<std::string> matches;
        /// An argument could stand at the cursor: the server must be asked,
        /// and its answer replaces `matches`.
        bool ask_server{false};
        /// The text is not a command at all (no slash): nothing to complete.
        bool command{false};
    };

    [[nodiscard]] Completion complete(std::string_view text) const;

    /// The usage lines an argument position shows, and the byte offset in
    /// `text` they are drawn from. Empty at a literal-only position.
    struct Usage {
        usize                    start{0};
        std::vector<std::string> lines;
    };

    [[nodiscard]] Usage usage(std::string_view text) const;

    /// The colour of each part of a typed command, as vanilla highlights it:
    /// literals grey, arguments in turn aqua, yellow, green, light purple,
    /// gold, and what does not parse red. Byte ranges of `text`.
    struct Span {
        usize from{0};
        usize to{0};
        u32   rgb{0};
    };

    [[nodiscard]] std::vector<Span> highlight(std::string_view text) const;

private:
    struct Walk {
        /// The node whose children the next token is matched against.
        i32 node{-1};
        /// Where that next token starts.
        usize token_start{0};
        /// A complete token matched no literal and the node has arguments:
        /// the walk cannot know how many tokens the argument consumes.
        bool lost_in_argument{false};
        /// A complete token matched nothing at all.
        bool failed{false};
        /// Where each walked token was, and whether it was a literal.
        std::vector<Span> spans;
    };

    /// Walk the complete tokens of `text` (everything before the last space).
    [[nodiscard]] Walk walk(std::string_view text) const;

    /// The node whose children apply at `node`: itself, or its redirect.
    [[nodiscard]] i32 children_of(i32 node) const noexcept;

    [[nodiscard]] bool has_argument_child(i32 node) const noexcept;

    [[nodiscard]] std::string token(i32 node) const;

    net::CommandGraphWire graph_;
};

}  // namespace ov::client
