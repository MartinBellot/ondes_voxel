// The scoreboard, against the real server's answers.
//
// Every JSON string and every hexadecimal payload below that is not built by
// the test itself is **a capture**: what a real 1.20.1 server sent a probe
// client for the same command, in the same state
// (scripts/capture_scoreboard.py). The commands run against a fake server of
// one or two players, and the lines and packets they produce are compared.
//
// The rest pins what the capture cannot: the file's round trip (unknown keys
// kept), the criteria, and the friendly-fire and collision rules.
#include "../src/commands/service.hpp"
#include "../src/scoreboard/scoreboard.hpp"

#include "ov/nbt/binary.hpp"
#include "ov/protocol/chat.hpp"
#include "ov/protocol/scoreboard_packets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::server;
using namespace ov::server::cmd;

namespace {

namespace sb = net::scoreboard;

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

std::string hex(std::span<const u8> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out;
    for (const u8 b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

struct FakePlayer {
    std::array<net::ItemStack, 46> inventory{};
    net::ItemStack                 carried{};
    SurvivalSession                survival{};
    EffectSession                  effects{};
    f64                            x{0.5};
    f64                            y{-60.0};
    f64                            z{0.5};
    f32                            yaw{0.0F};
    f32                            pitch{0.0F};
    u8                             game_mode{1};
    i32                            permission{4};
    i32                            id{1};
    std::string                    name;
    net::Uuid                      uuid{};
    std::vector<std::pair<i32, std::vector<u8>>> sent;
};

/// A server of a few players on nothing, recording what it is told.
struct Harness {
    std::vector<std::unique_ptr<FakePlayer>>     players;
    std::vector<std::pair<i32, std::vector<u8>>> broadcast;
    CommandService                               service;
    CommandHost                                  host;

    Harness()
        : service{ServiceConfig{packs().blocks ? &*packs().blocks : nullptr,
                                packs().registries ? &*packs().registries : nullptr,
                                {}, {}, false, 4, "Ondes VOXEL", {}}} {
        add_player("ovprobe", 4);
        host.for_each_player = [this](const std::function<void(PlayerRef&)>& visit) {
            for (auto& fp : players) {
                PlayerRef p;
                p.entity_id     = fp->id;
                p.name          = fp->name;
                p.uuid          = fp->uuid;
                p.x             = &fp->x;
                p.y             = &fp->y;
                p.z             = &fp->z;
                p.yaw           = &fp->yaw;
                p.pitch         = &fp->pitch;
                p.game_mode     = &fp->game_mode;
                p.permission    = &fp->permission;
                p.inventory     = &fp->inventory;
                p.carried       = &fp->carried;
                p.survival      = &fp->survival;
                p.effects       = &fp->effects;
                FakePlayer* raw = fp.get();
                p.send          = [raw](i32 id, std::span<const u8> payload) {
                    raw->sent.emplace_back(id, std::vector<u8>(payload.begin(), payload.end()));
                };
                p.broadcast_others = [](i32, std::span<const u8>) {};
                visit(p);
            }
        };
        host.entities  = [](std::vector<EntityInfo>&) {};
        host.broadcast = [this](i32 id, std::span<const u8> payload) {
            broadcast.emplace_back(id, std::vector<u8>(payload.begin(), payload.end()));
            for (auto& fp : players) {
                fp->sent.emplace_back(id, std::vector<u8>(payload.begin(), payload.end()));
            }
        };
    }

    FakePlayer& add_player(std::string name, i32 permission) {
        auto fp        = std::make_unique<FakePlayer>();
        fp->id         = static_cast<i32>(players.size()) + 1;
        fp->name       = name;
        fp->uuid       = net::Uuid::offline_player(name);
        fp->permission = permission;
        players.push_back(std::move(fp));
        return *players.back();
    }

    CommandSource source(usize who = 0) const {
        const FakePlayer& fp = *players[who];
        CommandSource     s;
        s.kind       = CommandSource::Kind::Player;
        s.entity_id  = fp.id;
        s.name       = fp.name;
        s.uuid       = fp.uuid;
        s.position   = Vec3d{fp.x, fp.y, fp.z};
        s.permission = fp.permission;
        return s;
    }

    /// Run as player `who`; its System Chat lines, as JSON.
    std::vector<std::string> run(std::string_view command, usize who = 0) {
        broadcast.clear();
        for (auto& fp : players) {
            fp->sent.clear();
        }
        (void)service.execute(source(who), command, host);
        return chat(who);
    }

    std::vector<std::string> chat(usize who = 0) const {
        std::vector<std::string> out;
        for (const auto& [id, payload] : players[who]->sent) {
            if (id == net::clientbound::kSystemChat) {
                if (const auto parsed = net::parse_system_chat(payload)) {
                    out.push_back(parsed->content);
                }
            }
        }
        return out;
    }

    /// The scoreboard packets everyone was sent, as hex.
    std::vector<std::string> board() const {
        std::vector<std::string> out;
        for (const auto& [id, payload] : broadcast) {
            if (id == sb::kUpdateScore || id == sb::kUpdateObjectives || id == sb::kUpdateTeams ||
                id == sb::kDisplayObjective) {
                out.push_back(hex(payload));
            }
        }
        return out;
    }
};

bool have_packs() { return packs().blocks.has_value() && packs().registries.has_value(); }

std::string bracket_objective(std::string_view name, std::string_view shown = {}) {
    return R"({"translate":"chat.square_brackets","with":[{"hoverEvent":{"action":"show_text","contents":{"text":")" +
           std::string{name} + R"("}},"text":")" + std::string{shown.empty() ? name : shown} + R"("}]})";
}

