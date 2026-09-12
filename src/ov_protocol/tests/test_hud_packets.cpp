// Boss bars, the world border, the scoreboard, statistics and advancement tabs.
//
// The expected bytes are written out by hand from the field tables of the
// frozen protocol page, not produced by our own encoder: a round trip alone
// passes when both halves agree on the same mistake.

#include "ov/io/byte_writer.hpp"
#include "ov/protocol/hud.hpp"
#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::net;

namespace {

using Bytes = std::vector<u8>;

Bytes with_trailing_byte(Bytes bytes) {
    bytes.push_back(0x00);
    return bytes;
}

Bytes without_last_byte(Bytes bytes) {
    bytes.pop_back();
    return bytes;
}

const Uuid kBarUuid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

}  // namespace

// ── World border ────────────────────────────────────────────────────────────

TEST_CASE("Set Border Size is one big-endian double", "[protocol][hud]") {
    const Bytes expected{0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // 1.0
    CHECK(encode_set_border_size(1.0) == expected);
    CHECK(parse_set_border_size(expected) == std::optional<f64>{1.0});
    CHECK_FALSE(parse_set_border_size(without_last_byte(expected)).has_value());
    CHECK_FALSE(parse_set_border_size(with_trailing_byte(expected)).has_value());
}

TEST_CASE("the border's lerp time is a VarLong, not a VarInt", "[protocol][hud]") {
    // 2^40 ms takes 41 bits: six VarLong bytes. A VarInt reader stops at five
    // and then misreads everything after — the one field where the protocol page
    // and minecraft-data disagree.
    const BorderLerp lerp{10.0, 20.0, i64{1} << 40};
    const Bytes      encoded = encode_set_border_lerp_size(lerp);
    REQUIRE(encoded.size() == 16 + 6);
    CHECK(Bytes(encoded.begin() + 16, encoded.end()) == Bytes{0x80, 0x80, 0x80, 0x80, 0x80, 0x20});
    CHECK(parse_set_border_lerp_size(encoded) == std::optional<BorderLerp>{lerp});
}

TEST_CASE("Initialize World Border carries every field in page order", "[protocol][hud]") {
    WorldBorderInit border;
    border.x                        = 0.5;
    border.z                        = -0.5;
    border.old_diameter             = 100.0;
    border.new_diameter             = 50.0;
    border.lerp_ms                  = 60'000;
    border.portal_teleport_boundary = 29'999'984;
    border.warning_blocks           = 5;
    border.warning_time             = 15;

    const Bytes encoded = encode_initialize_world_border(border);
    // Four doubles, then 60000 (3 bytes), 29999984 (4 bytes), 5 and 15.
    REQUIRE(encoded.size() == 32 + 3 + 4 + 1 + 1);
    CHECK(encoded[32] == 0xE0);  // 60000 = 0b11_1010100_1100000
    CHECK(encoded[33] == 0xD4);
    CHECK(encoded[34] == 0x03);
    CHECK(encoded[39] == 0x05);
    CHECK(encoded[40] == 0x0F);
    CHECK(parse_initialize_world_border(encoded) == std::optional<WorldBorderInit>{border});
    CHECK_FALSE(parse_initialize_world_border(without_last_byte(encoded)).has_value());
    CHECK_FALSE(parse_initialize_world_border(with_trailing_byte(encoded)).has_value());
}

TEST_CASE("the small border packets round-trip", "[protocol][hud]") {
    CHECK(parse_set_border_center(encode_set_border_center(12.5, -3.0)) ==
          std::optional<BorderCenter>{BorderCenter{12.5, -3.0}});
    CHECK(encode_set_border_warning_delay(15) == Bytes{0x0F});
    CHECK(parse_set_border_warning_delay(Bytes{0x0F}) == std::optional<i32>{15});
    CHECK(encode_set_border_warning_distance(300) == Bytes{0xAC, 0x02});
    CHECK(parse_set_border_warning_distance(Bytes{0xAC, 0x02}) == std::optional<i32>{300});
    CHECK_FALSE(parse_set_border_warning_distance(Bytes{0xAC}).has_value());
}

// ── Boss Bar ────────────────────────────────────────────────────────────────

TEST_CASE("a Boss Bar remove is the UUID and the action, nothing more", "[protocol][hud]") {
    BossBar bar;
    bar.uuid   = kBarUuid;
    bar.action = BossBarAction::Remove;
    const Bytes expected{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE,
                         0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10, 0x01};
    CHECK(encode_boss_bar(bar) == expected);

    const auto decoded = parse_boss_bar(expected);
    REQUIRE(decoded.has_value());
    CHECK(decoded->uuid.most_significant == kBarUuid.most_significant);
    CHECK(decoded->uuid.least_significant == kBarUuid.least_significant);
    CHECK(decoded->action == BossBarAction::Remove);
    CHECK_FALSE(parse_boss_bar(with_trailing_byte(expected)).has_value());
}

TEST_CASE("a Boss Bar add lays out title, health, colour, division, flags", "[protocol][hud]") {
    BossBar bar;
    bar.uuid       = kBarUuid;
    bar.action     = BossBarAction::Add;
    bar.title_json = R"({"text":"B"})";
    bar.health     = 0.5F;
    bar.color      = 2;  // red
    bar.division   = 4;  // 20 notches
    bar.flags      = kBossBarDarkenSky | kBossBarFog;

    const Bytes encoded = encode_boss_bar(bar);
    const Bytes tail(encoded.begin() + 16, encoded.end());
    const Bytes expected{0x00,  // add
                         0x0C, '{',  '"', 't', 'e',  'x',  't',  '"',  ':',
                         '"',  'B',  '"', '}', 0x3F, 0x00, 0x00, 0x00,  // 0.5F
                         0x02, 0x04, 0x05};
    CHECK(tail == expected);

    const auto decoded = parse_boss_bar(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->title_json == bar.title_json);
    CHECK(decoded->health == 0.5F);
    CHECK(decoded->color == 2);
    CHECK(decoded->division == 4);
    CHECK(decoded->flags == 0x05);
}

TEST_CASE("every Boss Bar update action round-trips", "[protocol][hud]") {
    for (const auto action : {BossBarAction::UpdateHealth, BossBarAction::UpdateTitle,
                              BossBarAction::UpdateStyle, BossBarAction::UpdateFlags}) {
        BossBar bar;
        bar.uuid   = kBarUuid;
        bar.action = action;
        if (action == BossBarAction::UpdateHealth) {
            bar.health = 0.25F;
        } else if (action == BossBarAction::UpdateTitle) {
            bar.title_json = R"({"text":"Wither"})";
        } else if (action == BossBarAction::UpdateStyle) {
            bar.color    = 5;
            bar.division = 1;
        } else {
            bar.flags = kBossBarDragonMusic;
        }
        const auto decoded = parse_boss_bar(encode_boss_bar(bar));
        REQUIRE(decoded.has_value());
        CHECK(decoded->action == action);
        CHECK(decoded->health == bar.health);
        CHECK(decoded->title_json == bar.title_json);
        CHECK(decoded->color == bar.color);
        CHECK(decoded->division == bar.division);
        CHECK(decoded->flags == bar.flags);
    }
}

TEST_CASE("a Boss Bar outside its tables is refused", "[protocol][hud]") {
    BossBar style;
    style.uuid   = kBarUuid;
    style.action = BossBarAction::UpdateStyle;
    style.color  = kBossBarColors;  // one past white
    CHECK_FALSE(parse_boss_bar(encode_boss_bar(style)).has_value());
    style.color    = 0;
    style.division = kBossBarDivisions;
    CHECK_FALSE(parse_boss_bar(encode_boss_bar(style)).has_value());

    io::ByteWriter unknown;
    write_uuid(unknown, kBarUuid);
    write_varint(unknown, 6);  // no such action
    CHECK_FALSE(parse_boss_bar(unknown.data()).has_value());
}

// ── Scoreboard ──────────────────────────────────────────────────────────────

TEST_CASE("Display Objective is a signed byte and a name", "[protocol][hud]") {
    const DisplayObjective sidebar{1, "kills"};
    const Bytes            expected{0x01, 0x05, 'k', 'i', 'l', 'l', 's'};
    CHECK(encode_display_objective(sidebar) == expected);
    CHECK(parse_display_objective(expected) == std::optional<DisplayObjective>{sidebar});
    // 18 is the last team sidebar; 19 and -1 are nothing.
    CHECK(parse_display_objective(Bytes{0x12, 0x00}).has_value());
    CHECK_FALSE(parse_display_objective(Bytes{0x13, 0x00}).has_value());
    CHECK_FALSE(parse_display_objective(Bytes{0xFF, 0x00}).has_value());
}

TEST_CASE("Update Objectives carries text and type only when not removing", "[protocol][hud]") {
    UpdateObjectives create{"deaths", ObjectiveMode::Create, R"({"text":"Deaths"})",
                            ObjectiveRender::Hearts};
    CHECK(parse_update_objectives(encode_update_objectives(create)) ==
          std::optional<UpdateObjectives>{create});

    const UpdateObjectives remove{"deaths", ObjectiveMode::Remove, "", ObjectiveRender::Integer};
    const Bytes            expected{0x06, 'd', 'e', 'a', 't', 'h', 's', 0x01};
    CHECK(encode_update_objectives(remove) == expected);
    CHECK(parse_update_objectives(expected) == std::optional<UpdateObjectives>{remove});
    CHECK_FALSE(parse_update_objectives(with_trailing_byte(expected)).has_value());

    CHECK_FALSE(parse_update_objectives(Bytes{0x01, 'x', 0x03}).has_value());  // mode 3
    Bytes bad_render  = encode_update_objectives(create);
    bad_render.back() = 0x02;  // neither integer nor hearts
    CHECK_FALSE(parse_update_objectives(bad_render).has_value());
}

TEST_CASE("Update Teams lays out each mode's fields", "[protocol][hud]") {
    UpdateTeams create;
    create.team                     = "red";
    create.mode                     = TeamMode::Create;
    create.info.display_json        = R"({"text":"Red"})";
    create.info.friendly_flags      = kTeamFriendlyFire | kTeamSeeInvisible;
    create.info.name_tag_visibility = "hideForOtherTeams";
    create.info.collision_rule      = "pushOwnTeam";
    create.info.color               = 12;  // red
    create.info.prefix_json         = R"({"text":"[R] "})";
    create.info.suffix_json         = R"({"text":""})";
    create.entities                 = {"Steve", "Alex"};
    CHECK(parse_update_teams(encode_update_teams(create)) == std::optional<UpdateTeams>{create});

    UpdateTeams remove;
    remove.team = "red";
    remove.mode = TeamMode::Remove;
    CHECK(encode_update_teams(remove) == Bytes{0x03, 'r', 'e', 'd', 0x01});
    CHECK(parse_update_teams(Bytes{0x03, 'r', 'e', 'd', 0x01}) ==
          std::optional<UpdateTeams>{remove});

    UpdateTeams join;
    join.team     = "red";
    join.mode     = TeamMode::AddEntities;
    join.entities = {"Steve"};
    CHECK(encode_update_teams(join) ==
          Bytes{0x03, 'r', 'e', 'd', 0x03, 0x01, 0x05, 'S', 't', 'e', 'v', 'e'});
    CHECK(parse_update_teams(encode_update_teams(join)) == std::optional<UpdateTeams>{join});
}

TEST_CASE("Update Teams refuses a lying count before allocating for it", "[protocol][hud]") {
    // Mode 3 claiming 2^31-1 entities in a six-byte packet.
    const Bytes lying{0x01, 'r', 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0x07};
    CHECK_FALSE(parse_update_teams(lying).has_value());
    CHECK_FALSE(parse_update_teams(Bytes{0x01, 'r', 0x05}).has_value());  // mode 5

    UpdateTeams bad_color;
    bad_color.team       = "t";
    bad_color.mode       = TeamMode::UpdateInfo;
    bad_color.info.color = kTeamColors;
    CHECK_FALSE(parse_update_teams(encode_update_teams(bad_color)).has_value());
}

TEST_CASE("Update Score sends a value only for a change", "[protocol][hud]") {
    const UpdateScore change{"Steve", ScoreAction::Change, "kills", 300};
    CHECK(encode_update_score(change) ==
          Bytes{0x05, 'S', 't', 'e', 'v', 'e', 0x00, 0x05, 'k', 'i', 'l', 'l', 's', 0xAC, 0x02});
    CHECK(parse_update_score(encode_update_score(change)) == std::optional<UpdateScore>{change});

    const UpdateScore remove{"Steve", ScoreAction::Remove, "kills", 0};
    const Bytes       removed{0x05, 'S', 't', 'e', 'v', 'e', 0x01, 0x05, 'k', 'i', 'l', 'l', 's'};
    CHECK(encode_update_score(remove) == removed);
    CHECK(parse_update_score(removed) == std::optional<UpdateScore>{remove});
    // A value after a removal is a field read out of place.
    CHECK_FALSE(parse_update_score(with_trailing_byte(removed)).has_value());
    Bytes bad_action = removed;
    bad_action[6]    = 0x02;
    CHECK_FALSE(parse_update_score(bad_action).has_value());
}

// ── Statistics ──────────────────────────────────────────────────────────────

TEST_CASE("Award Statistics is a counted list of three VarInts", "[protocol][hud]") {
    const std::array<Statistic, 2> stats{Statistic{8, 5, 300}, Statistic{0, 1, 1}};
    const Bytes                    encoded = encode_award_statistics(stats);
    CHECK(encoded == Bytes{0x02, 0x08, 0x05, 0xAC, 0x02, 0x00, 0x01, 0x01});
    const auto decoded = parse_award_statistics(encoded);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == std::vector<Statistic>(stats.begin(), stats.end()));

    CHECK(parse_award_statistics(Bytes{0x00}) ==
          std::optional<std::vector<Statistic>>{std::vector<Statistic>{}});
    // Two entries announced, one sent; and a count no payload could hold.
    CHECK_FALSE(parse_award_statistics(Bytes{0x02, 0x08, 0x05, 0x01}).has_value());
    CHECK_FALSE(parse_award_statistics(Bytes{0xFF, 0xFF, 0xFF, 0xFF, 0x07}).has_value());
}

