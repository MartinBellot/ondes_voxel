// The command engine, against the vanilla server's own answers.
//
// Every expected string below that looks like JSON is **a capture**: the
// bytes a real 1.20.1 server sent a probe client for the same command
// (`scripts/capture_commands.py`, `data/vanilla/1.20.1/normalized/
// commands_capture_vanilla.json`). The test runs the command against a fake
// server — one player, a flat world held in a map — and compares.
//
// The rest pins what the capture cannot: the parser on hostile input (no
// crash, ever), the SNBT and JSON round trips, the weather ramp's float bits,
// and the Commands packet read back by the protocol's own parser.
#include "../src/commands/dispatcher.hpp"
#include "../src/commands/game_rules.hpp"
#include "../src/commands/json.hpp"
#include "../src/commands/names.hpp"
#include "../src/commands/ops.hpp"
#include "../src/commands/selector.hpp"
#include "../src/commands/service.hpp"
#include "../src/commands/snbt.hpp"
#include "../src/commands/string_reader.hpp"
#include "../src/commands/text.hpp"
#include "../src/commands/world_state.hpp"

#include "ov/io/file.hpp"
#include "ov/protocol/chat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>
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

/// A server of one player on an empty flat world, recording what it is told.
struct FakeServer {
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
    std::string                    name{"ovprobe"};
    net::Uuid                      uuid{net::Uuid::offline_player("ovprobe")};

    std::vector<std::pair<i32, std::vector<u8>>> sent;
    std::vector<std::pair<i32, std::vector<u8>>> broadcast;
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> blocks;
    std::vector<EntityInfo>                                     mobs;
    std::vector<std::string>                                    console;
    i32  next_id{100};
    bool stopped{false};
    bool saved{false};

    CommandHost host() {
        CommandHost h;
        h.for_each_player = [this](const std::function<void(PlayerRef&)>& visit) {
            PlayerRef p;
            p.entity_id  = 1;
            p.name       = name;
            p.uuid       = uuid;
            p.x          = &x;
            p.y          = &y;
            p.z          = &z;
            p.yaw        = &yaw;
            p.pitch      = &pitch;
            p.game_mode  = &game_mode;
            p.permission = &permission;
            p.inventory  = &inventory;
            p.carried    = &carried;
            p.survival   = &survival;
            p.effects    = &effects;
            p.effect_bearer = EffectBearer{1, game_mode == 0 || game_mode == 2, 0};
            p.broadcast_others = [](i32, std::span<const u8>) {};
            p.send       = [this](i32 id, std::span<const u8> payload) {
                sent.emplace_back(id, std::vector<u8>(payload.begin(), payload.end()));
            };
            visit(p);
        };
        h.entities  = [this](std::vector<EntityInfo>& out) { out.insert(out.end(), mobs.begin(), mobs.end()); };
        h.broadcast = [this](i32 id, std::span<const u8> payload) {
            broadcast.emplace_back(id, std::vector<u8>(payload.begin(), payload.end()));
            sent.emplace_back(id, std::vector<u8>(payload.begin(), payload.end()));
        };
        h.teleport_player = [this](i32, Vec3d p, f32 new_yaw, f32 new_pitch, u8) {
            x     = p.x;
            y     = p.y;
            z     = p.z;
            yaw   = new_yaw;
            pitch = new_pitch;
        };
        h.teleport_entity = [this](i32 id, Vec3d p, f32, f32) {
            for (EntityInfo& e : mobs) {
                if (e.id == id) {
                    e.position = p;
                    return true;
                }
            }
            return false;
        };
        h.kill_entity = [this](i32 id) {
            return std::erase_if(mobs, [&](const EntityInfo& e) { return e.id == id; }) > 0;
        };
        h.summon = [this](std::string_view type, Vec3d p) -> std::optional<EntityInfo> {
            EntityInfo e;
            e.id       = next_id++;
            e.type     = std::string{type};
            e.uuid     = net::Uuid{0x1234, static_cast<u64>(e.id)};
            e.position = p;
            mobs.push_back(e);
            return e;
        };
        h.drop_item  = [](i32, const net::ItemStack&) {};
        h.is_loaded  = [](i32, i32) { return true; };
        h.block_at   = [this](BlockPos p) {
            const auto it = blocks.find({p.x, p.y, p.z});
            return it == blocks.end() ? registry::kAirState : it->second;
        };
        h.set_block  = [this](BlockPos p, registry::BlockStateId s) { blocks[{p.x, p.y, p.z}] = s; };
        h.destroy_block = [this](BlockPos p) { blocks.erase({p.x, p.y, p.z}); };
        h.set_blocks = [this](std::span<const BlockChange> changes) {
            for (const BlockChange& c : changes) {
                blocks[{c.pos.x, c.pos.y, c.pos.z}] = c.state;
            }
        };
        h.break_blocks    = [](std::span<const BlockPos>) {};
        h.kick            = [](i32, std::string_view) {};
        h.save            = [this] { saved = true; };
        h.stop            = [this] { stopped = true; };
        h.set_world_spawn = [](i32, i32, i32, f32) {};
        return h;
    }