std::string red_line(std::string_view key) {
    return R"({"color":"red","extra":[{"translate":")" + std::string{key} + R"("}],"text":""})";
}

}  // namespace

// ── Java's order ────────────────────────────────────────────────────────────

TEST_CASE("Java's hash map order is the order vanilla lists things in", "[scoreboard][order]") {
    CHECK(java_string_hash("") == 0);
    CHECK(java_string_hash("a") == 97);
    // "hello".hashCode(), the textbook value: 99162322.
    CHECK(java_string_hash("hello") == 99162322);
    // The 18 objectives of the capture, created in this order, came back from
    // `scoreboard objectives list` in the order below.
    Scoreboard board;
    for (const char* name : {"kills", "d", "d2", "d3", "abcdefghijklmnopqrstuvwxyz", "trig", "deaths",
                             "total", "hp", "food", "air", "armor", "xp", "lvl", "tkblue", "kbred",
                             "mined", "jumps"}) {
        (void)board.add_objective(name, *parse_criterion("dummy", nullptr), "{}", RenderType::Integer);
    }
    std::vector<std::string> listed;
    for (const Objective* o : board.objectives()) {
        listed.push_back(o->name);
    }
    CHECK(listed == std::vector<std::string>{"kills", "mined", "lvl", "d", "hp", "kbred", "jumps", "trig",
                                             "air", "d2", "food", "d3", "total", "armor", "tkblue",
                                             "abcdefghijklmnopqrstuvwxyz", "xp", "deaths"});
    // `*` named #hidden, ovprobe, fake — the holders' map.
    for (const char* holder : {"ovprobe", "fake", "#hidden"}) {
        (void)board.set_score(holder, "d", 1);
    }
    CHECK(board.holders() == std::vector<std::string>{"#hidden", "ovprobe", "fake"});
    // `team list`: red, green, blue, then the long one.
    for (const char* team : {"red", "blue", "green", "abcdefghijklmnopqrstuvwxyz"}) {
        (void)board.add_team(team, "{}");
    }
    std::vector<std::string> teams;
    for (const Team* t : board.teams()) {
        teams.push_back(t->name);
    }
    CHECK(teams == std::vector<std::string>{"red", "green", "blue", "abcdefghijklmnopqrstuvwxyz"});
}

// ── /scoreboard objectives ──────────────────────────────────────────────────

TEST_CASE("objectives answer as the real server did", "[scoreboard][commands][vanilla]") {
    if (!have_packs()) {
        SKIP("registry.ovpack not generated");
    }
    Harness h;
    CHECK(h.run("scoreboard objectives list") ==
          std::vector<std::string>{R"({"translate":"commands.scoreboard.objectives.list.empty"})"});
    CHECK(h.run("scoreboard objectives add kills playerKillCount") ==
          std::vector<std::string>{R"({"translate":"commands.scoreboard.objectives.add.success","with":[)" +
                                   bracket_objective("kills") + "]}"});
    CHECK(h.run("scoreboard objectives add kills dummy") ==
          std::vector<std::string>{red_line("commands.scoreboard.objectives.add.duplicate")});
    CHECK(h.run(R"(scoreboard objectives add d3 dummy {"text":"Three","color":"gold"})") ==
          std::vector<std::string>{
              R"({"translate":"commands.scoreboard.objectives.add.success","with":[{"translate":"chat.square_brackets","with":[{"color":"gold","hoverEvent":{"action":"show_text","contents":{"text":"d3"}},"text":"Three"}]}]})"});
    CHECK(h.run("scoreboard objectives add bad nosuchcriterion") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"argument.criteria.invalid","with":["nosuchcriterion"]}],"text":""})"});
    CHECK(h.run("scoreboard objectives add tkbad teamkill.nocolor") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"argument.criteria.invalid","with":["teamkill.nocolor"]}],"text":""})"});
    CHECK(h.run("scoreboard objectives add badstat minecraft.mined:minecraft.nosuch") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"argument.criteria.invalid","with":["minecraft.mined:minecraft.nosuch"]}],"text":""})"});
    CHECK(h.run("scoreboard objectives add mined minecraft.mined:minecraft.stone").size() == 1);
    // The capture's two lines for a word that does not start as one.
    CHECK(h.run("scoreboard objectives add 'q' dummy") ==
          std::vector<std::string>{
              red_line("command.expected.separator"),
              R"({"color":"red","extra":[{"color":"gray","clickEvent":{"action":"suggest_command","value":"/scoreboard objectives add 'q' dummy"},"extra":[{"text":"..."},{"text":"tives add "},{"underlined":true,"color":"red","text":"'q' dummy"},{"italic":true,"color":"red","translate":"command.context.here"}],"text":""}],"text":""})"});
    CHECK(h.run(R"(scoreboard objectives modify nosuch displayname "x")") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"arguments.objective.notFound","with":["nosuch"]}],"text":""})"});
    CHECK(h.run("scoreboard objectives setdisplay nosuchslot kills") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"argument.scoreboardDisplaySlot.invalid","with":["nosuchslot"]}],"text":""})"});
    // A stat criterion is stored under its full name.
    CHECK(h.service.scoreboard().objective("mined")->criterion.name == "minecraft.mined:minecraft.stone");
    CHECK(parse_criterion("minecraft.mined:stone", &*packs().registries)->name ==
          "minecraft.mined:minecraft.stone");
}

