// The dedicated server's administration, without a socket.
//
// What the files owe to the JDK (hash orders, Gson's escaping, the date
// patterns), server.properties' reading and writing, the three list files,
// the login gate's order, the RCON and Query codecs, the watchdog's arithmetic
// and compressed framing. Where an expected value is a file or a packet the
// real 1.20.1 server produced, the comment says so
// (scripts/capture_admin.py, docs/provenance/serveur-dedie.md).
#include "../src/admin/java_compat.hpp"
#include "../src/admin/query.hpp"
#include "../src/admin/rcon.hpp"
#include "../src/admin/server_admin.hpp"
#include "../src/admin/server_properties.hpp"
#include "../src/admin/status_icon.hpp"
#include "../src/admin/user_lists.hpp"
#include "../src/admin/watchdog.hpp"
#include "../src/commands/console.hpp"

#include "ov/protocol/framing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::server::admin;

namespace {

std::vector<std::string> keys_in(const std::vector<usize>& order,
                                 const std::vector<std::string>& keys) {
    std::vector<std::string> out;
    for (const usize i : order) {
        out.push_back(keys[i]);
    }
    return out;
}

}  // namespace

// ── The JDK ─────────────────────────────────────────────────────────────────

TEST_CASE("String.hashCode", "[admin][java]") {
    CHECK(java_string_hash("") == 0);
    CHECK(java_string_hash("hello") == 99162322);
    // The classic: a string whose hash is Integer.MIN_VALUE.
    CHECK(java_string_hash("polygenelubricants") == static_cast<i32>(0x80000000U));
    // Over UTF-16 units: "é" is one unit, U+1F600 two.
    CHECK(java_string_hash("\xC3\xA9") == 0xE9);
    CHECK(java_string_hash("\xF0\x9F\x98\x80") == 0xD83D * 31 + 0xDE00);
}

TEST_CASE("HashMap order is bucket order, insertion order within a bucket", "[admin][java]") {
    // "Aa" and "BB" share a hash (2112): the classic collision.
    REQUIRE(java_string_hash("Aa") == java_string_hash("BB"));
    // "a" (97) lands in bucket 1; "zz" (3904) would share bucket 0 with them.
    const std::vector<std::string> keys{"BB", "a", "Aa"};
    const auto                     order = keys_in(hash_map_order(keys, keys.size()), keys);
    const auto bb = std::ranges::find(order, "BB") - order.begin();
    const auto aa = std::ranges::find(order, "Aa") - order.begin();
    CHECK(bb + 1 == aa);  // same bucket, inserted first
}

TEST_CASE("ConcurrentHashMap keeps every key once", "[admin][java]") {
    ConcurrentHashMapOrder map{8};
    for (int i = 0; i < 100; ++i) {
        map.put("key-" + std::to_string(i));
        map.put("key-" + std::to_string(i / 2));
    }
    auto keys = map.keys();
    CHECK(keys.size() == 100);
    auto copy = map.copied(8).keys();
    std::ranges::sort(keys);
    std::ranges::sort(copy);
    CHECK(keys == copy);
}

TEST_CASE("Gson escapes HTML characters", "[admin][java]") {
    std::string out;
    append_gson_string(out, "a=b<c>'&\"\\\n\x01");
    CHECK(out == R"("a\u003db\u003cc\u003e\u0027\u0026\"\\\n\u0001")");
}

TEST_CASE("ban dates", "[admin][java]") {
    CHECK(format_ban_date(0, 3600) == "1970-01-01 01:00:00 +0100");
    CHECK(format_ban_date(0, -5 * 3600 - 1800) == "1969-12-31 18:30:00 -0530");
    const auto t = parse_ban_date("2099-01-02 03:04:05 +0000");
    REQUIRE(t);
    CHECK(format_ban_date(*t, 0) == "2099-01-02 03:04:05 +0000");
    CHECK(format_ban_date(*t, 7200) == "2099-01-02 05:04:05 +0200");
    CHECK(parse_ban_date("2026-09-11 18:40:12 +0200") == parse_ban_date("2026-09-11 16:40:12 +0000"));
    CHECK_FALSE(parse_ban_date("forever"));
    CHECK_FALSE(parse_ban_date("2026-13-11 18:40:12 +0200"));
    // Date.toString(): 2026-09-11 is a Friday.
    const i64 friday = *parse_ban_date("2026-09-11 18:40:12 +0200");
    CHECK(format_java_date(friday, 7200, "CEST") == "Fri Sep 11 18:40:12 CEST 2026");
    CHECK(format_zone_date(friday, 7200, "CEST") == "2026-09-11 18:40:12 CEST");
}