    /// The System Chat Messages sent to the player, as their JSON.
    [[nodiscard]] std::vector<std::string> chat() const {
        std::vector<std::string> out;
        for (const auto& [id, payload] : sent) {
            if (id == net::clientbound::kSystemChat) {
                if (const auto parsed = net::parse_system_chat(payload)) {
                    out.push_back(parsed->content);
                }
            }
        }
        return out;
    }
};

struct Harness {
    FakeServer     server;
    CommandService service;
    CommandHost    host;

    Harness()
        : service{ServiceConfig{packs().blocks ? &*packs().blocks : nullptr,
                                packs().registries ? &*packs().registries : nullptr,
                                {}, {}, false, 4, "Ondes VOXEL"}} {
        host            = server.host();
        service.console = [this](std::string_view line) { server.console.emplace_back(line); };
        service.world().rules.set(*GameRules::index_of("doDaylightCycle"), 0);
    }

    CommandSource player() const {
        CommandSource s;
        s.kind       = CommandSource::Kind::Player;
        s.entity_id  = 1;
        s.name       = server.name;
        s.uuid       = server.uuid;
        s.position   = Vec3d{server.x, server.y, server.z};
        s.yaw        = server.yaw;
        s.pitch      = server.pitch;
        s.permission = server.permission;
        return s;
    }

    std::vector<std::string> run(std::string_view command) {
        server.sent.clear();
        server.broadcast.clear();
        (void)service.execute(player(), command, host);
        return server.chat();
    }
};

bool have_packs() { return packs().blocks.has_value() && packs().registries.has_value(); }

const std::string kProbe =
    R"({"insertion":"ovprobe","clickEvent":{"action":"suggest_command","value":"/tell ovprobe "},)"
    R"("hoverEvent":{"action":"show_entity","contents":{"type":"minecraft:player",)"
    R"("id":"f3f20367-e988-37d8-ac20-1a1929fd1e59","name":{"text":"ovprobe"}}},"text":"ovprobe"})";

}  // namespace

// ── The reader ──────────────────────────────────────────────────────────────

TEST_CASE("the reader leaves the cursor where vanilla does", "[commands][reader]") {
    StringReader numbers{"1.5 -60"};
    const auto   bad = numbers.read_int();
    REQUIRE_FALSE(bad);
    CHECK(bad.error().cursor == 0u);
    CHECK(to_json(bad.error().message) == R"({"translate":"parsing.int.invalid","with":["1.5"]})");

    StringReader empty{"abc"};
    const auto   expected = empty.read_int();
    REQUIRE_FALSE(expected);
    CHECK(to_json(expected.error().message) == R"({"translate":"parsing.int.expected"})");

    StringReader quoted{R"("a\"b" rest)"};
    const auto   text = quoted.read_string();
    REQUIRE(text);
    CHECK(*text == R"(a"b)");

    StringReader escape{R"("a\q")"};
    const auto   refused = escape.read_string();
    REQUIRE_FALSE(refused);
    CHECK(to_json(refused.error().message) == R"({"translate":"parsing.quote.escape","with":["q"]})");

    StringReader boolean{"yes"};
    const auto   no = boolean.read_boolean();
    REQUIRE_FALSE(no);
    CHECK(to_json(no.error().message) == R"({"translate":"parsing.bool.invalid","with":["yes"]})");
}

TEST_CASE("java number parsing refuses what Java refuses", "[commands][reader]") {
    CHECK(parse_java_long("-5", -10, 10) == -5);
    CHECK_FALSE(parse_java_long("+5", -10, 10));
    CHECK_FALSE(parse_java_long("1-2", -10, 10));
    CHECK_FALSE(parse_java_long("2147483648", -2147483648LL, 2147483647));
    CHECK(parse_java_double("1.") == 1.0);
    CHECK(parse_java_double(".5") == 0.5);
    CHECK(parse_java_double("-.5") == -0.5);
    CHECK_FALSE(parse_java_double("."));
    CHECK_FALSE(parse_java_double("1.2.3"));
    CHECK_FALSE(parse_java_double("-"));
}

TEST_CASE("java number strings", "[commands][text]") {
    CHECK(java_float_string(0.0F) == "0.0");
    CHECK(java_float_string(90.0F) == "90.0");
    CHECK(java_float_string(45.0F) == "45.0");
    CHECK(java_float_string(0.5F) == "0.5");
    CHECK(java_float_string(1.0e7F) == "1.0E7");
    CHECK(java_double_string(0.001) == "0.001");
    CHECK(java_double_string(0.0001) == "1.0E-4");
    CHECK(java_fixed6(0.5) == "0.500000");
    CHECK(java_fixed6(-60.0) == "-60.000000");
}