TEST_CASE("the display slots send what vanilla sends, when it sends it", "[scoreboard][packets][vanilla]") {
    if (!have_packs()) {
        SKIP("registry.ovpack not generated");
    }
    Harness h;
    (void)h.run("scoreboard objectives add d dummy");
    (void)h.run("scoreboard objectives add hp health");
    (void)h.run("scoreboard objectives add d3 dummy {\"text\":\"Three\",\"color\":\"gold\"}");
    // Nothing travels for an objective nobody is shown.
    CHECK(h.board().empty());

    CHECK(h.run("scoreboard objectives setdisplay sidebar d") ==
          std::vector<std::string>{
              R"({"translate":"commands.scoreboard.objectives.display.set","with":["sidebar",{"text":"d"}]})"});
    CHECK(h.board() == std::vector<std::string>{"0164000c7b2274657874223a2264227d00", "010164"});
    CHECK(h.run("scoreboard objectives setdisplay sidebar d") ==
          std::vector<std::string>{red_line("commands.scoreboard.objectives.display.alreadySet")});

    (void)h.run("scoreboard objectives setdisplay list hp");
    CHECK(h.board() == std::vector<std::string>{"026870000d7b2274657874223a226870227d01", "00026870"});

    (void)h.run("scoreboard objectives setdisplay sidebar.team.red d3");
    CHECK(h.board() == std::vector<std::string>{
                           "026433001f7b22636f6c6f72223a22676f6c64222c2274657874223a225468726565227d00",
                           "0f026433"});
    // Cleared, and shown nowhere else: the objective leaves the clients.
    CHECK(h.run("scoreboard objectives setdisplay sidebar.team.red") ==
          std::vector<std::string>{
              R"({"translate":"commands.scoreboard.objectives.display.cleared","with":["sidebar.team.red"]})"});
    CHECK(h.board() == std::vector<std::string>{"02643301"});
    CHECK(h.run("scoreboard objectives setdisplay sidebar.team.red") ==
          std::vector<std::string>{red_line("commands.scoreboard.objectives.display.alreadyEmpty")});
    // Already shown elsewhere: only the slot travels.
    (void)h.run("scoreboard objectives setdisplay sidebar.team.red d");
    CHECK(h.board() == std::vector<std::string>{"0f0164"});
    // Removed while shown: the objective leaves.
    (void)h.run("scoreboard objectives setdisplay sidebar.team.blue d3");
    CHECK(h.run("scoreboard objectives remove d3") ==
          std::vector<std::string>{
              R"({"translate":"commands.scoreboard.objectives.remove.success","with":[{"translate":"chat.square_brackets","with":[{"color":"gold","hoverEvent":{"action":"show_text","contents":{"text":"d3"}},"text":"Three"}]}]})"});
    CHECK(h.board() == std::vector<std::string>{"02643301"});
}

// ── /scoreboard players ─────────────────────────────────────────────────────

TEST_CASE("scores are created at 0, then set — and reset as vanilla resets", "[scoreboard][packets][vanilla]") {
    if (!have_packs()) {
        SKIP("registry.ovpack not generated");
    }
    Harness h;
    (void)h.run("scoreboard objectives add d dummy");
    (void)h.run("scoreboard objectives setdisplay sidebar d");
    CHECK(h.run("scoreboard players set @s d 5") ==
          std::vector<std::string>{R"({"translate":"commands.scoreboard.players.set.success.single","with":[)" +
                                   bracket_objective("d") + R"(,"ovprobe","5"]})"});
    CHECK(h.board() == std::vector<std::string>{"076f7670726f626500016400", "076f7670726f626500016405"});
    (void)h.run("scoreboard players set fake d 7");
    (void)h.run("scoreboard players set #hidden d 1");
    CHECK(h.run("scoreboard players set * d 3") ==
          std::vector<std::string>{R"({"translate":"commands.scoreboard.players.set.success.multiple","with":[)" +
                                   bracket_objective("d") + R"(,"3","3"]})"});
    // #hidden, ovprobe, fake: the holders' map order.
    CHECK(h.board() == std::vector<std::string>{"072368696464656e00016403", "076f7670726f626500016403",
                                                "0466616b6500016403"});
    CHECK(h.run("scoreboard players add @s d 10") ==
          std::vector<std::string>{R"({"translate":"commands.scoreboard.players.add.success.single","with":["10",)" +
                                   bracket_objective("d") + R"(,"ovprobe","13"]})"});
    CHECK(h.run("scoreboard players get nobody d") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"commands.scoreboard.players.get.null","with":["d","nobody"]}],"text":""})"});
    CHECK(h.run("scoreboard players list") ==
          std::vector<std::string>{
              R"({"translate":"commands.scoreboard.players.list.success","with":["3",{"extra":[{"color":"green","text":"#hidden"},{"color":"gray","text":", "},{"color":"green","text":"fake"},{"color":"gray","text":", "},{"color":"green","text":"ovprobe"}],"text":""}]})"});
    CHECK(h.run("scoreboard players list nobody") ==
          std::vector<std::string>{
              R"({"translate":"commands.scoreboard.players.list.entity.empty","with":["nobody"]})"});
    // fake's only score: a removal of everything.
    CHECK(h.run("scoreboard players reset fake d") ==
          std::vector<std::string>{R"({"translate":"commands.scoreboard.players.reset.specific.single","with":[)" +
                                   bracket_objective("d") + R"(,"fake"]})"});
    CHECK(h.board() == std::vector<std::string>{"0466616b650100"});
    CHECK(h.run("scoreboard players reset fake") ==
          std::vector<std::string>{R"({"translate":"commands.scoreboard.players.reset.all.single","with":["fake"]})"});
    CHECK(h.board().empty());
    (void)h.run("scoreboard objectives add hp health");
    CHECK(h.run("scoreboard players set @s hp 3") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"arguments.objective.readonly","with":["hp"]}],"text":""})"});
    // `*` in a single-holder argument names nobody — the capture's answer,
    // with three holders on the board.
    CHECK(h.run("scoreboard players list *") ==
          std::vector<std::string>{red_line("argument.scoreHolder.empty")});
}