// ── server.properties ───────────────────────────────────────────────────────

TEST_CASE("Properties.load", "[admin][properties]") {
    const auto entries = parse_properties(
        "# comment\n"
        "! also a comment\n"
        "   \n"
        "level-type=minecraft\\:flat\n"
        "motd = spaced value \n"
        "key\\ with\\ space:colon\n"
        "unicode=Caf\\u00e9\n"
        "long=first \\\n"
        "    second\n"
        "bare\n"
        "tabs\t\tvalue\n"
        "dup=1\n"
        "dup=2\n");
    const auto get = [&](std::string_view key) -> std::string {
        for (const auto& [k, v] : entries) {
            if (k == key) {
                return v;
            }
        }
        return "<missing>";
    };
    CHECK(get("level-type") == "minecraft:flat");
    CHECK(get("motd") == "spaced value ");
    CHECK(get("key with space") == "colon");
    CHECK(get("unicode") == "Caf\xC3\xA9");
    CHECK(get("long") == "first second");
    CHECK(get("bare").empty());
    CHECK(get("tabs") == "value");
    CHECK(get("dup") == "2");
    CHECK(entries.size() == 8);
}

TEST_CASE("Properties.store escaping", "[admin][properties]") {
    CHECK(escape_property("minecraft:normal", false, true) == "minecraft\\:normal");
    CHECK(escape_property(" lead", false, true) == "\\ lead");
    CHECK(escape_property("in side", false, true) == "in side");
    CHECK(escape_property("a b", true, true) == "a\\ b");
    CHECK(escape_property("Caf\xC3\xA9", false, true) == "Caf\\u00E9");
    CHECK(escape_property("#!=\\", false, true) == "\\#\\!\\=\\\\");
    CHECK(escape_property("{}", false, true) == "{}");
}

TEST_CASE("properties bytes: UTF-8 when valid, else Latin-1", "[admin][properties]") {
    CHECK(decode_properties_bytes("Caf\xC3\xA9") == "Caf\xC3\xA9");
    CHECK(decode_properties_bytes("Caf\xE9") == "Caf\xC3\xA9");
}

TEST_CASE("every vanilla key is written, unknown keys are kept", "[admin][properties]") {
    ServerProperties p = ServerProperties::from_text("ov-unknown-key=kept\nmotd=Hi\n");
    const auto       s = DedicatedSettings::read(p);
    CHECK(s.motd == "Hi");
    CHECK(s.online_mode);
    const std::string text = p.render("Fri Sep 11 18:40:12 CEST 2026");
    CHECK(text.starts_with("#Minecraft server properties\n#Fri Sep 11 18:40:12 CEST 2026\n"));
    CHECK(text.find("\nov-unknown-key=kept\n") != std::string::npos);
    CHECK(text.find("\nlevel-type=minecraft\\:normal\n") != std::string::npos);
    for (const PropertyDefault& d : ServerProperties::vanilla_defaults()) {
        CHECK(text.find("\n" + std::string{d.key} + "=") != std::string::npos);
    }
}

TEST_CASE("values are read with vanilla's rules and rewritten as understood",
          "[admin][properties]") {
    ServerProperties p = ServerProperties::from_text(
        "max-players=abc\nview-distance=08\npvp=TRUE\nhardcore=yes\ndifficulty=3\n"
        "gamemode=creative\nfunction-permission-level=9\n");
    const auto s = DedicatedSettings::read(p);
    CHECK(s.max_players == 20);
    CHECK(*p.get("max-players") == "20");
    CHECK(s.view_distance == 8);
    CHECK(*p.get("view-distance") == "8");
    CHECK(s.pvp);
    CHECK(*p.get("pvp") == "true");
    CHECK_FALSE(s.hardcore);
    CHECK(*p.get("hardcore") == "false");
    CHECK(s.difficulty == 3);
    CHECK(*p.get("difficulty") == "hard");
    CHECK(s.gamemode == 1);
    CHECK(s.function_permission_level == 4);
}

// ── The list files ──────────────────────────────────────────────────────────

