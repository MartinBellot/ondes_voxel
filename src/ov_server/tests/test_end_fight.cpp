// The dragon fight: its start, its hits, its crystals and their blasts, its
// death (animated or not), its save, its respawn.
#include "../src/end_fight.hpp"
#include "../src/end_travel.hpp"
#include "../src/survival_session.hpp"

#include "ov/gameplay/damage.hpp"
#include "ov/protocol/survival.hpp"

#include "ov/gameplay/end_portal.hpp"
#include "ov/protocol/blast.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/end.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] const registry::BlockRegistry& blocks() {
    static const auto pack = registry::BlockRegistry::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    REQUIRE(pack.has_value());
    return *pack;
}

class MapLevel final : public world::LevelWriter {
public:
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        const auto found = cells_.find(key(pos));
        return found == cells_.end() ? registry::kAirState : found->second;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape      shape() const override { return world::WorldShape::the_end(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {false, false}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return ::blocks(); }
    void set_block(BlockPos pos, registry::BlockStateId state) override {
        if (state == registry::kAirState) {
            cells_.erase(key(pos));
        } else {
            cells_[key(pos)] = state;
        }
    }
    void schedule_tick(BlockPos, std::string_view, i64, world::TickQueue,
                       world::TickPriority) override {}
    [[nodiscard]] bool has_scheduled_tick(BlockPos, std::string_view,
                                          world::TickQueue) const override {
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return 0; }

    [[nodiscard]] std::string_view name_at(BlockPos pos) const {
        const auto state = block_at(pos);
        return state == registry::kAirState ? "minecraft:air"
                                            : ::blocks().block_name(::blocks().block_of(state));
    }

private:
    using Key = std::tuple<i32, i32, i32>;
    [[nodiscard]] static Key key(BlockPos pos) { return {pos.x, pos.y, pos.z}; }
    std::map<Key, registry::BlockStateId> cells_;
};

struct Recorder {
    std::vector<i32>                   ids;
    std::vector<std::pair<i32, i32>>   sent;  // (player, packet)
    std::vector<std::pair<i32, f32>>   hurts;
    i32                                experience{0};
    i32                                next{100};
    std::vector<EndFightPlayer>        people;
    EndFightHost                       host() {
        EndFightHost h;
        h.broadcast          = [this](i32 id, std::span<const u8>) { ids.push_back(id); };
        h.reserve_entity_ids = [this](i32 count) {
            const i32 first = next;
            next += count;
            return first;
        };
        h.players = [this](std::vector<EndFightPlayer>& out) {
            out.insert(out.end(), people.begin(), people.end());
        };
        h.send_to = [this](i32 player, i32 id, std::span<const u8>) {
            sent.emplace_back(player, id);
        };
        h.hurt_player = [this](i32 player, f32 amount, gameplay::DamageKind) {
            hurts.emplace_back(player, amount);
            return true;
        };
        h.award_experience = [this](i32, i32 value) { experience += value; };
        return h;
    }
};

constexpr i64 kSeed = 1234567890;

[[nodiscard]] std::vector<registry::BlockId> ids(std::initializer_list<std::string_view> names) {
    std::vector<registry::BlockId> out;
    for (const std::string_view name : names) {
        if (const auto block = blocks().find_block(name)) {
            out.push_back(*block);
        }
    }
    return out;
}

[[nodiscard]] EndFight make_fight(const gameplay::EndPortalRules& rules) {
    static const auto immune = ids({"minecraft:bedrock", "minecraft:obsidian", "minecraft:end_stone",
                                    "minecraft:iron_bars", "minecraft:end_portal",
                                    "minecraft:end_gateway"});
    static const auto transparent = ids({"minecraft:fire", "minecraft:light"});
    return EndFight{rules, blocks(), kSeed, EndFightTypes{}, immune, transparent};
}

/// A floor of end stone round the origin at y = 62, wide enough for the
/// breath, which lands nine or ten blocks out from the fountain.
void island(MapLevel& level) {
    const auto stone = blocks().default_state(*blocks().find_block("minecraft:end_stone"));
    for (i32 x = -20; x <= 20; ++x) {
        for (i32 z = -20; z <= 20; ++z) {
            level.set_block({x, 62, z}, stone);
        }
    }
}

}  // namespace