TEST_CASE("operations are Java's: floor division, floor modulo, no division by zero",
          "[scoreboard][commands][vanilla]") {
    if (!have_packs()) {
        SKIP("registry.ovpack not generated");
    }
    Harness h;
    (void)h.run("scoreboard objectives add d dummy");
    const auto result = [&](std::string_view value) {
        return std::vector<std::string>{
            R"({"translate":"commands.scoreboard.players.operation.success.single","with":[)" +
            bracket_objective("d") + R"(,"a",")" + std::string{value} + R"("]})"};
    };
    (void)h.run("scoreboard players set a d 7");
    (void)h.run("scoreboard players set b d 2");
    CHECK(h.run("scoreboard players operation a d += b d") == result("9"));
    CHECK(h.run("scoreboard players operation a d -= b d") == result("7"));
    CHECK(h.run("scoreboard players operation a d *= b d") == result("14"));
    CHECK(h.run("scoreboard players operation a d /= b d") == result("7"));
    CHECK(h.run("scoreboard players operation a d %= b d") == result("1"));
    CHECK(h.run("scoreboard players operation a d = b d") == result("2"));
    (void)h.run("scoreboard players set a d 9");
    CHECK(h.run("scoreboard players operation a d >< b d") == result("2"));
    CHECK(h.service.scoreboard().score("b", "d")->value == 9);
    (void)h.run("scoreboard players set a d -7");
    (void)h.run("scoreboard players set b d 2");
    CHECK(h.run("scoreboard players operation a d /= b d") == result("-4"));
    (void)h.run("scoreboard players set a d -7");
    CHECK(h.run("scoreboard players operation a d %= b d") == result("1"));
    (void)h.run("scoreboard players set b d 0");
    CHECK(h.run("scoreboard players operation a d /= b d") ==
          std::vector<std::string>{red_line("arguments.operation.div0")});
    CHECK(h.run("scoreboard players operation a d foo b d") ==
          std::vector<std::string>{red_line("arguments.operation.invalid")});
    // A source with no score gets one, at 0.
    CHECK(h.run("scoreboard players operation a d += nobody d") == result("1"));
    CHECK(h.service.scoreboard().score("nobody", "d")->value == 0);
    (void)h.run("scoreboard players set a d 2147483647");
    CHECK(h.run("scoreboard players add a d 1") ==
          std::vector<std::string>{R"({"translate":"commands.scoreboard.players.add.success.single","with":["1",)" +
                                   bracket_objective("d") + R"(,"a","-2147483648"]})"});
    CHECK(h.run("scoreboard players add @s d -1").front() ==
          R"({"color":"red","extra":[{"translate":"argument.integer.low","with":["0","-1"]}],"text":""})");
}

TEST_CASE("trigger: enabled once, used once", "[scoreboard][commands][vanilla]") {
    if (!have_packs()) {
        SKIP("registry.ovpack not generated");
    }
    Harness h;
    (void)h.run("scoreboard objectives add trig trigger");
    (void)h.run("scoreboard objectives add d dummy");
    (void)h.run("scoreboard players set @s d 1");
    const std::string trig = bracket_objective("trig");
    CHECK(h.run("scoreboard players enable @s trig") ==
          std::vector<std::string>{R"({"translate":"commands.scoreboard.players.enable.success.single","with":[)" +
                                   trig + R"(,"ovprobe"]})"});
    CHECK(h.run("scoreboard players enable @s trig") ==
          std::vector<std::string>{red_line("commands.scoreboard.players.enable.failed")});
    CHECK(h.run("scoreboard players enable @s d") ==
          std::vector<std::string>{red_line("commands.scoreboard.players.enable.invalid")});
    CHECK(h.run("trigger trig") ==
          std::vector<std::string>{R"({"translate":"commands.trigger.simple.success","with":[)" + trig + "]}"});
    CHECK(h.run("trigger trig add 5") == std::vector<std::string>{red_line("commands.trigger.failed.unprimed")});
    CHECK(h.run("trigger d") == std::vector<std::string>{red_line("commands.trigger.failed.invalid")});
    CHECK(h.run("trigger nosuch") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"arguments.objective.notFound","with":["nosuch"]}],"text":""})"});
    (void)h.run("scoreboard players enable @s trig");
    CHECK(h.run("trigger trig add 5") ==
          std::vector<std::string>{R"({"translate":"commands.trigger.add.success","with":[)" + trig + R"(,"5"]})"});
    CHECK(h.service.scoreboard().score("ovprobe", "trig")->value == 6);
    // From the console: a player is required.
    std::vector<std::string> console;
    h.service.console = [&](std::string_view line) { console.emplace_back(line); };
    (void)h.service.execute(CommandSource{}, "trigger trig", h.host);
    CHECK(console == std::vector<std::string>{"permissions.requires.player"});
}

TEST_CASE("the scores= selector option chooses by score", "[scoreboard][selector]") {
    if (!have_packs()) {
        SKIP("registry.ovpack not generated");
    }
    Harness h;
    (void)h.run("scoreboard objectives add d dummy");
    (void)h.run("scoreboard objectives add e dummy");
    (void)h.run("scoreboard players set @s d 5");
    const std::vector<std::string> x{R"({"text":"x"})"};
    const std::vector<std::string> nobody{red_line("argument.entity.notfound.player")};
    CHECK(h.run(R"(tellraw @a[scores={d=5}] "x")") == x);
    CHECK(h.run(R"(tellraw @a[scores={d=1..}] "x")") == x);
    CHECK(h.run(R"(tellraw @a[scores={d=..4}] "x")") == nobody);
    // An objective the player has no score on excludes it.
    CHECK(h.run(R"(tellraw @a[scores={d=5,e=0..}] "x")") == nobody);
    (void)h.run("scoreboard players set @s e 0");
    CHECK(h.run(R"(tellraw @a[scores={ d = 5 , e = 0.. }] "x")") == x);
}