// ── Advancement tabs ────────────────────────────────────────────────────────

TEST_CASE("Select Advancements Tab is a flag and an optional identifier", "[protocol][hud]") {
    CHECK(encode_select_advancements_tab(SelectAdvancementsTab{}) == Bytes{0x00});
    CHECK(parse_select_advancements_tab(Bytes{0x00}) ==
          std::optional<SelectAdvancementsTab>{SelectAdvancementsTab{}});

    const SelectAdvancementsTab story{std::string{"minecraft:story/root"}};
    CHECK(parse_select_advancements_tab(encode_select_advancements_tab(story)) ==
          std::optional<SelectAdvancementsTab>{story});
    CHECK_FALSE(parse_select_advancements_tab(Bytes{0x02}).has_value());
    CHECK_FALSE(parse_select_advancements_tab(Bytes{0x00, 0x00}).has_value());
}

TEST_CASE("Seen Advancements carries a tab only when one was opened", "[protocol][hud]") {
    const SeenAdvancements opened{SeenAdvancementsAction::OpenedTab, "minecraft:nether/root"};
    CHECK(parse_seen_advancements(encode_seen_advancements(opened)) ==
          std::optional<SeenAdvancements>{opened});

    const SeenAdvancements closed{SeenAdvancementsAction::ClosedScreen, ""};
    CHECK(encode_seen_advancements(closed) == Bytes{0x01});
    CHECK(parse_seen_advancements(Bytes{0x01}) == std::optional<SeenAdvancements>{closed});
    // No presence flag: after a close, any byte is one too many.
    CHECK_FALSE(parse_seen_advancements(Bytes{0x01, 0x00}).has_value());
    CHECK_FALSE(parse_seen_advancements(Bytes{0x02}).has_value());
}