TEST_CASE("the fight starts with the exit portal, the dragon and ten crystals", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    REQUIRE(rules.valid());
    MapLevel  level;
    Recorder  record;
    EndFight  fight = make_fight(rules);
    const auto host = record.host();
    const auto stone = blocks().default_state(*blocks().find_block("minecraft:end_stone"));
    for (i32 y = 0; y < 62; ++y) {
        level.set_block({0, y, 0}, stone);
    }
    fight.start(level, 62, host);
    REQUIRE(fight.started());
    CHECK(fight.dragon_alive());
    CHECK(fight.crystals_alive() == 10);
    REQUIRE(fight.portal().has_value());
    CHECK(*fight.portal() == BlockPos{0, 61, 0});
    CHECK(level.name_at({0, 61, 0}) == "minecraft:bedrock");
    CHECK(level.name_at({1, 61, 0}) == "minecraft:air");  // not yet active
    CHECK(fight.dragon_id() == 100);  // nine ids: the dragon and its parts

    // Shown to a player: the dragon, its metadata, the boss bar, the crystals.
    std::vector<i32> shown;
    fight.show_to([&](i32 id, std::span<const u8>) { shown.push_back(id); });
    CHECK(std::ranges::count(shown, net::clientbound::kSpawnEntity) == 11);
    CHECK(std::ranges::count(shown, kBossBarPacket) == 1);
    CHECK(EndFight::within_range(Vec3d{100.5, 50.0, 0.5}));
    CHECK_FALSE(EndFight::within_range(Vec3d{300.0, 50.0, 0.0}));
}

TEST_CASE("a hit on the head is whole, on the body a quarter plus one", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    MapLevel  level;
    Recorder  record;
    EndFight  fight = make_fight(rules);
    const auto host = record.host();
    fight.start(level, 62, host);
    const i32 dragon = fight.dragon_id();

    CHECK(fight.hurt(dragon + 1, 8.0F, host));  // the head
    CHECK(fight.dragon_health() == 192.0F);
    for (i32 i = 0; i < 12; ++i) {
        fight.tick(level, host);  // past the hurt cooldown (a crystal may heal it)
    }
    const f32 before = fight.dragon_health();
    CHECK(fight.hurt(dragon + 3, 8.0F, host));  // the body: 8 / 4 + min(8, 1) = 3
    CHECK(fight.dragon_health() == before - 3.0F);
    CHECK_FALSE(fight.hurt(dragon + 40, 8.0F, host));  // past the parts and the crystals
    CHECK_FALSE(fight.hurt(12, 8.0F, host));
}

TEST_CASE("a hit crystal is removed, and blows on the next tick", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    MapLevel  level;
    Recorder  record;
    EndFight  fight = make_fight(rules);
    const auto host = record.host();
    fight.start(level, 62, host);
    const auto spike = worldgen::end_spikes(kSeed)[0];
    // A player beside the pillar hears it and is hurt; dirt round the crystal
    // goes; the obsidian stays.
    record.people.push_back(EndFightPlayer{7, Vec3d{static_cast<f64>(spike.centre_x) + 3.5,
                                                    static_cast<f64>(spike.height),
                                                    static_cast<f64>(spike.centre_z) + 0.5},
                                           true, true});
    const auto dirt     = blocks().default_state(*blocks().find_block("minecraft:dirt"));
    const auto obsidian = blocks().default_state(*blocks().find_block("minecraft:obsidian"));
    const BlockPos near{spike.centre_x + 1, spike.height + 1, spike.centre_z};
    const BlockPos under{spike.centre_x + 1, spike.height - 1, spike.centre_z};
    level.set_block(near, dirt);
    level.set_block(under, obsidian);
    // The crystals take the ids after the dragon's nine.
    CHECK(fight.hurt(fight.dragon_id() + 9, 1.0F, host, 7, true));
    CHECK(fight.crystals_alive() == 9);
    CHECK(std::ranges::count(record.ids, net::clientbound::kRemoveEntities) == 1);
    CHECK(level.name_at(near) == "minecraft:dirt");  // not yet: the tick blows it
    fight.tick(level, host);
    CHECK(level.name_at(near) == "minecraft:air");
    CHECK(level.name_at(under) == "minecraft:obsidian");
    CHECK(std::ranges::count(record.sent, std::pair<i32, i32>{7, net::clientbound::kExplosion}) == 1);
    CHECK_FALSE(record.hurts.empty());
    // And the dragon strafes the player who broke it.
    REQUIRE(fight.dragon() != nullptr);
    CHECK(fight.dragon()->phase() == gameplay::DragonPhase::StrafePlayer);
    CHECK(fight.dragon()->attack_target() == std::optional<i32>{7});
}