TEST_CASE("a score component reads the scoreboard", "[scoreboard][text]") {
    if (!have_packs()) {
        SKIP("registry.ovpack not generated");
    }
    Harness h;
    (void)h.run("scoreboard objectives add d dummy");
    (void)h.run("scoreboard players set @s d 12");
    CHECK(h.run(R"(tellraw @s {"score":{"name":"@s","objective":"d"}})") ==
          std::vector<std::string>{R"({"text":"12"})"});
    CHECK(h.run(R"(tellraw @s {"score":{"name":"*","objective":"d"}})") ==
          std::vector<std::string>{R"({"text":"12"})"});
    // No score: an empty text, not an error.
    CHECK(h.run(R"(tellraw @s {"score":{"name":"nobody","objective":"d"}})") ==
          std::vector<std::string>{R"({"text":""})"});
}

// ── /team ───────────────────────────────────────────────────────────────────

TEST_CASE("teams: created then named, joined, dressed, listed", "[scoreboard][teams][vanilla]") {
    if (!have_packs()) {
        SKIP("registry.ovpack not generated");
    }
    Harness h;
    CHECK(h.run("team add red") ==
          std::vector<std::string>{
              R"({"translate":"commands.team.add.success","with":[{"translate":"chat.square_brackets","with":[{"insertion":"red","hoverEvent":{"action":"show_text","contents":{"text":"red"}},"text":"red"}]}]})"});
    // Create, then Update: `team add` names the team after making it.
    CHECK(h.board() ==
          std::vector<std::string>{
              "03726564000e7b2274657874223a22726564227d0306616c7761797306616c77617973150b7b2274657874223a2222"
              "7d0b7b2274657874223a22227d00",
              "03726564020e7b2274657874223a22726564227d0306616c7761797306616c77617973150b7b2274657874223a2222"
              "7d0b7b2274657874223a22227d"});
    (void)h.run("team add blue \"Blue Team\"");
    (void)h.run(R"(team add green {"text":"G","color":"green"})");
    (void)h.run("team add abcdefghijklmnopqrstuvwxyz");
    CHECK(h.run("team list") ==
          std::vector<std::string>{
              R"({"translate":"commands.team.list.teams.success","with":["4",{"extra":[{"translate":"chat.square_brackets","with":[{"insertion":"red","hoverEvent":{"action":"show_text","contents":{"text":"red"}},"text":"red"}]},{"color":"gray","text":", "},{"translate":"chat.square_brackets","with":[{"color":"green","insertion":"green","hoverEvent":{"action":"show_text","contents":{"text":"green"}},"text":"G"}]},{"color":"gray","text":", "},{"translate":"chat.square_brackets","with":[{"insertion":"blue","hoverEvent":{"action":"show_text","contents":{"text":"blue"}},"text":"Blue Team"}]},{"color":"gray","text":", "},{"translate":"chat.square_brackets","with":[{"insertion":"abcdefghijklmnopqrstuvwxyz","hoverEvent":{"action":"show_text","contents":{"text":"abcdefghijklmnopqrstuvwxyz"}},"text":"abcdefghijklmnopqrstuvwxyz"}]}],"text":""}]})"});
    CHECK(h.run("team list nosuch") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"team.notFound","with":["nosuch"]}],"text":""})"});
    (void)h.run("team join red");
    // Joining the team one is on: out, then in.
    (void)h.run("team join red @s");
    CHECK(h.board() == std::vector<std::string>{"037265640401076f7670726f6265", "037265640301076f7670726f6265"});
    (void)h.run("team join red fake");
    CHECK(h.run("team join blue fake2 fake3") ==
          std::vector<std::string>{
              red_line("command.unknown.argument"),
              R"({"color":"red","extra":[{"color":"gray","clickEvent":{"action":"suggest_command","value":"/team join blue fake2 fake3"},"extra":[{"text":"..."},{"text":"lue fake2 "},{"underlined":true,"color":"red","text":"fake3"},{"italic":true,"color":"red","translate":"command.context.here"}],"text":""}],"text":""})"});
    CHECK(h.run("team list red") ==
          std::vector<std::string>{
              R"({"translate":"commands.team.list.members.success","with":[{"translate":"chat.square_brackets","with":[{"insertion":"red","hoverEvent":{"action":"show_text","contents":{"text":"red"}},"text":"red"}]},"2",{"extra":[{"color":"green","text":"fake"},{"color":"gray","text":", "},{"color":"green","text":"ovprobe"}],"text":""}]})"});
    CHECK(h.run("team modify red color red") ==
          std::vector<std::string>{
              R"({"translate":"commands.team.option.color.success","with":[{"color":"red","translate":"chat.square_brackets","with":[{"insertion":"red","hoverEvent":{"action":"show_text","contents":{"text":"red"}},"text":"red"}]},"red"]})"});
    CHECK(h.run("team modify red color red") ==
          std::vector<std::string>{red_line("commands.team.option.color.unchanged")});
    CHECK(h.run("team modify red color nocolor") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"argument.color.invalid","with":["nocolor"]}],"text":""})"});
    CHECK(h.run("team modify red friendlyFire false").size() == 1);
    CHECK(h.run("team modify red friendlyFire false") ==
          std::vector<std::string>{red_line("commands.team.option.friendlyfire.alreadyDisabled")});
    CHECK(h.run("team modify red nametagVisibility hideForOtherTeams") ==
          std::vector<std::string>{
              R"({"translate":"commands.team.option.nametagVisibility.success","with":[{"color":"red","translate":"chat.square_brackets","with":[{"insertion":"red","hoverEvent":{"action":"show_text","contents":{"text":"red"}},"text":"red"}]},{"translate":"team.visibility.hideForOtherTeams"}]})"});
    (void)h.run("team modify red collisionRule pushOwnTeam");
    CHECK(h.run(R"(team modify red displayName {"text":"Rouge","color":"dark_red"})") ==
          std::vector<std::string>{
              R"({"translate":"commands.team.option.name.success","with":[{"color":"red","translate":"chat.square_brackets","with":[{"color":"dark_red","insertion":"red","hoverEvent":{"action":"show_text","contents":{"text":"red"}},"text":"Rouge"}]}]})"});
    CHECK(h.run(R"(team modify red prefix {"text":"[R] "})") ==
          std::vector<std::string>{R"({"translate":"commands.team.option.prefix.success","with":[{"text":"[R] "}]})"});
    (void)h.run(R"(team modify red suffix " !")");
    // A player's name now wears the team, in every line that names it.
    CHECK(h.run(R"(tellraw @s {"selector":"@s"})") ==
          std::vector<std::string>{
              R"({"color":"red","insertion":"ovprobe","clickEvent":{"action":"suggest_command","value":"/tell ovprobe "},"hoverEvent":{"action":"show_entity","contents":{"type":"minecraft:player","id":"f3f20367-e988-37d8-ac20-1a1929fd1e59","name":{"text":"ovprobe"}}},"extra":[{"text":"[R] "},{"text":"ovprobe"},{"text":" !"}],"text":""})"});
    CHECK(h.run("team leave fake") ==
          std::vector<std::string>{R"({"translate":"commands.team.leave.success.single","with":["fake"]})"});
    CHECK(h.run("team leave nobody") ==
          std::vector<std::string>{R"({"translate":"commands.team.leave.success.single","with":["nobody"]})"});
    CHECK(h.run("team empty blue") == std::vector<std::string>{red_line("commands.team.empty.unchanged")});
    CHECK(h.run("team remove nosuch").size() == 1);
    // The selector reads the teams: ovprobe is on red, and on some team.
    const std::vector<std::string> x{R"({"text":"x"})"};
    const std::vector<std::string> nobody{red_line("argument.entity.notfound.player")};
    CHECK(h.run("tellraw @a[team=red] \"x\"") == x);
    CHECK(h.run("tellraw @a[team=!] \"x\"") == x);
    CHECK(h.run("tellraw @a[team=!blue] \"x\"") == x);
    CHECK(h.run("tellraw @a[team=blue] \"x\"") == nobody);
    CHECK(h.run("tellraw @a[team=] \"x\"") == nobody);
}

