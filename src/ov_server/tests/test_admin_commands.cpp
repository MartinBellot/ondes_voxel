// The administration commands, run against a fake host with a real
// `ServerAdmin`: what they change in the lists, whom they kick, which
// server.properties keys they write, and how they refuse.
//
// The exact words are the jar's (checked end to end by
// scripts/check_admin.py); here the translation keys and the state are
// pinned, which is what a refactor would break.
#include "../src/admin/server_admin.hpp"
#include "../src/commands/service.hpp"

#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::server;
using namespace ov::server::cmd;

namespace {

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

const Packs& packs() {
    static const Packs state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Packs out;
        if (auto b = registry::BlockRegistry::load(path)) {
            out.blocks = std::move(*b);
        }
        if (auto r = registry::Registries::load(path)) {
            out.registries = std::move(*r);
        }
        return out;
    }();
    return state;
}

/// One online player, "Alice", at 10.0.0.7; the console runs the commands.
struct Harness {
    admin::ServerAdmin                  admin;
    CommandService                      service;
    std::vector<std::string>            console;
    std::vector<std::pair<i32, std::string>> kicks;
    std::map<std::string, std::string>  properties;
    std::string                         alice_name{"Alice"};
    net::Uuid                           alice{net::Uuid::offline_player("Alice")};
    i32                                 alice_permission{0};
    u8                                  alice_mode{0};
    f64                                 x{0}, y{0}, z{0};
    f32                                 yaw{0}, pitch{0};
    CommandHost                         host;