TEST_CASE("/kill takes the dragon without its animation or its experience", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    MapLevel  level;
    Recorder  record;
    EndFight  fight = make_fight(rules);
    const auto host = record.host();
    fight.start(level, 62, host);
    const BlockPos portal = *fight.portal();
    CHECK(fight.kill(fight.dragon_id(), host));
    CHECK(fight.dragon_alive());  // carried out by the next tick
    fight.tick(level, host);
    CHECK_FALSE(fight.dragon_alive());
    CHECK(fight.dragon_killed());
    CHECK(fight.previously_killed());
    CHECK(fight.orbs() == 0);
    CHECK(level.name_at({portal.x + 1, portal.y, portal.z}) == "minecraft:end_portal");
    CHECK(level.name_at({portal.x, portal.y + 4, portal.z}) == "minecraft:dragon_egg");
    REQUIRE(fight.last_gateway().has_value());
    CHECK(*fight.last_gateway() == gameplay::end_gateway_order(kSeed)[0]);
    CHECK(level.name_at(*fight.last_gateway()) == "minecraft:end_gateway");
    CHECK(fight.gateways().size() == 19);
}

TEST_CASE("a lethal hit: the flight to the fountain, 200 ticks, 12000 experience", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    MapLevel  level;
    island(level);
    Recorder  record;
    EndFight  fight = make_fight(rules);
    const auto host = record.host();
    fight.start(level, 63, host);
    const BlockPos portal = *fight.portal();
    CHECK(fight.hurt(fight.dragon_id() + 1, 1000.0F, host));
    CHECK(fight.dragon_health() == 1.0F);  // flying to die
    CHECK(fight.dragon()->phase() == gameplay::DragonPhase::Dying);
    CHECK(std::ranges::count(record.ids, net::clientbound::kEntityEvent) == 1);
    i32 ticks = 0;
    while (fight.dragon_alive() && ticks < 20000) {
        fight.tick(level, host);
        ++ticks;
    }
    CHECK_FALSE(fight.dragon_alive());
    CHECK(fight.experience_dropped() == 12000);
    CHECK(fight.orbs() > 0);
    CHECK(level.name_at({portal.x + 1, portal.y, portal.z}) == "minecraft:end_portal");
    CHECK(level.name_at({portal.x, portal.y + 4, portal.z}) == "minecraft:dragon_egg");
}

