// Who may come in: the ban lists, the whitelist, the player limit — and the
// words a refused player is shown.
//
// Two threads need this and it is not the world, so it carries its own lock:
// the network thread asks `login_refusal` for every Login Start, and the tick
// thread changes the lists when a command runs. Everything else in the server
// keeps its no-mutex rule; this object is the administration's, and no chunk
// or entity is ever reached through it.
//
// Vanilla's order of refusals (PlayerList.canPlayerLogin, re-specified from
// its answers, docs/provenance/serveur-dedie.md): a banned profile, then the
// whitelist, then a banned address, then a full server.
#pragma once

#include "user_lists.hpp"

#include "ov/base/types.hpp"
#include "ov/protocol/types.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server::admin {

/// An operator as the door sees it: whitelisted by being an operator, and
/// perhaps allowed past a full server.
struct OpPass {
    net::Uuid uuid{};
    bool      bypasses_player_limit{false};
};

struct AdminConfig {
    /// Where the three list files live; empty keeps everything in memory.
    std::filesystem::path directory;
    WallClock             clock{WallClock::system()};
    bool                  white_list{false};
    bool                  enforce_whitelist{false};
    i32                   max_players{20};
};

class ServerAdmin {
public:
    explicit ServerAdmin(AdminConfig config);

    /// Read the files. Returns one line per file that could not be read.
    [[nodiscard]] std::vector<std::string> load();

    /// The refusal a player is shown, as a text component in JSON, or none.
    /// `address` is the bare IP, no port.
    [[nodiscard]] std::optional<std::string> login_refusal(const net::Uuid& uuid,
                                                           std::string_view address,
                                                           i32              online_players);

    /// Anything below runs with the lock held; `fn` gets the lists.
    struct Lists {
        BanList&   players;
        BanList&   ips;
        WhiteList& whitelist;
        bool&      white_list_enabled;
        bool&      enforce_whitelist;
    };
    template <typename Fn>
    decltype(auto) with(Fn&& fn) {
        const std::scoped_lock lock{mutex_};
        Lists                  lists{players_, ips_, whitelist_, white_list_, enforce_whitelist_};
        return fn(lists);
    }

    /// The operators, as the command engine's list says. Called whenever it
    /// changes.
    void set_ops(std::vector<OpPass> ops);
    [[nodiscard]] bool is_whitelisted(const net::Uuid& uuid);
    /// Spawn protection only exists while somebody is an operator.
    [[nodiscard]] bool has_ops() {
        const std::scoped_lock lock{mutex_};
        return !ops_.empty();
    }

    void set_max_players(i32 max) {
        const std::scoped_lock lock{mutex_};
        max_players_ = max;
    }
    [[nodiscard]] const WallClock& clock() const noexcept { return clock_; }

    /// The JSON of a ban's refusal, as vanilla words it.
    [[nodiscard]] std::string ban_refusal(const BanEntry& entry, bool ip) const;

private:
    [[nodiscard]] bool whitelisted_locked(const net::Uuid& uuid) const;

    std::mutex          mutex_;
    WallClock           clock_;
    BanList             players_;
    BanList             ips_;
    WhiteList           whitelist_;
    std::vector<OpPass> ops_;
    bool                white_list_{false};
    bool                enforce_whitelist_{false};
    i32                 max_players_{20};
};

/// "1.2.3.4:5678" or "[::1]:5678" → the address alone.
[[nodiscard]] std::string address_without_port(std::string_view peer);

/// InetAddresses.isInetAddress, for what `ban-ip` accepts as an address.
[[nodiscard]] bool is_ip_address(std::string_view text);

}  // namespace ov::server::admin
