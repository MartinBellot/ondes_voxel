#include "ov/client/command_suggestions.hpp"

#include <algorithm>
#include <array>

namespace ov::client {

namespace {

namespace flags = net::command_flags;

// The ChatFormatting colours the highlight uses.
constexpr u32 kGray = 0xAAAAAA;
constexpr u32 kRed  = 0xFF5555;
constexpr std::array<u32, 5> kArgumentColours{0x55FFFF, 0xFFFF55, 0x55FF55, 0xFF55FF, 0xFFAA00};

[[nodiscard]] char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool starts_with_folded(std::string_view text, std::string_view prefix) noexcept {
    if (prefix.size() > text.size()) {
        return false;
    }
    for (usize i = 0; i < prefix.size(); ++i) {
        if (lower(text[i]) != lower(prefix[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool less_folded(const std::string& a, const std::string& b) noexcept {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return lower(x) < lower(y); });
}

}  // namespace

CommandTree::CommandTree(net::CommandGraphWire graph) : graph_(std::move(graph)) {}

i32 CommandTree::children_of(i32 node) const noexcept {
    if (node < 0 || static_cast<usize>(node) >= graph_.nodes.size()) {
        return -1;
    }
    const net::CommandNodeWire& wire = graph_.nodes[static_cast<usize>(node)];
    if ((wire.flags & flags::kHasRedirect) != 0 && wire.redirect >= 0 &&
        static_cast<usize>(wire.redirect) < graph_.nodes.size()) {
        return wire.redirect;
    }
    return node;
}

bool CommandTree::has_argument_child(i32 node) const noexcept {
    const i32 parent = children_of(node);
    if (parent < 0) {
        return false;
    }
    for (const i32 child : graph_.nodes[static_cast<usize>(parent)].children) {
        if ((graph_.nodes[static_cast<usize>(child)].flags & flags::kTypeMask) == flags::kArgument) {
            return true;
        }
    }
    return false;
}

std::string CommandTree::token(i32 node) const {
    const net::CommandNodeWire& wire = graph_.nodes[static_cast<usize>(node)];
    if ((wire.flags & flags::kTypeMask) == flags::kArgument) {
        return "<" + wire.name + ">";
    }
    return wire.name;
}

CommandTree::Walk CommandTree::walk(std::string_view text) const {
    Walk w;
    if (graph_.nodes.empty() || text.empty() || text.front() != '/') {
        return w;
    }
    w.node        = graph_.root;
    usize pos     = 1;
    for (;;) {
        const usize space = text.find(' ', pos);
        if (space == std::string_view::npos) {
            break;
        }
        const std::string_view word   = text.substr(pos, space - pos);
        const i32              parent = children_of(w.node);
        i32                    next   = -1;
        for (const i32 child : graph_.nodes[static_cast<usize>(parent)].children) {
            const net::CommandNodeWire& wire = graph_.nodes[static_cast<usize>(child)];
            if ((wire.flags & flags::kTypeMask) == flags::kLiteral && wire.name == word) {
                next = child;
                break;
            }
        }
        if (next < 0) {
            w.token_start = pos;
            if (has_argument_child(w.node)) {
                w.lost_in_argument = true;
            } else {
                w.failed = true;
            }
            return w;
        }
        w.spans.push_back(Span{pos, space, kGray});
        w.node = next;
        pos    = space + 1;
    }
    w.token_start = pos;
    return w;
}

CommandTree::Completion CommandTree::complete(std::string_view text) const {
    Completion out;
    if (text.empty() || text.front() != '/') {
        return out;
    }
    out.command = true;
    const Walk w = walk(text);
    if (w.node < 0 || w.failed) {
        return out;
    }
    out.start = w.token_start;
    if (w.lost_in_argument) {
        out.ask_server = true;
        return out;
    }
    const std::string_view partial = text.substr(w.token_start);
    const i32              parent  = children_of(w.node);
    for (const i32 child : graph_.nodes[static_cast<usize>(parent)].children) {
        const net::CommandNodeWire& wire = graph_.nodes[static_cast<usize>(child)];
        if ((wire.flags & flags::kTypeMask) == flags::kLiteral &&
            starts_with_folded(wire.name, partial)) {
            out.matches.push_back(wire.name);
        }
    }
    std::ranges::sort(out.matches, less_folded);
    out.ask_server = has_argument_child(w.node);
    return out;
}

CommandTree::Usage CommandTree::usage(std::string_view text) const {
    Usage out;
    const Walk w = walk(text);
    if (w.node < 0 || w.failed || w.lost_in_argument) {
        return out;
    }
    out.start          = w.token_start;
    const i32 parent   = children_of(w.node);
    for (const i32 child : graph_.nodes[static_cast<usize>(parent)].children) {
        const net::CommandNodeWire& wire = graph_.nodes[static_cast<usize>(child)];
        if ((wire.flags & flags::kTypeMask) != flags::kArgument) {
            continue;
        }
        std::string line = token(child);
        // One level of what follows, as the measured "<gamemode> [<target>]":
        // optional (in brackets) when the argument is already a whole command,
        // alternatives in parentheses when there are several.
        const i32  after = children_of(child);
        const auto& kids = graph_.nodes[static_cast<usize>(after)].children;
        if (!kids.empty()) {
            std::string tail;
            if (kids.size() == 1) {
                tail = token(kids.front());
            } else {
                tail = "(";
                for (usize i = 0; i < kids.size(); ++i) {
                    tail += (i == 0 ? "" : "|") + token(kids[i]);
                }
                tail += ")";
            }
            const bool executable = (wire.flags & flags::kExecutable) != 0;
            line += executable ? " [" + tail + "]" : " " + tail;
        }
        out.lines.push_back(std::move(line));
    }
    return out;
}

std::vector<CommandTree::Span> CommandTree::highlight(std::string_view text) const {
    std::vector<Span> out;
    if (text.empty() || text.front() != '/') {
        return out;
    }
    const Walk w = walk(text);
    if (w.node < 0) {
        return out;
    }
    out = w.spans;
    if (w.failed) {
        out.push_back(Span{w.token_start, text.size(), kRed});
        return out;
    }
    if (w.lost_in_argument) {
        // The arguments cannot be parsed here: each word takes the next
        // argument colour, which is what the common one-word arguments get.
        usize colour = 0;
        usize pos    = w.token_start;
        while (pos < text.size()) {
            usize end = text.find(' ', pos);
            end       = end == std::string_view::npos ? text.size() : end;
            if (end > pos) {
                out.push_back(Span{pos, end, kArgumentColours[colour % kArgumentColours.size()]});
                ++colour;
            }
            pos = end + 1;
        }
        return out;
    }
    // The word being typed: grey when it is already a whole literal, red
    // otherwise — measured, "/ti" draws "ti" red.
    const std::string_view partial = text.substr(w.token_start);
    if (!partial.empty()) {
        bool whole = false;
        for (const i32 child : graph_.nodes[static_cast<usize>(children_of(w.node))].children) {
            const net::CommandNodeWire& wire = graph_.nodes[static_cast<usize>(child)];
            whole = whole || ((wire.flags & flags::kTypeMask) == flags::kLiteral && wire.name == partial);
        }
        out.push_back(Span{w.token_start, text.size(),
                           whole ? kGray : (has_argument_child(w.node) ? kArgumentColours[0] : kRed)});
    }
    return out;
}

}  // namespace ov::client