// ── Components ──────────────────────────────────────────────────────────────

TEST_CASE("components serialise in vanilla's key order", "[commands][text]") {
    Text admin = Text::translatable("chat.type.admin", {Text::literal("Server"),
                                                        Text::translatable("commands.time.set", {Text::raw("1000")})});
    admin.style.italic = true;
    admin.color("gray");
    CHECK(to_json(admin) ==
          R"({"italic":true,"color":"gray","translate":"chat.type.admin","with":[{"text":"Server"},{"translate":"commands.time.set","with":["1000"]}]})");
    CHECK(to_json(player_display_name("ovprobe", net::Uuid::offline_player("ovprobe"))) == kProbe);
}

TEST_CASE("tellraw components read and re-serialise as vanilla's do", "[commands][text]") {
    const auto convert = [](std::string_view json) {
        usize      consumed = 0;
        const auto value    = parse_json(json, consumed);
        REQUIRE(value);
        const auto text = text_from_json(*value);
        REQUIRE(text);
        return to_json(*text);
    };
    // All five from the capture.
    CHECK(convert(R"("plain")") == R"({"text":"plain"})");
    CHECK(convert(R"({"text":"hi","color":"red"})") == R"({"color":"red","text":"hi"})");
    CHECK(convert(R"(["a",{"text":"b","bold":true}])") == R"({"extra":[{"bold":true,"text":"b"}],"text":"a"})");
    CHECK(convert(R"({"translate":"commands.time.set","with":["5"]})") ==
          R"({"translate":"commands.time.set","with":["5"]})");
    CHECK(convert("12") == R"({"text":"12"})");
    CHECK(convert(R"({"text":"x","color":"nocolor"})") == R"({"text":"x"})");
}

TEST_CASE("malformed JSON gets Gson's lenient hint", "[commands][json]") {
    usize      consumed = 0;
    const auto bad      = parse_json("{bad json", consumed);
    REQUIRE_FALSE(bad);
    // The exact message the vanilla server put in argument.component.invalid.
    CHECK(bad.error().message ==
          "Use JsonReader.setLenient(true) to accept malformed JSON at line 1 column 3 path $.");
}

TEST_CASE("the JSON reader survives hostile input", "[commands][json][hostile]") {
    std::string deep(100000, '[');
    usize       consumed = 0;
    CHECK_FALSE(parse_json(deep, consumed));
    u64 state = 0x9E3779B97F4A7C15ULL;
    for (int round = 0; round < 2000; ++round) {
        std::string noise;
        for (int i = 0; i < 40; ++i) {
            state ^= state << 13U;
            state ^= state >> 7U;
            state ^= state << 17U;
            noise.push_back("{}[]\",:\\ 0123456789truefalsenull.-eE"[state % 37]);
        }
        (void)parse_json(noise, consumed);
    }
    SUCCEED();
}

// ── SNBT ────────────────────────────────────────────────────────────────────

TEST_CASE("snbt reads and prints as vanilla's hover does", "[commands][snbt]") {
    StringReader reader{R"({display:{Name:'"x"'}})"};
    const auto   tag = read_snbt_compound(reader);
    REQUIRE(tag);
    CHECK(to_snbt(*tag) == R"({display:{Name:'"x"'}})");
    StringReader damage{"{Damage:0}"};
    CHECK(to_snbt(*read_snbt_compound(damage)) == "{Damage:0}");
    StringReader numbers{"{a:1b,b:2s,c:3L,d:1.5f,e:2.5d,f:2.5,g:true,h:[I;1,2],i:[1,2]}"};
    const auto   typed = read_snbt_compound(numbers);
    REQUIRE(typed);
    CHECK(to_snbt(*typed) == "{a:1b,b:2s,c:3L,d:1.5f,e:2.5d,f:2.5d,g:1b,h:[I;1,2],i:[1,2]}");
    StringReader mixed{"[1,2b]"};
    const auto   refused = read_snbt_value(mixed);
    REQUIRE_FALSE(refused);
    CHECK(to_json(refused.error().message) ==
          R"({"translate":"argument.nbt.list.mixed","with":["TAG_Byte","TAG_Int"]})");
}

TEST_CASE("snbt survives hostile input", "[commands][snbt][hostile]") {
    std::string deep = std::string(4000, '{') + std::string(4000, '}');
    StringReader reader{deep};
    (void)read_snbt_value(reader);
    u64 state = 1;
    for (int round = 0; round < 2000; ++round) {
        std::string noise;
        for (int i = 0; i < 30; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            noise.push_back("{}[],:;\"'\\ BILbsflfd0123456789.-+"[(state >> 33U) % 34]);
        }
        StringReader r{noise};
        (void)read_snbt_value(r);
    }
    SUCCEED();
}