TEST_CASE("DragonFight is written with the real server's keys, and read back", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    MapLevel  level;
    Recorder  record;
    EndFight  fight = make_fight(rules);
    const auto host = record.host();

    // Before anyone went: scanning still needed, the twenty gateways.
    const nbt::Tag fresh = fight.save();
    CHECK(fresh.find("NeedsStateScanning")->as_bool());
    REQUIRE(fresh.find("Gateways")->get_if<nbt::Tag::IntArray>() != nullptr);
    CHECK(fresh.find("Gateways")->get_if<nbt::Tag::IntArray>()->size() == 20);

    fight.start(level, 64, host);
    (void)fight.kill(fight.dragon_id(), host);
    fight.tick(level, host);
    const nbt::Tag after = fight.save();
    // Measured after a death: NeedsStateScanning 0, ExitPortalLocation
    // [0, 63, 0], Gateways without the 17, killed and previously killed, the
    // dragon's UUID kept.
    CHECK_FALSE(after.find("NeedsStateScanning")->as_bool());
    const auto* portal = after.find("ExitPortalLocation")->get_if<nbt::Tag::IntArray>();
    REQUIRE(portal != nullptr);
    CHECK(*portal == nbt::Tag::IntArray{0, 63, 0});
    const auto* gateways = after.find("Gateways")->get_if<nbt::Tag::IntArray>();
    REQUIRE(gateways != nullptr);
    CHECK(*gateways == nbt::Tag::IntArray{10, 0, 18, 13, 8, 9, 5, 4, 12, 3, 1, 7, 2, 14, 16, 15,
                                          11, 19, 6});
    CHECK(after.find("DragonKilled")->as_bool());
    CHECK(after.find("PreviouslyKilled")->as_bool());
    REQUIRE(after.find("Dragon") != nullptr);
    CHECK(after.find("Dragon")->get_if<nbt::Tag::IntArray>()->size() == 4);
    const std::vector<std::string_view> keys{"NeedsStateScanning", "ExitPortalLocation", "Gateways",
                                             "DragonKilled", "PreviouslyKilled", "Dragon"};
    std::vector<std::string_view> written;
    for (const auto& entry : *after.compound()) {
        written.push_back(entry.name);
    }
    CHECK(written == keys);

    // A save the real server wrote reads back: no arena to build, no dragon.
    nbt::Tag vanilla = nbt::Tag::make_compound();
    vanilla.put("NeedsStateScanning", nbt::Tag::make_bool(false));
    vanilla.put("ExitPortalLocation", nbt::Tag{nbt::Tag::IntArray{0, 63, 0}});
    vanilla.put("Gateways", nbt::Tag{nbt::Tag::IntArray{10, 0, 18, 13, 8, 9, 5, 4, 12, 3, 1, 7, 2,
                                                        14, 16, 15, 11, 19, 6}});
    vanilla.put("DragonKilled", nbt::Tag::make_bool(true));
    vanilla.put("PreviouslyKilled", nbt::Tag::make_bool(true));
    vanilla.put("Dragon", nbt::Tag{nbt::Tag::IntArray{-305374642, 1043548086, -2133351795,
                                                      -1078253271}});
    EndFight reread = make_fight(rules);
    reread.load(vanilla);
    CHECK(reread.started());
    CHECK_FALSE(reread.dragon_alive());
    CHECK(reread.dragon_killed());
    CHECK(reread.portal() == std::optional<BlockPos>{BlockPos{0, 63, 0}});
    CHECK(reread.gateways().size() == 19);
    const nbt::Tag again = reread.save();
    CHECK(*again.find("Dragon")->get_if<nbt::Tag::IntArray>() ==
          nbt::Tag::IntArray{-305374642, 1043548086, -2133351795, -1078253271});
}

TEST_CASE("four crystals on the exit portal bring the dragon back", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    MapLevel  level;
    island(level);
    Recorder  record;
    EndFight  fight = make_fight(rules);
    const auto host = record.host();
    fight.start(level, 63, host);
    const BlockPos portal = *fight.portal();
    (void)fight.kill(fight.dragon_id(), host);
    fight.tick(level, host);
    REQUIRE(level.name_at({portal.x, portal.y + 4, portal.z}) == "minecraft:dragon_egg");
    for (const auto& [dx, dz] : std::array<std::pair<i32, i32>, 4>{{{3, 0}, {-3, 0}, {0, 3}, {0, -3}}}) {
        fight.place_crystal(BlockPos{portal.x + dx, portal.y, portal.z + dz}, host);
    }
    fight.tick(level, host);
    CHECK(fight.respawn_ticks() >= 0);
    CHECK(level.name_at({portal.x + 1, portal.y, portal.z}) == "minecraft:air");  // inactive again
    CHECK(level.name_at({portal.x, portal.y + 4, portal.z}) == "minecraft:air");  // the egg goes
    // Measured: no IsRespawning in the save, DragonKilled still set.
    const nbt::Tag saving = fight.save();
    CHECK(saving.find("IsRespawning") == nullptr);
    CHECK(saving.find("DragonKilled")->as_bool());
    for (i32 i = 0; i < kDragonRespawnTicks + 5 && !fight.dragon_alive(); ++i) {
        fight.tick(level, host);
    }
    CHECK(fight.dragon_alive());
    CHECK_FALSE(fight.dragon_killed());
    CHECK(fight.previously_killed());
    CHECK(fight.crystals_alive() == 10);  // the spikes' crystals, back; the four blew
    // A later dragon is worth 500.
    REQUIRE(fight.dragon() != nullptr);
    CHECK(fight.dragon()->experience() == 500);
}