TEST_CASE("banned-players.json round trip", "[admin][lists]") {
    const WallClock clock = WallClock::fixed(*parse_ban_date("2026-09-11 18:40:12 +0200"), 7200, "CEST");
    BanList         bans{BanList::Kind::Players, {}, clock};
    const std::string file = R"([
  {
    "uuid": "f3f20367-e988-37d8-ac20-1a1929fd1e59",
    "name": "ovprobe",
    "created": "2026-09-11 10:00:00 +0200",
    "source": "Server",
    "expires": "forever",
    "reason": "a\u003db"
  }
])";
    REQUIRE(bans.from_json(file));
    REQUIRE(bans.entries().size() == 1);
    CHECK(bans.entries()[0].reason == "a=b");
    CHECK(bans.to_json() == file);
}

TEST_CASE("an expired ban is dropped when it is asked for", "[admin][lists]") {
    const WallClock clock = WallClock::fixed(*parse_ban_date("2026-09-11 18:40:12 +0200"), 0, "UTC");
    BanList         bans{BanList::Kind::Ips, {}, clock};
    REQUIRE(bans.from_json(R"([{"ip":"1.2.3.4","created":"2000-01-01 00:00:00 +0000",
        "source":"Server","expires":"2001-01-01 00:00:00 +0000","reason":"old"},
        {"ip":"5.6.7.8","created":"2000-01-01 00:00:00 +0000",
        "source":"Server","expires":"2099-01-01 00:00:00 +0000","reason":"new"}])"));
    CHECK(bans.entries().size() == 2);
    CHECK(bans.get("1.2.3.4") == nullptr);
    CHECK(bans.get("5.6.7.8") != nullptr);
    CHECK(bans.entries().size() == 1);
}

TEST_CASE("whitelist.json", "[admin][lists]") {
    WhiteList list{{}};
    CHECK(list.to_json() == "[]");
    REQUIRE(list.add({net::Uuid::offline_player("Alice"), "Alice"}));
    CHECK_FALSE(list.add({net::Uuid::offline_player("Alice"), "Alice"}));
    WhiteList again{{}};
    REQUIRE(again.from_json(list.to_json()));
    CHECK(again.names() == std::vector<std::string>{"Alice"});
}