    static admin::AdminConfig admin_config() {
        admin::AdminConfig c;
        c.clock = admin::WallClock::fixed(1'800'000'000, 0, "UTC");
        return c;
    }
    static ServiceConfig service_config(admin::ServerAdmin& a) {
        ServiceConfig c;
        c.blocks     = packs().blocks ? &*packs().blocks : nullptr;
        c.registries = packs().registries ? &*packs().registries : nullptr;
        c.admin      = &a;
        return c;
    }

    Harness() : admin{admin_config()}, service{service_config(admin)} {
        service.console = [this](std::string_view line) { console.emplace_back(line); };
        host.for_each_player = [this](const std::function<void(PlayerRef&)>& visit) {
            PlayerRef ref;
            ref.entity_id  = 1;
            ref.name       = alice_name;
            ref.uuid       = alice;
            ref.x          = &x;
            ref.y          = &y;
            ref.z          = &z;
            ref.yaw        = &yaw;
            ref.pitch      = &pitch;
            ref.game_mode  = &alice_mode;
            ref.permission = &alice_permission;
            ref.send       = [](i32, std::span<const u8>) {};
            ref.broadcast_others = [](i32, std::span<const u8>) {};
            ref.address    = "10.0.0.7";
            visit(ref);
        };
        host.entities     = [](std::vector<EntityInfo>&) {};
        host.broadcast    = [](i32, std::span<const u8>) {};
        host.kick         = [this](i32 id, std::string_view reason) { kicks.emplace_back(id, reason); };
        host.set_property = [this](std::string_view key, std::string value) {
            properties[std::string{key}] = std::move(value);
        };
        host.tick_count = [] { return i64{0}; };
        service.run(host);  // greets Alice
        console.clear();
    }

    std::vector<std::string> run(std::string_view command) {
        console.clear();
        (void)service.execute(CommandSource{}, command, host);
        return console;
    }
};

bool have_packs() { return packs().blocks.has_value() && packs().registries.has_value(); }

bool mentions(const std::vector<std::string>& lines, std::string_view needle) {
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("ban and pardon: the list, the kick, the refusals", "[admin][commands]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    CHECK(mentions(h.run("banlist"), "commands.banlist.none"));
    (void)h.run("ban Alice Being Alice");
    REQUIRE(h.kicks.size() == 1);
    CHECK(h.kicks[0].second == R"({"translate":"multiplayer.disconnect.banned"})");
    const std::string key = h.alice.to_string();
    h.admin.with([&](admin::ServerAdmin::Lists& lists) {
        const admin::BanEntry* ban = lists.players.get(key);
        REQUIRE(ban != nullptr);
        CHECK(ban->reason == "Being Alice");
        CHECK(ban->source == "Server");
        CHECK(ban->name == "Alice");
    });
    CHECK(mentions(h.run("ban Alice"), "commands.ban.failed"));
    CHECK(mentions(h.run("banlist players"), "commands.banlist.list"));
    CHECK(h.admin.login_refusal(h.alice, "1.2.3.4", 0).has_value());
    (void)h.run("pardon Alice");
    CHECK_FALSE(h.admin.login_refusal(h.alice, "1.2.3.4", 0).has_value());
    CHECK(mentions(h.run("pardon Alice"), "commands.pardon.failed"));
}

TEST_CASE("ban-ip by address and by name, pardon-ip", "[admin][commands]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    CHECK(mentions(h.run("ban-ip not.an.ip"), "commands.banip.invalid"));
    (void)h.run("ban-ip 10.0.0.9 Spam");
    CHECK(h.kicks.empty());
    CHECK(mentions(h.run("ban-ip 10.0.0.9"), "commands.banip.failed"));
    // By name: the online player's own address, and they are kicked.
    (void)h.run("ban-ip Alice");
    REQUIRE(h.kicks.size() == 1);
    CHECK(h.kicks[0].second == R"({"translate":"multiplayer.disconnect.ip_banned"})");
    CHECK(h.admin.login_refusal(net::Uuid::offline_player("Bob"), "10.0.0.7", 0).has_value());
    CHECK(mentions(h.run("pardon-ip bad"), "commands.pardonip.invalid"));
    (void)h.run("pardon-ip 10.0.0.7");
    CHECK_FALSE(h.admin.login_refusal(net::Uuid::offline_player("Bob"), "10.0.0.7", 0).has_value());
    CHECK(mentions(h.run("pardon-ip 10.0.0.7"), "commands.pardonip.failed"));
}

TEST_CASE("whitelist: on/off writes white-list, add/remove/list", "[admin][commands]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    CHECK(mentions(h.run("whitelist list"), "commands.whitelist.none"));
    (void)h.run("whitelist on");
    CHECK(h.properties["white-list"] == "true");
    CHECK(mentions(h.run("whitelist on"), "commands.whitelist.alreadyOn"));
    // Not enforced (enforce-whitelist=false): Alice stays.
    CHECK(h.kicks.empty());
    CHECK(h.admin.login_refusal(net::Uuid::offline_player("Carol"), "1.2.3.4", 0).has_value());
    (void)h.run("whitelist add Carol");
    // Nobody called Carol is online: the jar's offline profile of the name in
    // lower case goes on the list — so "carol" gets in and "Carol", whose
    // uuid is another, still does not (measured on the real server with ban).
    CHECK_FALSE(h.admin.login_refusal(net::Uuid::offline_player("carol"), "1.2.3.4", 0).has_value());
    CHECK(h.admin.login_refusal(net::Uuid::offline_player("Carol"), "1.2.3.4", 0).has_value());
    CHECK(mentions(h.run("whitelist add Carol"), "commands.whitelist.add.failed"));
    CHECK(mentions(h.run("whitelist list"), "commands.whitelist.list"));
    (void)h.run("whitelist remove Carol");
    CHECK(mentions(h.run("whitelist remove Carol"), "commands.whitelist.remove.failed"));
    (void)h.run("whitelist off");
    CHECK(h.properties["white-list"] == "false");
    // Enforced: turning it on sends away whoever is not on it.
    h.admin.with([](admin::ServerAdmin::Lists& lists) { lists.enforce_whitelist = true; });
    (void)h.run("whitelist on");
    REQUIRE(h.kicks.size() == 1);
    CHECK(h.kicks[0].second == R"({"translate":"multiplayer.disconnect.not_whitelisted"})");
}

TEST_CASE("save-off, save-on, setidletimeout, debug", "[admin][commands]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    CHECK(h.service.autosave_enabled());
    CHECK(mentions(h.run("save-on"), "commands.save.alreadyOn"));
    (void)h.run("save-off");
    CHECK_FALSE(h.service.autosave_enabled());
    CHECK(mentions(h.run("save-off"), "commands.save.alreadyOff"));
    (void)h.run("save-on");
    CHECK(h.service.autosave_enabled());
    (void)h.run("setidletimeout 7");
    CHECK(h.service.idle_timeout() == 7);
    CHECK(h.properties["player-idle-timeout"] == "7");
    CHECK(mentions(h.run("debug stop"), "commands.debug.notRunning"));
    (void)h.run("debug start");
    CHECK(mentions(h.run("debug start"), "commands.debug.alreadyRunning"));
    CHECK(mentions(h.run("debug stop"), "commands.debug.stopped"));
}

TEST_CASE("RCON's words are collected, not printed", "[admin][commands]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness     h;
    std::string words;
    bool        done = false;
    h.service.enqueue_captured("banlist", "Rcon", [&](std::string w) {
        words = std::move(w);
        done  = true;
    });
    h.console.clear();
    h.service.run(h.host);
    REQUIRE(done);
    CHECK(words.find("commands.banlist.none") != std::string::npos);
    CHECK(h.console.empty());
}

TEST_CASE("the ban commands exist only where there are ban lists", "[admin][commands]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    ServiceConfig config;
    config.blocks     = packs().blocks ? &*packs().blocks : nullptr;
    config.registries = packs().registries ? &*packs().registries : nullptr;
    CommandService           service{std::move(config)};
    std::vector<std::string> lines;
    service.console = [&](std::string_view line) { lines.emplace_back(line); };
    CommandHost host;
    host.for_each_player = [](const std::function<void(PlayerRef&)>&) {};
    host.entities        = [](std::vector<EntityInfo>&) {};
    (void)service.execute(CommandSource{}, "banlist", host);
    CHECK(mentions(lines, "command.unknown.command"));
}