TEST_CASE("teammsg: out to the sender, in to the team, as vanilla types them", "[scoreboard][teams][vanilla]") {
    if (!have_packs()) {
        SKIP("registry.ovpack not generated");
    }
    Harness h;
    h.add_player("ovother", 0);
    (void)h.run("team add red");
    (void)h.run(R"(team modify red displayName {"text":"Rouge","color":"dark_red"})");
    (void)h.run("team modify red color red");
    (void)h.run(R"(team modify red prefix {"text":"[R] "})");
    (void)h.run(R"(team modify red suffix " !")");
    (void)h.run("team join red ovprobe");
    CHECK(h.run("teammsg alone", 1) == std::vector<std::string>{red_line("commands.teammsg.failed.noteam")});
    (void)h.run("team join red ovother");
    (void)h.run("teammsg to both of us");
    const auto chats = [&](usize who) {
        std::vector<net::PlayerChat> out;
        for (const auto& [id, payload] : h.players[who]->sent) {
            if (id == net::clientbound::kPlayerChat) {
                if (auto chat = net::parse_player_chat(payload)) {
                    out.push_back(std::move(*chat));
                }
            }
        }
        return out;
    };
    const std::string name =
        R"({"color":"red","insertion":"ovprobe","clickEvent":{"action":"suggest_command","value":"/tell ovprobe "},"hoverEvent":{"action":"show_entity","contents":{"type":"minecraft:player","id":"f3f20367-e988-37d8-ac20-1a1929fd1e59","name":{"text":"ovprobe"}}},"extra":[{"text":"[R] "},{"text":"ovprobe"},{"text":" !"}],"text":""})";
    const std::string target =
        R"({"color":"red","clickEvent":{"action":"suggest_command","value":"/teammsg "},"hoverEvent":{"action":"show_text","contents":{"translate":"chat.type.team.hover"}},"translate":"chat.square_brackets","with":[{"color":"dark_red","insertion":"red","hoverEvent":{"action":"show_text","contents":{"text":"red"}},"text":"Rouge"}]})";
    const auto mine   = chats(0);
    const auto theirs = chats(1);
    REQUIRE(mine.size() == 1);
    REQUIRE(theirs.size() == 1);
    CHECK(mine[0].chat_type == 6);
    CHECK(theirs[0].chat_type == 5);
    CHECK(mine[0].name_json == name);
    CHECK(mine[0].target_json == target);
    CHECK(theirs[0].body == "to both of us");
    // /tm is the same command.
    (void)h.run("tm and back", 1);
    CHECK(chats(0).size() == 1);
    CHECK(chats(0)[0].chat_type == 5);
}

// ── Criteria ────────────────────────────────────────────────────────────────

