// The server, as something the client can host.
//
// It was an application: two thousand lines inside main(). That was right while
// the only way to run it was from a terminal, and wrong the moment the client
// needed one of its own — because "the solo game is multiplayer" means the
// client hosts a real server and talks to it over a real socket, not that it
// reaches into a world object.
//
// So the whole thing moved here unchanged and main() became three lines. The
// only addition is a way to stop it that is not a signal: a dedicated server
// exits on SIGINT, an integrated one exits when the window closes.
#pragma once

#include "ov/base/types.hpp"

#include <atomic>

namespace ov::server {

/// Run until stopped. Returns the process exit code.
///
/// `external_stop` is polled by the tick loop alongside the signal flag. When
/// it is null the server installs SIGINT and SIGTERM handlers and behaves as
/// the dedicated binary always has; when it is not, it leaves signals alone —
/// a client's window has its own idea of when to quit, and two handlers
/// fighting over one process is a bug nobody enjoys finding.
[[nodiscard]] int run(int argc, char** argv, const std::atomic<bool>* external_stop = nullptr);

}  // namespace ov::server