// ── Rules, ops, the world's clock ───────────────────────────────────────────

TEST_CASE("gamerules store as strings and read them back", "[commands][rules]") {
    GameRules rules;
    CHECK(rules.flag("doDaylightCycle"));
    CHECK(rules.number("randomTickSpeed") == 3);
    rules.load({{"randomTickSpeed", "10"}, {"keepInventory", "TRUE"}, {"futureRule", "7"},
                {"spawnRadius", "garbage"}});
    CHECK(rules.number("randomTickSpeed") == 10);
    CHECK(rules.flag("keepInventory"));
    CHECK(rules.number("spawnRadius") == 10);
    const auto stored = rules.store();
    CHECK(stored.size() == kGameRules.size() + 1);
    CHECK(stored.back() == std::pair<std::string, std::string>{"futureRule", "7"});
}

TEST_CASE("ops.json is written in vanilla's shape and read back", "[commands][ops]") {
    OpList ops;
    CHECK(ops.to_json() == "[]");
    CHECK(ops.add(OpEntry{net::Uuid::offline_player("ovprobe"), "ovprobe", 4, false}));
    CHECK_FALSE(ops.add(OpEntry{net::Uuid::offline_player("ovprobe"), "ovprobe", 4, false}));
    const std::string text = ops.to_json();
    CHECK(text ==
          "[\n  {\n    \"uuid\": \"f3f20367-e988-37d8-ac20-1a1929fd1e59\",\n    \"name\": \"ovprobe\",\n"
          "    \"level\": 4,\n    \"bypassesPlayerLimit\": false\n  }\n]");
    OpList back;
    REQUIRE(back.from_json(text));
    CHECK(back.level_of(net::Uuid::offline_player("ovprobe")) == 4);
    CHECK(back.remove(net::Uuid::offline_player("ovprobe")));
}

TEST_CASE("the rain ramps in float steps and announces itself at 0.2", "[commands][weather]") {
    WorldState world;
    world.rules.set(*GameRules::index_of("doWeatherCycle"), 0);
    world.set_weather(0, 1000, true, false);
    std::vector<Broadcast> out;
    world.tick(out);
    REQUIRE(out.size() == 1);
    // The capture's first rain level: 0.009999999776482582, the float 0.01f.
    CHECK(out[0].payload == net::encode_game_event(7, 0.01F));
    out.clear();
    for (int i = 0; i < 25; ++i) {
        world.tick(out);
    }
    // Twenty ticks in, the level crosses 0.2 and "begin raining" (event 1)
    // goes out, followed by both levels again — the capture's order.
    bool began = false;
    for (usize i = 0; i + 2 < out.size(); ++i) {
        if (out[i].payload[0] == 1) {
            began = true;
            CHECK(out[i + 1].payload[0] == 7);
            CHECK(out[i + 2].payload[0] == 8);
        }
    }
    CHECK(began);
}

TEST_CASE("a frozen clock sends its time negated, and zero as -1", "[commands][time]") {
    WorldState world;
    world.rules.set(*GameRules::index_of("doDaylightCycle"), 0);
    world.day_time = 0;
    CHECK(world.update_time_payload() == net::encode_update_time(0, -1));
    world.day_time = 24100;
    CHECK(world.update_time_payload() == net::encode_update_time(0, -24100));
    world.rules.set(*GameRules::index_of("doDaylightCycle"), 1);
    CHECK(world.update_time_payload() == net::encode_update_time(0, 24100));
}

// ── The registry of parsers ─────────────────────────────────────────────────

TEST_CASE("the parser fallback table is the registry's", "[commands][registry]") {
    const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                      "generated" / "reports" / "registries.json";
    const auto bytes = io::read_file(path);
    if (!bytes) {
        SKIP("no data generator output");
    }
    usize      consumed = 0;
    const auto json     = parse_json(
        std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()}, consumed);
    REQUIRE(json);
    const JsonValue* entries = json->find("minecraft:command_argument_type")->find("entries");
    REQUIRE(entries != nullptr);
    const auto table = parser_id_table();
    REQUIRE(entries->object.size() == table.size());
    for (const auto& [name, entry] : entries->object) {
        const auto id = std::stoul(entry.find("protocol_id")->string);
        CHECK(table[id] == name);
    }
}

// ── Commands, against the capture ───────────────────────────────────────────