TEST_CASE("the door: ban, whitelist, ip ban, full — in that order", "[admin][login]") {
    AdminConfig config;
    config.clock       = WallClock::fixed(1'000'000'000, 0, "UTC");
    config.max_players = 1;
    ServerAdmin admin{config};
    const auto  alice = net::Uuid::offline_player("Alice");
    CHECK_FALSE(admin.login_refusal(alice, "127.0.0.1", 0));
    CHECK(*admin.login_refusal(alice, "127.0.0.1", 1) ==
          R"({"translate":"multiplayer.disconnect.server_full"})");
    admin.with([&](ServerAdmin::Lists& lists) {
        BanEntry ip;
        ip.key = "127.0.0.1";
        lists.ips.add(ip);
        lists.white_list_enabled = true;
    });
    CHECK(*admin.login_refusal(alice, "127.0.0.1", 0) ==
          R"({"translate":"multiplayer.disconnect.not_whitelisted"})");
    admin.set_ops({OpPass{alice, true}});
    CHECK(admin.login_refusal(alice, "127.0.0.1", 0)->find("banned_ip") != std::string::npos);
    admin.with([&](ServerAdmin::Lists& lists) {
        BanEntry ban;
        ban.key    = alice.to_string();
        ban.reason = "Because";
        lists.players.add(ban);
    });
    CHECK(admin.login_refusal(alice, "127.0.0.1", 0)->find("banned.reason") != std::string::npos);
}

TEST_CASE("addresses", "[admin][login]") {
    CHECK(address_without_port("127.0.0.1:52000") == "127.0.0.1");
    CHECK(address_without_port("[::1]:52000") == "::1");
    CHECK(is_ip_address("10.0.0.1"));
    CHECK(is_ip_address("::1"));
    CHECK(is_ip_address("fe80::1:2"));
    CHECK_FALSE(is_ip_address("999.1.1.1"));
    CHECK_FALSE(is_ip_address("not.an.ip"));
    CHECK_FALSE(is_ip_address("1.2.3"));
    CHECK_FALSE(is_ip_address("01234.1.1.1"));
}

// ── RCON ────────────────────────────────────────────────────────────────────

TEST_CASE("RCON packets", "[admin][rcon]") {
    const auto bytes = encode_rcon({7, kRconLogin, "secret"});
    CHECK(bytes.size() == 4 + 10 + 6);
    CHECK(bytes[0] == 16);
    const auto back = decode_rcon(bytes);
    REQUIRE(back);
    CHECK(back->request == 7);
    CHECK(back->type == kRconLogin);
    CHECK(back->body == "secret");
    CHECK_FALSE(decode_rcon(std::span<const u8>{bytes}.first(bytes.size() - 1)));
    CHECK(split_rcon_response(1, "").size() == 1);
    CHECK(split_rcon_response(1, std::string(9000, 'x')).size() == 3);
    CHECK(split_rcon_response(1, std::string(4096, 'x')).size() == 1);
}

TEST_CASE("RCON session: a password, then commands", "[admin][rcon]") {
    RconSession session{"pw", false, [](std::string c) { return "ran " + c; }};
    auto        out = session.handle({3, kRconCommand, "seed"});
    CHECK(out.front().request == -1);
    out = session.handle({4, kRconLogin, "wrong"});
    CHECK(out.front().request == -1);
    out = session.handle({5, kRconLogin, "pw"});
    CHECK(out.front().request == 5);
    CHECK(out.front().type == kRconAuthOk);
    out = session.handle({6, kRconCommand, "seed"});
    CHECK(out.front().body == "ran seed");
    CHECK(out.front().type == kRconResponse);
    out = session.handle({7, 99, ""});
    CHECK(out.front().body == "Unknown request 63");
}

// ── Query ───────────────────────────────────────────────────────────────────

TEST_CASE("Query: handshake, then basic and full stat", "[admin][query]") {
    QueryInfo info;
    info.motd        = "A Minecraft Server";
    info.map         = "world";
    info.players     = {"Alice"};
    info.max_players = 20;
    info.host_port   = 25565;
    QueryResponder responder{[] { return 9513307; }};
    const std::vector<u8> hello{0xFE, 0xFD, 9, 0, 0, 0, 1};
    const auto            token = responder.answer(hello, "a", 0, info);
    REQUIRE(token);
    CHECK(std::string(token->begin() + 5, token->end()) == std::string{"9513307\0", 8});
    std::vector<u8> basic{0xFE, 0xFD, 0, 0, 0, 0, 1};
    for (const u8 b : std::array<u8, 4>{0x00, 0x91, 0x29, 0x5B}) {  // 9513307, big-endian
        basic.push_back(b);
    }
    const auto stat = responder.answer(basic, "a", 1000, info);
    REQUIRE(stat);
    CHECK(*stat == query_basic_stat(1, info));
    CHECK_FALSE(responder.answer(basic, "b", 1000, info));       // another sender
    CHECK_FALSE(responder.answer(basic, "a", 31'000, info));     // stale
    auto full = basic;
    full.insert(full.end(), {0, 0, 0, 0});
    REQUIRE(responder.answer(hello, "a", 40'000, info));
    const auto full_stat = responder.answer(full, "a", 40'000, info);
    REQUIRE(full_stat);
    CHECK(*full_stat == query_full_stat(1, info));
}

// ── Watchdog ────────────────────────────────────────────────────────────────

TEST_CASE("watchdog arithmetic", "[admin][watchdog]") {
    CHECK_FALSE(watchdog_overdue(0, 60'000, 60'000));
    CHECK(watchdog_overdue(0, 60'001, 60'000) == 60'001);
    CHECK_FALSE(watchdog_overdue(0, 1'000'000, -1));
    CHECK(watchdog_message(60'001) ==
          "A single server tick took 60.00 seconds (should be max 0.05)\n"
          "Considering it to be crashed, server will forcibly shutdown.");
    const auto report =
        watchdog_report(61'000, 60'000, *parse_ban_date("2026-09-11 18:40:12 +0200"), 7200, "x");
    CHECK(report.file_name == "crash-2026-09-11_18.40.12-server.txt");
    CHECK(report.text.starts_with("---- Minecraft Crash Report ----\n"));
}

// ── Compression ─────────────────────────────────────────────────────────────

TEST_CASE("frames re-framed for a compressed connection read back", "[admin][compression]") {
    std::vector<u8> frames;
    const std::vector<u8> small(10, 0x41);
    const std::vector<u8> large(1000, 0x42);
    for (const auto* body : {&small, &large}) {
        const auto framed = net::encode_packet(0x24, *body);
        REQUIRE(framed);
        frames.insert(frames.end(), framed->begin(), framed->end());
    }
    const auto compressed = net::compress_frames(frames, 256);
    REQUIRE(compressed);
    CHECK(compressed->size() < frames.size());
    net::FrameDecoder decoder;
    decoder.set_compression_threshold(256);
    decoder.feed(*compressed);
    const auto first = decoder.next();
    REQUIRE(first);
    CHECK(first->id == 0x24);
    CHECK(first->body == small);
    const auto second = decoder.next();
    REQUIRE(second);
    CHECK(second->body == large);
    CHECK_FALSE(net::compress_frames(std::span<const u8>{frames}.first(5), 256));
}

// ── The console's completion ────────────────────────────────────────────────

TEST_CASE("Tab completes one match, lists several", "[admin][console]") {
    using ov::server::cmd::complete_line;
    using ov::server::cmd::ConsoleSuggestions;
    CHECK(complete_line("tim", ConsoleSuggestions{0, 3, {"time"}}).line == "time ");
    const auto several = complete_line("time s", ConsoleSuggestions{5, 1, {"set", "sett"}});
    CHECK(several.line == "time set");
    CHECK(several.listing == std::vector<std::string>{"set", "sett"});
    const auto differ = complete_line("ban", ConsoleSuggestions{0, 3, {"ban", "ban-ip", "banlist"}});
    CHECK(differ.line == "ban");
    CHECK(differ.listing.size() == 3);
    CHECK(complete_line("xyz", ConsoleSuggestions{}).line == "xyz");
}

// ── The icon ────────────────────────────────────────────────────────────────

TEST_CASE("base64, RFC 4648's vectors", "[admin][icon]") {
    const auto b64 = [](std::string_view text) {
        return base64(std::span<const u8>{reinterpret_cast<const u8*>(text.data()), text.size()});
    };
    CHECK(b64("").empty());
    CHECK(b64("f") == "Zg==");
    CHECK(b64("fo") == "Zm8=");
    CHECK(b64("foo") == "Zm9v");
    CHECK(b64("foob") == "Zm9vYg==");
    CHECK(b64("fooba") == "Zm9vYmE=");
    CHECK(b64("foobar") == "Zm9vYmFy");
}

TEST_CASE("server-icon.png must be a 64x64 PNG", "[admin][icon]") {
    // Signature, then IHDR's length, its name, width and height.
    const auto header = [](u32 width, u32 height) {
        std::vector<u8> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
        for (const u32 v : {width, height}) {
            for (const u32 shift : {24U, 16U, 8U, 0U}) {
                png.push_back(static_cast<u8>((v >> shift) & 0xFFU));
            }
        }
        return png;
    };
    const auto good = status_icon(header(64, 64));
    REQUIRE(good);
    CHECK(good->starts_with("data:image/png;base64,iVBORw0KGgo"));
    CHECK(status_icon(header(32, 64)).error() == "Must be 64 pixels wide");
    CHECK(status_icon(header(64, 65)).error() == "Must be 64 pixels high");
    CHECK_FALSE(status_icon(std::vector<u8>(30, 0)).has_value());
}

// ── server.properties on disk ───────────────────────────────────────────────

TEST_CASE("a first start writes the file, a second one finds it", "[admin][properties]") {
    const auto dir = std::filesystem::temp_directory_path() / "ov_admin_properties_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "server.properties";

    const PropertiesFile first = load_properties_file(path, "Fri Sep 11 18:40:12 CEST 2026");
    CHECK_FALSE(first.existed);
    REQUIRE(std::filesystem::exists(path));

    const PropertiesFile second = load_properties_file(path, "Fri Sep 11 18:41:00 CEST 2026");
    CHECK(second.existed);
    CHECK(second.settings.motd == "A Minecraft Server");
    CHECK(second.settings.server_port == 25565);
    // Every key once — but not in the same order both times, and that is the
    // JDK's doing, not ours: a second start loads the keys in the order the
    // first one wrote them, and two keys that share a bucket come back
    // swapped. OpenJDK 17 does exactly this (scripts/jdk_order_oracle.java,
    // "restart"): gamemode and enable-command-block trade places, nothing else.
    std::vector<std::string> restarted = first.properties.write_order();
    REQUIRE(restarted.size() == ServerProperties::vanilla_defaults().size());
    REQUIRE(restarted[1] == "gamemode");
    std::swap(restarted[1], restarted[2]);
    CHECK(second.properties.write_order() == restarted);
    std::filesystem::remove_all(dir);
}

// ── Against the JDK itself ──────────────────────────────────────────────────
//
// The expected orders below were printed by OpenJDK 17.0.18 running
// scripts/jdk_order_oracle.java — plain java.util, no game code — so these
// tests hold our re-specification of the two maps to the real ones.

namespace {

std::vector<std::string> split_commas(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const auto comma = text.find(',');
        out.emplace_back(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return out;
}

constexpr std::string_view kJdkFresh =
    "rcon.port,gamemode,enable-command-block,pvp,max-chained-neighbor-updates,"
    "network-compression-threshold,max-tick-time,max-players,online-mode,resource-pack-prompt,"
    "allow-nether,resource-pack-id,hide-online-players,rcon.password,force-gamemode,white-list,"
    "spawn-npcs,log-ips,function-permission-level,initial-enabled-packs,level-type,"
    "text-filtering-config,max-world-size,enable-jmx-monitoring,level-seed,enable-query,"
    "generator-settings,enforce-secure-profile,level-name,motd,query.port,generate-structures,"
    "difficulty,require-resource-pack,use-native-transport,enable-status,allow-flight,"
    "initial-disabled-packs,broadcast-rcon-to-ops,view-distance,server-ip,server-port,enable-rcon,"
    "sync-chunk-writes,op-permission-level,prevent-proxy-connections,resource-pack,"
    "entity-broadcast-range-percentage,simulation-distance,player-idle-timeout,rate-limit,hardcore,"
    "broadcast-console-to-ops,spawn-animals,spawn-monsters,enforce-whitelist,resource-pack-sha1,"
    "spawn-protection";

}  // namespace

TEST_CASE("the Properties copy iterates as JDK 17's does", "[admin][java][jdk]") {
    // A first start: every key put in the order the settings are read.
    ServerProperties fresh = ServerProperties::from_text("");
    fresh.fill_defaults();
    CHECK(fresh.write_order() == split_commas(kJdkFresh));

    // The capture's file: three keys loaded first, one of them unknown.
    ServerProperties capture = ServerProperties::from_text(
        "server-port=25610\nov-unknown-key=kept\nmotd=Caf\\u00e9 \\: \\= x\n");
    capture.fill_defaults();
    std::vector<std::string> expected = split_commas(kJdkFresh);
    expected.insert(std::ranges::find(expected, "max-tick-time"), "ov-unknown-key");
    CHECK(capture.write_order() == expected);
}

TEST_CASE("HashMap iterates as JDK 17's does", "[admin][java][jdk]") {
    std::vector<std::string> uuids;
    for (const char* name : {"Ovq_tempban", "Ovq_oldban", "Ovq_alice", "Ovq_bobby", "ovprobe",
                             "Alice", "Bob", "Carol", "Dave", "Erin", "Frank", "Grace", "Heidi",
                             "Ivan"}) {
        uuids.push_back(net::Uuid::offline_player(name).to_string());
    }
    CHECK(keys_in(hash_map_order(uuids, uuids.size()), uuids) ==
          split_commas("faa5dca3-c3d4-354b-ae1b-dde9e5a14b3b,85bd460a-256b-3c2e-a2e4-cf58580daba7,"
                       "ba22f360-8433-3f48-8ed8-da98139f3902,1937f6bb-cb53-3f3b-88bc-27e282673699,"
                       "80333097-598c-3d5f-9b99-4ef1a3920f06,a976eef7-9126-33f9-b8be-ef2e1d406d93,"
                       "812819a3-82f4-34ad-8e7e-da5e5eb26deb,e19dc097-ff1e-319c-ab80-b677c3c3bde4,"
                       "10920508-d5d8-3eed-93d2-92f193afe7d7,0af3f783-cbb9-32f0-953c-0d7e29e82d58,"
                       "f3f20367-e988-37d8-ac20-1a1929fd1e59,fa39c8d7-19b0-3b25-910f-a8e7f8d7f190,"
                       "c01bb543-3c4b-30c2-a587-85f5d36f788a,6ae9f2b8-00b0-3749-a576-b5a51f6417b9"));
    const std::vector<std::string> ips{"10.9.9.9", "10.0.0.1", "10.0.0.2", "127.0.0.1", "10.0.0.3"};
    CHECK(keys_in(hash_map_order(ips, ips.size()), ips) ==
          split_commas("10.0.0.3,10.0.0.2,10.0.0.1,10.9.9.9,127.0.0.1"));
}