TEST_CASE("the kill and death criteria count as the capture counted", "[scoreboard][criteria]") {
    Scoreboard board;
    const auto add = [&](std::string name, std::string_view criterion) {
        (void)board.add_objective(std::move(name), *parse_criterion(criterion, nullptr), "{}", RenderType::Integer);
    };
    add("kills", "playerKillCount");
    add("total", "totalKillCount");
    add("deaths", "deathCount");
    add("tkblue", "teamkill.blue");
    add("kbred", "killedByTeam.red");
    (void)board.add_team("red", "{}");
    (void)board.add_team("blue", "{}");
    board.edit_team("red")->color  = *color_from_name("red");
    board.edit_team("blue")->color = *color_from_name("blue");
    board.join("red", "ovprobe");
    board.join("blue", "ovother");
    // ovprobe (red) kills ovother (blue).
    board.on_kill("ovprobe", "ovother", true);
    board.on_death("ovother");
    CHECK(board.score("ovprobe", "kills")->value == 1);
    CHECK(board.score("ovprobe", "total")->value == 1);
    CHECK(board.score("ovprobe", "tkblue")->value == 1);
    CHECK(board.score("ovother", "kbred")->value == 1);
    CHECK(board.score("ovother", "deaths")->value == 1);
    // A mob is not a player kill, and dying by one's own hand counts nothing.
    board.on_kill("ovprobe", "3f0e1b5a-0000-0000-0000-000000000000", false);
    board.on_kill("ovprobe", "ovprobe", true);
    CHECK(board.score("ovprobe", "kills")->value == 1);
    CHECK(board.score("ovprobe", "total")->value == 2);
}

TEST_CASE("the player's own criteria: all at arrival, then what changes", "[scoreboard][criteria]") {
    Scoreboard board;
    (void)board.add_objective("hp", *parse_criterion("health", nullptr), "{}", RenderType::Hearts);
    (void)board.add_objective("air", *parse_criterion("air", nullptr), "{}", RenderType::Integer);
    board.set_displayed(sb::kSlotList, "hp");
    (void)board.take_packets();
    board.update_player("ovother", PlayerCriteria{20, 20, 300, 0, 0, 0});
    // The capture's arrival: hp created at 0, then 20.
    const auto packets = board.take_packets();
    REQUIRE(packets.size() == 2);
    CHECK(hex(packets[0].payload) == "076f766f746865720002687000");
    CHECK(hex(packets[1].payload) == "076f766f746865720002687014");
    CHECK(board.score("ovother", "air")->value == 300);
    board.update_player("ovother", PlayerCriteria{19, 20, 300, 0, 0, 0});
    CHECK(hex(board.take_packets().at(0).payload) == "076f766f746865720002687013");
    board.update_player("ovother", PlayerCriteria{19, 20, 300, 0, 0, 0});
    CHECK_FALSE(board.has_packets());
    // The statistics hook: a total becomes the score.
    (void)board.add_objective("mined", *parse_criterion("minecraft.mined:minecraft.stone", nullptr), "{}",
                              RenderType::Integer);
    board.on_stat("ovother", "minecraft:mined", "minecraft:stone", 42);
    CHECK(board.score("ovother", "mined")->value == 42);
}

TEST_CASE("friendly fire and collision rules", "[scoreboard][teams]") {
    Scoreboard board;
    (void)board.add_team("red", "{}");
    (void)board.add_team("blue", "{}");
    const Team* red  = board.team("red");
    const Team* blue = board.team("blue");
    CHECK(Scoreboard::can_hurt(red, red));
    board.edit_team("red")->friendly_fire = false;
    CHECK_FALSE(Scoreboard::can_hurt(red, red));
    CHECK(Scoreboard::can_hurt(red, blue));
    CHECK(Scoreboard::can_hurt(nullptr, red));
    CHECK(Scoreboard::can_push(red, blue));
    board.edit_team("red")->collision = CollisionRule::PushOwnTeam;
    CHECK(Scoreboard::can_push(red, red));
    CHECK_FALSE(Scoreboard::can_push(red, blue));
    CHECK_FALSE(Scoreboard::can_push(red, nullptr));
    board.edit_team("red")->collision = CollisionRule::PushOtherTeams;
    CHECK_FALSE(Scoreboard::can_push(red, red));
    CHECK(Scoreboard::can_push(red, blue));
    board.edit_team("blue")->collision = CollisionRule::Never;
    CHECK_FALSE(Scoreboard::can_push(red, blue));
}

// ── The file ────────────────────────────────────────────────────────────────