TEST_CASE("time answers exactly as vanilla", "[commands][vanilla]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    CHECK(h.run("time set 1000") ==
          std::vector<std::string>{R"({"translate":"commands.time.set","with":["1000"]})"});
    CHECK(h.run("time set noon") ==
          std::vector<std::string>{R"({"translate":"commands.time.set","with":["6000"]})"});
    h.run("time set 0");
    h.run("time add 100");
    CHECK(h.run("time add 1d") ==
          std::vector<std::string>{R"({"translate":"commands.time.set","with":["100"]})"});
    CHECK(h.run("time set 1.5") ==
          std::vector<std::string>{R"({"translate":"commands.time.set","with":["2"]})"});
    CHECK(h.run("time set -1") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"argument.time.tick_count_too_low","with":["0","-1"]}],"text":""})"});
    CHECK(h.run("time sett day") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"command.unknown.argument"}],"text":""})",
              R"({"color":"red","extra":[{"color":"gray","clickEvent":{"action":"suggest_command","value":"/time sett day"},"extra":[{"text":"time "},{"underlined":true,"color":"red","text":"sett day"},{"italic":true,"color":"red","translate":"command.context.here"}],"text":""}],"text":""})"});
    CHECK(h.run("time set day extra")[1] ==
          R"({"color":"red","extra":[{"color":"gray","clickEvent":{"action":"suggest_command","value":"/time set day extra"},"extra":[{"text":"..."},{"text":"e set day "},{"underlined":true,"color":"red","text":"extra"},{"italic":true,"color":"red","translate":"command.context.here"}],"text":""}],"text":""})");
    CHECK(h.run("time set day ")[1] ==
          R"({"color":"red","extra":[{"color":"gray","clickEvent":{"action":"suggest_command","value":"/time set day "},"extra":[{"text":"..."},{"text":"me set day"},{"underlined":true,"color":"red","text":" "},{"italic":true,"color":"red","translate":"command.context.here"}],"text":""}],"text":""})");
    CHECK(h.run("time")[0] ==
          R"({"color":"red","extra":[{"translate":"command.unknown.command"}],"text":""})");
}

TEST_CASE("the branch rule: tp's error lands where vanilla's does", "[commands][vanilla]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    // Column 6, not at "foo": the `destination` branch wins because it has no
    // error, and it stopped at 6.
    CHECK(h.run("tp @s 0 -60 0 foo")[1] ==
          R"({"color":"red","extra":[{"color":"gray","clickEvent":{"action":"suggest_command","value":"/tp @s 0 -60 0 foo"},"extra":[{"text":"tp @s "},{"underlined":true,"color":"red","text":"0 -60 0 foo"},{"italic":true,"color":"red","translate":"command.context.here"}],"text":""}],"text":""})");
    CHECK(h.run("gamemode creative @e")[1] ==
          R"({"color":"red","extra":[{"color":"gray","clickEvent":{"action":"suggest_command","value":"/gamemode creative @e"},"extra":[{"text":""},{"underlined":true,"color":"red","text":"gamemode creative @e"},{"italic":true,"color":"red","translate":"command.context.here"}],"text":""}],"text":""})");
    CHECK(h.run("kill @r[type=cow]")[0] ==
          R"({"color":"red","extra":[{"translate":"argument.entity.options.inapplicable","with":["type"]}],"text":""})");
    CHECK(h.run("setblock 0 -60 0 oak_stairs[facing=up]")[0] ==
          R"({"color":"red","extra":[{"translate":"argument.block.property.invalid","with":["minecraft:oak_stairs","up","facing"]}],"text":""})");
    CHECK(h.run("gamemode  creative")[0] ==
          R"({"color":"red","extra":[{"translate":"argument.gamemode.invalid","with":[""]}],"text":""})");
    CHECK(h.run("nosuchcommand")[0] ==
          R"({"color":"red","extra":[{"translate":"command.unknown.command"}],"text":""})");
}

TEST_CASE("tp moves the player and names it as vanilla does", "[commands][vanilla]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    CHECK(h.run("tp 10 -60 10") ==
          std::vector<std::string>{R"({"translate":"commands.teleport.success.location.single","with":[)" +
                                   kProbe + R"(,"10.500000","-60.000000","10.500000"]})"});
    CHECK(h.server.x == 10.5);
    CHECK(h.run("tp @s ~ ~5 ~")[0].find("\"-55.000000\"") != std::string::npos);
}

