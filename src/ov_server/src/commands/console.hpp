// The dedicated server's console: lines typed on stdin run as commands at
// level 4, without the slash, as vanilla's do.
//
// A thread of its own because reading stdin blocks. It never touches the
// world: it hands each line to a callback that queues it, and the tick thread
// runs the queue. On POSIX the read is a poll with a short timeout, so the
// thread notices the server stopping and can be joined; stdin closing (a
// server run under a script, or with `< /dev/null`) simply ends it.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace ov::server::cmd {

class ConsoleReader {
public:
    explicit ConsoleReader(std::function<void(std::string)> on_line);
    ~ConsoleReader();

    ConsoleReader(const ConsoleReader&)            = delete;
    ConsoleReader& operator=(const ConsoleReader&) = delete;

private:
    void run();

    std::function<void(std::string)> on_line_;
    std::atomic<bool>                stop_{false};
    std::thread                      thread_;
};

}  // namespace ov::server::cmd