TEST_CASE("scoreboard.dat reads back what it wrote, and keeps what it does not model",
          "[scoreboard][persistence]") {
    Scoreboard board;
    (void)board.add_objective("d", *parse_criterion("dummy", nullptr), R"({"text":"d"})", RenderType::Integer);
    (void)board.add_objective("hp", *parse_criterion("health", nullptr), R"({"text":"hp"})", RenderType::Hearts);
    (void)board.set_score("a", "d", -2147483647 - 1);
    (void)board.set_score("#hidden", "d", 3);
    (void)board.set_locked("fake", "d", false);
    board.set_displayed(sb::kSlotSidebar, "d");
    board.set_displayed(15, "d");
    (void)board.add_team("red", R"({"color":"dark_red","text":"Rouge"})");
    board.edit_team("red")->color         = 12;
    board.edit_team("red")->friendly_fire = false;
    board.edit_team("red")->collision     = CollisionRule::PushOwnTeam;
    board.join("red", "ovprobe");
    (void)board.add_team("plain", R"({"text":"plain"})");

    nbt::Tag root = board.save();
    // What the real file carries, key for key (vanilla_scoreboard.dat of the
    // capture): a team with no colour has no TeamColor; a slot is slot_<n>.
    const nbt::Tag* data = root.find("data");
    REQUIRE(data != nullptr);
    CHECK(root.find("DataVersion")->as_i64() == 3465);
    CHECK(data->find("DisplaySlots")->find("slot_1")->as_string() == "d");
    CHECK(data->find("DisplaySlots")->find("slot_15")->as_string() == "d");
    const auto& teams = *data->find("Teams")->list();
    REQUIRE(teams.size() == 2);
    const nbt::Tag& red   = teams[0].find("Name")->as_string() == "red" ? teams[0] : teams[1];
    const nbt::Tag& plain = teams[0].find("Name")->as_string() == "red" ? teams[1] : teams[0];
    CHECK(red.find("TeamColor")->as_string() == "red");
    CHECK(red.find("AllowFriendlyFire")->as_i64() == 0);
    CHECK(red.find("CollisionRule")->as_string() == "pushOwnTeam");
    CHECK(plain.find("TeamColor") == nullptr);
    CHECK(plain.find("MemberNamePrefix")->as_string() == R"({"text":""})");

    // Keys this server does not know, and an objective whose criterion it
    // does not know, with its score: all come back out.
    root.find("data")->put("FutureKey", nbt::Tag{std::string{"kept"}});
    root.put("RootKey", nbt::Tag{i32{7}});
    nbt::Tag odd = nbt::Tag::make_compound();
    odd.put("Name", nbt::Tag{std::string{"odd"}});
    odd.put("CriteriaName", nbt::Tag{std::string{"modded:thing"}});
    root.find("data")->find("Objectives")->push(odd);
    nbt::Tag odd_score = nbt::Tag::make_compound();
    odd_score.put("Name", nbt::Tag{std::string{"x"}});
    odd_score.put("Objective", nbt::Tag{std::string{"odd"}});
    odd_score.put("Score", nbt::Tag{i32{1}});
    root.find("data")->find("PlayerScores")->push(odd_score);

    auto loaded = Scoreboard::load(root, nullptr);
    REQUIRE(loaded.has_value());
    CHECK(loaded->score("a", "d")->value == -2147483647 - 1);
    CHECK(loaded->score("#hidden", "d")->locked);
    CHECK_FALSE(loaded->score("fake", "d")->locked);
    CHECK(loaded->displayed(sb::kSlotSidebar)->name == "d");
    CHECK(loaded->team_of("ovprobe")->name == "red");
    CHECK(loaded->team("red")->collision == CollisionRule::PushOwnTeam);
    CHECK_FALSE(loaded->team("red")->friendly_fire);
    CHECK(loaded->objective("hp")->render == RenderType::Hearts);
    CHECK_FALSE(loaded->dirty());
    CHECK_FALSE(loaded->has_packets());
    // Saved again: the unknown parts included.
    const nbt::Tag again = loaded->save();
    CHECK(again.find("RootKey")->as_i64() == 7);
    CHECK(again.find("data")->find("FutureKey")->as_string() == "kept");
    usize odd_objectives = 0;
    for (const nbt::Tag& o : *again.find("data")->find("Objectives")->list()) {
        odd_objectives += o.find("Name")->as_string() == "odd" ? 1U : 0U;
    }
    CHECK(odd_objectives == 1);
    CHECK(again.find("data")->find("PlayerScores")->list()->size() ==
          root.find("data")->find("PlayerScores")->list()->size());

    // A newer DataVersion is refused rather than half-read.
    nbt::Tag newer = board.save();
    newer.put("DataVersion", nbt::Tag{i32{9999}});
    CHECK_FALSE(Scoreboard::load(newer, nullptr).has_value());
}

TEST_CASE("a newcomer is sent the teams, then what the slots show", "[scoreboard][packets][vanilla]") {
    Scoreboard board;
    (void)board.add_team("red", "{}");
    (void)board.add_objective("hp", *parse_criterion("health", nullptr), R"({"text":"hp"})", RenderType::Hearts);
    (void)board.add_objective("d", *parse_criterion("dummy", nullptr), R"({"text":"d"})", RenderType::Integer);
    board.set_displayed(sb::kSlotSidebar, "d");
    board.set_displayed(sb::kSlotList, "hp");
    board.set_displayed(15, "d");
    for (const auto& [holder, value] : std::vector<std::pair<std::string, i32>>{
             {"ovprobe", 12}, {"#hidden", 3}, {"a,b", 0}, {"b", 0}, {"nobody", 0}, {"a", -2147483647 - 1}}) {
        (void)board.set_score(holder, "d", value);
    }
    (void)board.take_packets();
    std::vector<ScoreboardPacket> arrival;
    board.arrival_packets(arrival);
    std::vector<std::string> seen;
    for (const ScoreboardPacket& p : arrival) {
        seen.push_back(hex(p.payload));
    }
    // The capture's order: teams; hp and its slot; d, its two slots, and its
    // scores by value, then by name backwards.
    REQUIRE(seen.size() == 12);
    CHECK(seen[1] == "026870000d7b2274657874223a226870227d01");
    CHECK(seen[2] == "00026870");
    CHECK(seen[3] == "0164000c7b2274657874223a2264227d00");
    CHECK(seen[4] == "010164");
    CHECK(seen[5] == "0f0164");
    CHECK(seen[6] == "01610001648080808008");
    CHECK(seen[7] == "066e6f626f647900016400");
    CHECK(seen[8] == "016200016400");
    CHECK(seen[9] == "03612c6200016400");
    CHECK(seen[10] == "072368696464656e00016403");
    CHECK(seen[11] == "076f7670726f62650001640c");
}