TEST_CASE("setblock and fill count as vanilla counts", "[commands][vanilla]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    CHECK(h.run("setblock 0 -60 0 stone") ==
          std::vector<std::string>{R"({"translate":"commands.setblock.success","with":["0","-60","0"]})"});
    CHECK(h.run("setblock 0 -60 0 stone") ==
          std::vector<std::string>{R"({"color":"red","extra":[{"translate":"commands.setblock.failed"}],"text":""})"});
    CHECK(h.run("setblock 0 400 0 stone") ==
          std::vector<std::string>{R"({"color":"red","extra":[{"translate":"argument.pos.outofworld"}],"text":""})"});
    h.run("setblock 0 -60 0 air");
    CHECK(h.run("fill 0 -60 0 3 -58 3 stone") ==
          std::vector<std::string>{R"({"translate":"commands.fill.success","with":["48"]})"});
    CHECK(h.run("fill 0 -60 0 3 -58 3 glass hollow") ==
          std::vector<std::string>{R"({"translate":"commands.fill.success","with":["48"]})"});
    CHECK(h.run("fill 0 -60 0 3 -58 3 air replace glass") ==
          std::vector<std::string>{R"({"translate":"commands.fill.success","with":["44"]})"});
    CHECK(h.run("fill 0 -60 0 3 -58 3 air destroy") ==
          std::vector<std::string>{R"({"color":"red","extra":[{"translate":"commands.fill.failed"}],"text":""})"});
    CHECK(h.run("fill 0 -60 0 200 -50 200 stone") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"commands.fill.toobig","with":["32768","444411"]}],"text":""})"});
}

TEST_CASE("give and clear agree with the inventory", "[commands][vanilla]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    const auto give = h.run("give @s diamond 5");
    REQUIRE(give.size() == 1);
    CHECK(give[0] ==
          R"({"translate":"commands.give.success.single","with":["5",{"color":"white","hoverEvent":{"action":"show_item","contents":{"id":"minecraft:diamond","count":5}},"translate":"chat.square_brackets","with":[{"extra":[{"translate":"item.minecraft.diamond"}],"text":""}]},)" +
              kProbe + "]}");
    CHECK(h.server.inventory[36].count == 5);
    const auto sword = h.run("give @s diamond_sword");
    CHECK(sword[0].find(R"("tag":"{Damage:0}")") != std::string::npos);
    h.run("give @s minecraft:stone 100");
    CHECK(h.server.inventory[38].count == 64);
    CHECK(h.server.inventory[39].count == 36);
    CHECK(h.run("give @s diamond 6401")[0].find("toomanyitems") != std::string::npos);
    CHECK(h.run("clear @s diamond 0")[0].find(R"("commands.clear.test.single","with":["5")") !=
          std::string::npos);
    CHECK(h.run("clear @s diamond 2")[0].find(R"("commands.clear.success.single","with":["2")") !=
          std::string::npos);
    h.run("clear @s");
    CHECK(h.run("clear @s") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"clear.failed.single","with":[{"text":"ovprobe"}]}],"text":""})"});
}

TEST_CASE("gamemode, difficulty and gamerule answer as vanilla", "[commands][vanilla]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    CHECK(h.run("gamemode survival") ==
          std::vector<std::string>{
              R"({"translate":"commands.gamemode.success.self","with":[{"translate":"gameMode.survival"}]})"});
    CHECK(h.server.game_mode == 0);
    CHECK(h.run("gamemode survival ovprobe").empty());
    CHECK(h.run("difficulty hard") ==
          std::vector<std::string>{
              R"({"translate":"commands.difficulty.success","with":[{"translate":"options.difficulty.hard"}]})"});
    CHECK(h.run("difficulty hard") ==
          std::vector<std::string>{
              R"({"color":"red","extra":[{"translate":"commands.difficulty.failure","with":["hard"]}],"text":""})"});
    CHECK(h.run("gamerule randomTickSpeed 10") ==
          std::vector<std::string>{R"({"translate":"commands.gamerule.set","with":["randomTickSpeed","10"]})"});
    CHECK(h.run("gamerule keepInventory") ==
          std::vector<std::string>{R"({"translate":"commands.gamerule.query","with":["keepInventory","false"]})"});
}

TEST_CASE("effect answers with vanilla's three arguments", "[commands][vanilla]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    // The capture: effect, target, and the duration in whole seconds — a
    // third argument the English pattern never prints.
    CHECK(h.run("effect give @s speed") ==
          std::vector<std::string>{R"({"translate":"commands.effect.give.success.single","with":[{"translate":"effect.minecraft.speed"},)" +
                                   kProbe + R"(,"30"]})"});
    CHECK(h.run("effect give @s speed 10 0") ==
          std::vector<std::string>{R"({"color":"red","extra":[{"translate":"commands.effect.give.failed"}],"text":""})"});
    CHECK(h.run("effect give @s speed infinite 2 true")[0].find(R"(,"0"]})") != std::string::npos);
    CHECK(h.run("effect clear @s speed") ==
          std::vector<std::string>{R"({"translate":"commands.effect.clear.specific.success.single","with":[{"translate":"effect.minecraft.speed"},)" +
                                   kProbe + "]}"});
    CHECK(h.run("effect clear @s speed") ==
          std::vector<std::string>{R"({"color":"red","extra":[{"translate":"commands.effect.clear.specific.failed"}],"text":""})"});
    CHECK(h.run("effect clear") ==
          std::vector<std::string>{R"({"color":"red","extra":[{"translate":"commands.effect.clear.everything.failed"}],"text":""})"});
    CHECK(h.run("effect give @s nosuch") ==
          std::vector<std::string>{R"({"color":"red","extra":[{"translate":"argument.resource.not_found","with":["minecraft:nosuch","minecraft:mob_effect"]}],"text":""})"});
}