TEST_CASE("a bottle takes half a block off a breath cloud", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    MapLevel  level;
    island(level);
    Recorder  record;
    EndFight  fight = make_fight(rules);
    const auto host = record.host();
    fight.start(level, 63, host);
    record.people.push_back(EndFightPlayer{9, Vec3d{4.5, 63.0, 0.5}, true, true});
    // Put the dragon on its perch, breathing.
    gameplay::Dragon* dragon = fight.dragon();
    REQUIRE(dragon != nullptr);
    dragon->set_phase(gameplay::DragonPhase::Landing);
    i32 ticks = 0;
    while (fight.clouds() == 0 && ticks < 20000) {
        fight.tick(level, host);
        ++ticks;
    }
    REQUIRE(fight.clouds() == 1);
    CHECK_FALSE(fight.take_breath(Vec3d{60.0, 63.0, 60.0}));  // out of reach
    bool took = false;
    // The cloud is 2.5 blocks ahead of a head 6.5 ahead of the body: up to
    // nine or ten blocks out from the fountain.
    for (i32 x = -16; x <= 16 && !took; ++x) {
        for (i32 z = -16; z <= 16 && !took; ++z) {
            took = fight.take_breath(Vec3d{static_cast<f64>(x) + 0.5, 63.0, static_cast<f64>(z) + 0.5});
        }
    }
    CHECK(took);
}

TEST_CASE("a death dealt between two ticks is carried out on the next", "[end]") {
    // `/kill` hurts through the command path, outside the tick. The tick used
    // to return early on `health.dead` and never carried the death out: no
    // Combat Death, no `awaiting_respawn`, and the respawn that followed
    // respawned nobody. Found by the End's death-and-return e2e.
    SurvivalSession  session;
    std::vector<i32> sent;
    const SurvivalIo io{.send      = [&](i32 id, std::span<const u8>) { sent.push_back(id); },
                        .broadcast = [](i32, std::span<const u8>) {}};
    const SurvivalPlayer player{.entity_id = 7, .name = "ovender", .y = 50.0};
    (void)session.tick(player, io, gameplay::Difficulty::Normal, true, 0.0);  // the first update
    (void)session.hurt(gameplay::DamageKind::GenericKill, std::numeric_limits<f32>::max(), io, 7);
    REQUIRE(session.health.dead);
    CHECK_FALSE(session.awaiting_respawn);

    sent.clear();
    const SurvivalOutcome outcome =
        session.tick(player, io, gameplay::Difficulty::Normal, true, 0.0);
    CHECK(outcome.died);
    CHECK(session.awaiting_respawn);
    CHECK(std::ranges::count(sent, net::clientbound::kCombatDeath) == 1);

    // And only once.
    sent.clear();
    CHECK_FALSE(session.tick(player, io, gameplay::Difficulty::Normal, true, 0.0).died);
    CHECK(std::ranges::count(sent, net::clientbound::kCombatDeath) == 0);

    // The Respawn names the level of the death: the End, as the game's does.
    session.death_dimension = "minecraft:the_end";
    std::vector<u8> respawn;
    const SurvivalIo capture{.send = [&](i32 id, std::span<const u8> payload) {
                                 if (id == net::clientbound::kRespawn) {
                                     respawn.assign(payload.begin(), payload.end());
                                 }
                             },
                             .broadcast = [](i32, std::span<const u8>) {}};
    SurvivalOutcome back;
    REQUIRE(session.perform_respawn(player, capture, back, 0));
    const std::string bytes(respawn.begin(), respawn.end());
    CHECK(bytes.find("minecraft:the_end") != std::string::npos);
}

TEST_CASE("the End's arrival and its chunks", "[end]") {
    const EndArrival arrival = end_arrival();
    CHECK(arrival.position.x == 100.5);
    CHECK(arrival.position.y == 49.0);  // measured: on the obsidian at y = 48
    CHECK(arrival.position.z == 0.5);
    CHECK(arrival.yaw == 90.0F);
    const auto chunks = end_platform_chunks();
    CHECK(chunks[0] == ChunkPos{6, -1});
    CHECK(chunks[1] == ChunkPos{6, 0});
}
