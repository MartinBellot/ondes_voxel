// The dedicated server's console: lines typed on stdin run as commands at
// level 4, without the slash, as vanilla's do.
//
// A thread of its own because reading stdin blocks. It never touches the
// world: it hands each line to a callback that queues it, and the tick thread
// runs the queue. On POSIX the read is a poll with a short timeout, so the
// thread notices the server stopping and can be joined; stdin closing (a
// server run under a script, or with `< /dev/null`) simply ends it.
//
// ── dedicated server administration ── Completion. When stdin and stdout are
// both a terminal and a completer is given, the line is edited here, in the
// terminal's raw mode: Tab asks the command engine what may follow — the same
// suggestions a player's client gets — and completes the word, or lists the
// choices. Anything else (a pipe, a script) reads lines exactly as before.
// Vanilla 1.20.1's own console has no completion; this is an addition, and
// what it prints to the terminal is nothing a command answers.
#pragma once

#include "ov/base/types.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ov::server::cmd {

/// What the engine proposes for a line: the range of the line it replaces,
/// and the words that may go there.
struct ConsoleSuggestions {
    usize                    start{0};
    usize                    length{0};
    std::vector<std::string> matches;
};

/// What Tab does to a line: one match replaces the range; several replace it
/// with their longest common prefix and are listed.
struct CompletedLine {
    std::string              line;
    std::vector<std::string> listing;
};
[[nodiscard]] CompletedLine complete_line(std::string_view line, const ConsoleSuggestions& s);

class ConsoleReader {
public:
    using Completer = std::function<ConsoleSuggestions(std::string_view line)>;

    explicit ConsoleReader(std::function<void(std::string)> on_line, Completer complete = {});
    ~ConsoleReader();

    ConsoleReader(const ConsoleReader&)            = delete;
    ConsoleReader& operator=(const ConsoleReader&) = delete;

private:
    void run();
    void run_terminal();

    std::function<void(std::string)> on_line_;
    Completer                        complete_;
    std::atomic<bool>                stop_{false};
    std::thread                      thread_;
};

}  // namespace ov::server::cmd