TEST_CASE("killing a player announces the death first, and @e forgets the dead", "[commands][vanilla]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    const auto killed = h.run("kill @s");
    REQUIRE(killed.size() == 2);
    CHECK(killed[0] == R"({"translate":"death.attack.genericKill","with":[)" + kProbe + "]}");
    CHECK(killed[1] == R"({"translate":"commands.kill.success.single","with":[)" + kProbe + "]}");
    // The capture's `kill @e[gamemode=creative]` with the probe lying dead.
    CHECK(h.run("kill @e[gamemode=creative]") ==
          std::vector<std::string>{R"({"color":"red","extra":[{"translate":"argument.entity.notfound.entity"}],"text":""})"});
}

TEST_CASE("help prints vanilla's smart usage", "[commands][vanilla]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    CHECK(h.run("help time") == std::vector<std::string>{
                                    R"j({"text":"/time set (day|noon|night|midnight|<time>)"})j",
                                    R"j({"text":"/time add <time>"})j",
                                    R"j({"text":"/time query (daytime|gametime|day)"})j"});
    CHECK(h.run("help gamemode") == std::vector<std::string>{R"({"text":"/gamemode <gamemode> [<target>]"})"});
    CHECK(h.run("help tp").empty());
    // Every usage vanilla's `/help` printed for a command this server has.
    const std::vector<std::string> expected{
        "/clear [<targets>]",
        "/defaultgamemode <gamemode>",
        "/difficulty [peaceful|easy|normal|hard]",
        "/effect (clear|give)",
        "/me <action>",
        "/experience (add|set|query)",
        "/xp -> experience",
        "/fill <from> <to> <block> [replace|keep|outline|hollow|destroy]",
        "/gamemode <gamemode> [<target>]",
        "/give <targets> <item> [<count>]",
        "/help [<command>]",
        "/kick <targets> [<reason>]",
        "/kill [<targets>]",
        "/list [uuids]",
        "/msg <targets> <message>",
        "/tell -> msg",
        "/w -> msg",
        "/say <message>",
        "/seed",
        "/setblock <pos> <block> [destroy|keep|replace]",
        "/spawnpoint [<targets>]",
        "/setworldspawn [<pos>]",
        "/summon <entity> [<pos>]",
        "/teleport (<location>|<destination>|<targets>)",
        "/tp -> teleport",
        "/tellraw <targets> <message>",
        "/time (set|add|query)",
        "/title <targets> (clear|reset|title|subtitle|actionbar|times)",
        "/weather (clear|rain|thunder)",
        "/deop <targets>",
        "/op <targets>",
        "/save-all [flush]",
        "/stop"};
    // And gamerule, whose usage lists every rule.
    std::string gamerule = "/gamerule (";
    for (usize i = 0; i < kGameRules.size(); ++i) {
        gamerule += (i == 0 ? "" : "|") + std::string{kGameRules[i].name};
    }
    std::vector<std::string> with_rules = expected;
    with_rules.insert(with_rules.begin() + 9, gamerule + ")");
    std::vector<std::string> printed;
    for (const std::string& line : h.run("help")) {
        printed.push_back(line.substr(9, line.size() - 11));  // {"text":"…"}
    }
    CHECK(printed == with_rules);
}

TEST_CASE("the Commands packet reads back and gates by permission", "[commands][packet]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness     h;
    const auto  full = net::parse_commands(h.service.commands_packet(4));
    REQUIRE(full);
    const auto& root = full->nodes[static_cast<usize>(full->root)];
    std::vector<std::string> names;
    for (const i32 child : root.children) {
        names.push_back(full->nodes[static_cast<usize>(child)].name);
    }
    CHECK(names.front() == "clear");
    CHECK(names.back() == "stop");
    const auto everyone = net::parse_commands(h.service.commands_packet(0));
    REQUIRE(everyone);
    std::vector<std::string> open;
    for (const i32 child : everyone->nodes[static_cast<usize>(everyone->root)].children) {
        open.push_back(everyone->nodes[static_cast<usize>(child)].name);
    }
    // Vanilla's level-0 tree, less teammsg/tm/trigger which this server lacks.
    CHECK(open == std::vector<std::string>{"me", "help", "list", "msg", "tell", "w"});
}

