// The dragon fight: what starts it, what heals the dragon, what a hit does,
// and what its death leaves. See end_fight.hpp for what is not the game's.
#include "../src/end_fight.hpp"
#include "../src/end_travel.hpp"
#include "../src/survival_session.hpp"

#include "ov/gameplay/damage.hpp"
#include "ov/protocol/survival.hpp"

#include "ov/gameplay/end_portal.hpp"
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
    void set_block(BlockPos pos, registry::BlockStateId state) override { cells_[key(pos)] = state; }
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
    std::vector<i32> ids;
    i32              next{100};
    EndFightHost     host() {
        EndFightHost h;
        h.broadcast          = [this](i32 id, std::span<const u8>) { ids.push_back(id); };
        h.reserve_entity_ids = [this](i32 count) {
            const i32 first = next;
            next += count;
            return first;
        };
        return h;
    }
};

constexpr i64 kSeed = 1234567890;

}  // namespace

TEST_CASE("the fight starts with the exit portal, the dragon and ten crystals", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    REQUIRE(rules.valid());
    MapLevel  level;
    Recorder  record;
    EndFight  fight{rules, kSeed, 27, 26};
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
    EndFight  fight{rules, kSeed, 27, 26};
    const auto host = record.host();
    fight.start(level, 62, host);
    const i32 dragon = fight.dragon_id();

    CHECK(fight.hurt(dragon + 1, 8.0F, host));  // the head
    CHECK(fight.dragon_health() == 192.0F);
    CHECK(fight.hurt(dragon + 3, 8.0F, host));  // the body: 8 / 4 + min(8, 1) = 3
    CHECK(fight.dragon_health() == 189.0F);
    CHECK(fight.hurt(dragon + 7, 0.5F, host));  // a wing: 0.125 + 0.5
    CHECK(fight.dragon_health() == 188.375F);
    CHECK_FALSE(fight.hurt(dragon + 40, 8.0F, host));  // past the parts and the crystals
    CHECK_FALSE(fight.hurt(12, 8.0F, host));
}

TEST_CASE("a crystal is destroyed by a hit", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    MapLevel  level;
    Recorder  record;
    EndFight  fight{rules, kSeed, 27, 26};
    const auto host = record.host();
    fight.start(level, 62, host);
    // The crystals take the ids after the dragon's nine.
    CHECK(fight.hurt(fight.dragon_id() + 9, 1.0F, host));
    CHECK(fight.crystals_alive() == 9);
    CHECK(std::ranges::count(record.ids, net::clientbound::kRemoveEntities) == 1);
}

TEST_CASE("the dragon's death opens the portal, leaves the egg and a gateway", "[end]") {
    const gameplay::EndPortalRules rules{blocks()};
    MapLevel  level;
    Recorder  record;
    EndFight  fight{rules, kSeed, 27, 26};
    const auto host = record.host();
    fight.start(level, 62, host);
    const BlockPos portal = *fight.portal();
    CHECK(fight.hurt(fight.dragon_id() + 1, 1000.0F, host));
    CHECK(fight.dragon_health() == 0.0F);
    CHECK(fight.dragon_alive());  // dying
    for (i32 tick = 0; tick < kDragonDeathTicks - 1; ++tick) {
        fight.tick(level, host);
    }
    CHECK(fight.dragon_alive());
    CHECK(level.name_at({portal.x + 1, portal.y, portal.z}) == "minecraft:air");
    fight.tick(level, host);
    CHECK_FALSE(fight.dragon_alive());
    CHECK(fight.dragon_killed());
    CHECK(fight.previously_killed());
    CHECK(level.name_at({portal.x + 1, portal.y, portal.z}) == "minecraft:end_portal");
    CHECK(level.name_at({portal.x, portal.y + 4, portal.z}) == "minecraft:dragon_egg");
    REQUIRE(fight.last_gateway().has_value());
    CHECK(*fight.last_gateway() == gameplay::end_gateway_order(kSeed)[0]);
    CHECK(level.name_at(*fight.last_gateway()) == "minecraft:end_gateway");
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