TEST_CASE("suggestions answer with vanilla's ranges", "[commands][suggest]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness     h;
    CommandSource source = h.player();
    const std::vector<std::string> names{"ovprobe"};
    auto time_s = h.service.suggest(source, 3, "/time s", names);
    CHECK(time_s.start == 6);
    CHECK(time_s.length == 1);
    REQUIRE(time_s.matches.size() == 1);
    CHECK(time_s.matches[0].text == "set");
    auto tp = h.service.suggest(source, 9, "/tp ovp", names);
    CHECK(tp.start == 4);
    REQUIRE(tp.matches.size() == 1);
    CHECK(tp.matches[0].text == "ovprobe");
    auto stairs = h.service.suggest(source, 16, "/setblock ~ ~ ~ oak_st", names);
    std::vector<std::string> got;
    for (const auto& m : stairs.matches) {
        got.push_back(m.text);
    }
    CHECK(got == std::vector<std::string>{"minecraft:dark_oak_stairs", "minecraft:oak_stairs"});
    auto props = h.service.suggest(source, 17, "/setblock 0 0 0 oak_stairs[", names);
    got.clear();
    for (const auto& m : props.matches) {
        got.push_back(m.text);
    }
    CHECK(got == std::vector<std::string>{"]", "facing=", "half=", "shape=", "waterlogged="});
    CHECK(props.start == 27);
}

TEST_CASE("selectors choose and refuse as vanilla's do", "[commands][selector]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    const ParseEnv env{&*packs().blocks, &*packs().registries};
    std::vector<EntityInfo> world;
    for (int i = 0; i < 4; ++i) {
        EntityInfo cow;
        cow.id       = 10 + i;
        cow.type     = "minecraft:cow";
        cow.position = Vec3d{static_cast<f64>(i * 3), -60.0, 0.0};
        world.push_back(cow);
    }
    CommandSource source;
    source.position = Vec3d{0.0, -60.0, 0.0};
    const auto pick = [&](std::string_view text) {
        StringReader reader{text};
        auto         selector = parse_entity_selector(reader, env);
        REQUIRE(selector);
        auto found = find_entities(*selector, source, world, [](u32) { return 0U; }, &env);
        REQUIRE(found);
        std::vector<i32> ids;
        for (const EntityInfo* e : *found) {
            ids.push_back(e->id);
        }
        return ids;
    };
    CHECK(pick("@e[type=cow,limit=1,sort=nearest]") == std::vector<i32>{10});
    CHECK(pick("@e[type=cow,limit=1,sort=furthest]") == std::vector<i32>{13});
    CHECK(pick("@e[distance=..4]") == std::vector<i32>{10, 11});
    CHECK(pick("@e[type=!cow]").empty());
    CHECK(pick("@e[x=3,y=-60,z=0,dx=1,dy=1,dz=1]") == std::vector<i32>{11});
    CHECK(pick("@e[type=#minecraft:skeletons]").empty());

    const auto refused = [&](std::string_view text) {
        StringReader reader{text};
        auto         selector = parse_entity_selector(reader, env);
        REQUIRE_FALSE(selector);
        return std::make_pair(to_json(selector.error().message), *selector.error().cursor);
    };
    CHECK(refused("@q") == std::make_pair(std::string{R"({"translate":"argument.entity.selector.unknown","with":["@q"]})"}, usize{1}));
    CHECK(refused("@e[foo=1]").second == 3);
    CHECK(refused("@e[limit=0]").second == 9);
    CHECK(refused("@a[type=cow]").second == 7);
    CHECK(refused("@e[type=cow").first == R"({"translate":"argument.entity.options.unterminated"})");
    CHECK(refused("@e[distance=-1]").first ==
          R"({"translate":"argument.entity.options.distance.negative"})");
}

TEST_CASE("the dispatcher survives hostile input", "[commands][hostile]") {
    if (!have_packs()) {
        SKIP("no registry pack");
    }
    Harness h;
    u64     state = 42;
    const std::string alphabet = "@aeprs[]{}=,!~^.-0123456789 \"'\\:#/abcdefghijklmnopqrstuvwxyz";
    const std::vector<std::string> starts{"tp ", "give @s ", "setblock ~ ~ ~ ", "kill @e[", "fill 0 0 0 1 1 1 ",
                                          "tellraw @s ", "summon cow ~ ~ ~ ", "time set ", "gamerule "};
    for (int round = 0; round < 1500; ++round) {
        std::string command = starts[static_cast<usize>(round) % starts.size()];
        for (int i = 0; i < 24; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            command.push_back(alphabet[(state >> 33U) % alphabet.size()]);
        }
        (void)h.run(command);
        (void)h.service.suggest(h.player(), 1, "/" + command, std::vector<std::string>{"ovprobe"});
    }
    SUCCEED();
}
