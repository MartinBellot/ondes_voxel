// The weather session on real chunks: the path the server runs.
//
// A 3x3 square of chunks — grass, a biome, open sky — behind a `ServerLevel`,
// and `WeatherSession::tick` driven the way the server drives it. What the
// gameplay tests hold still one rule at a time, this runs through the chunk
// map, the heightmaps and the biome array, with a fake host that records what
// would have gone to the clients.
#include "../src/weather_session.hpp"
#include "../src/world_ticks.hpp"

#include "ov/entity/world.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/world/chunk_section.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

struct Packs {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
};

[[nodiscard]] const Packs& packs() {
    static const Packs state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Packs out;
        auto  blocks = registry::BlockRegistry::load(path);
        auto  regs   = registry::Registries::load(path);
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] registry::BlockStateId state_of(
    std::string_view name,
    std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
    const auto& blocks = *packs().blocks;
    const auto  id     = blocks.find_block(name);
    REQUIRE(id.has_value());
    registry::BlockStateId state = blocks.default_state(*id);
    for (const auto& [key, value] : props) {
        const auto property = blocks.find_property(*id, key);
        REQUIRE(property.has_value());
        for (usize i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == value) {
                state = blocks.with_property(state, *property, static_cast<u16>(i));
            }
        }
    }
    return state;
}

[[nodiscard]] std::string name_at(registry::BlockStateId state) {
    const auto& blocks = *packs().blocks;
    return std::string{blocks.block_name(blocks.block_of(state))};
}

/// Nine chunks of grass at y = -61 in one biome, open to the sky.
struct World {
    world::ChunkMap              chunks;
    std::optional<ServerLevel>   level;
    cmd::WorldState              state;
    std::vector<ChunkPos>        selected{ChunkPos{0, 0}};

    explicit World(std::string_view biome) {
        const auto& blocks = *packs().blocks;
        const auto  id     = blocks.find_biome(biome);
        REQUIRE(id.has_value());
        const auto grass = state_of("minecraft:grass_block");
        for (i32 cz = -1; cz <= 1; ++cz) {
            for (i32 cx = -1; cx <= 1; ++cx) {
                world::Chunk chunk{ChunkPos{cx, cz}, world::WorldShape::overworld(),
                                   world::AirStates::from(blocks), &blocks};
                chunk.fill_biome(static_cast<u16>(*id));
                for (usize z = 0; z < 16; ++z) {
                    for (usize x = 0; x < 16; ++x) {
                        chunk.set_block(x, -61, z, grass);
                    }
                }
                // Open sky above the ground: sky light 15 in the sections from
                // the ground up, which is what "sees the sky" reads.
                for (i32 y = -64; y < 320; y += 16) {
                    if (world::ChunkSection* section = chunk.section_for_y(y); section != nullptr && y >= -64) {
                        for (usize i = 0; i < 4096; ++i) {
                            section->sky_light().set(i, 15);
                        }
                    }
                }
                chunks.publish(ChunkPos{cx, cz}, std::move(chunk));
            }
        }
        LevelHooks hooks;
        hooks.block_at = [this](BlockPos p) {
            const world::Chunk* chunk = chunks.find(ChunkPos{p.x >> 4, p.z >> 4});
            return chunk == nullptr ? registry::kAirState
                                    : chunk->get_block(static_cast<usize>(p.x & 15), p.y,
                                                       static_cast<usize>(p.z & 15));
        };
        hooks.is_loaded = [this](BlockPos p) {
            return chunks.find(ChunkPos{p.x >> 4, p.z >> 4}) != nullptr;
        };
        hooks.set_block = [this](BlockPos p, registry::BlockStateId s) {
            if (world::Chunk* chunk = chunks.find(ChunkPos{p.x >> 4, p.z >> 4})) {
                chunk->set_block(static_cast<usize>(p.x & 15), p.y, static_cast<usize>(p.z & 15), s);
            }
        };
        level.emplace(blocks, std::move(hooks));
        state.weather.raining    = true;
        state.weather.rain_level = 1.0F;
        state.weather.rain_time  = 1000000;
    }

    void set(BlockPos p, registry::BlockStateId s) { level->set_block(p, s); }
    [[nodiscard]] registry::BlockStateId at(BlockPos p) const { return level->block_at(p); }
};

/// What would have gone to clients.
struct Recorder {
    std::vector<WeatherPlayer>        players;
    std::vector<std::pair<i32, std::string>> messages;  // player, packet id and first bytes
    std::vector<i32>                  broadcast_ids;
    std::vector<std::pair<i32, Vec3d>> placed;
    std::vector<std::string>          converted;
    std::vector<i32>                  charged;
    i32                               next_id{1000};
    bool                              spawn_changed{true};
    WeatherHost                       host;

    Recorder() {
        host.broadcast = [this](i32 id, std::span<const u8>) { broadcast_ids.push_back(id); };
        host.send_to   = [this](i32 player, i32, std::span<const u8> payload) {
            messages.emplace_back(player, std::string(payload.begin(), payload.end()));
        };
        host.players = [this](std::vector<WeatherPlayer>& out) {
            out.insert(out.end(), players.begin(), players.end());
        };
        host.allocate_entity_id = [this] { return next_id++; };
        host.place_player = [this](i32 id, Vec3d feet, f32, f32, bool) {
            placed.emplace_back(id, feet);
            for (WeatherPlayer& who : players) {
                if (who.entity_id == id) {
                    who.feet = feet;
                }
            }
        };
        host.convert_mob = [this](entity::EntityHandle, std::string_view type) {
            converted.emplace_back(type);
            return true;
        };
        host.charge_creeper = [this](i32 id) { charged.push_back(id); };
        host.set_spawn      = [this](const net::Uuid&, BlockPos, f32) { return spawn_changed; };
    }

    [[nodiscard]] bool said(std::string_view key) const {
        for (const auto& [player, text] : messages) {
            if (text.find(key) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] usize count(i32 packet) const {
        usize n = 0;
        for (const i32 id : broadcast_ids) {
            n += id == packet ? 1 : 0;
        }
        return n;
    }
};

}  // namespace

TEST_CASE("snow falls on the chunks the random tick picked, at one column in 4096 a tick") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    World world{"minecraft:snowy_plains"};
    // A 5x5 pond in the middle of the ticked chunk.
    const auto water = state_of("minecraft:water");
    for (i32 x = 5; x < 10; ++x) {
        for (i32 z = 5; z < 10; ++z) {
            world.set({x, -61, z}, water);
        }
    }
    WeatherSession session{*packs().blocks, *packs().registries, nullptr, 7};
    Recorder       recorder;
    constexpr i32  kTicks = 8192;
    WeatherStats   total;
    for (i32 t = 0; t < kTicks; ++t) {
        const WeatherStats stats = session.tick(*world.level, world.chunks, world.selected, world.state,
                                                nullptr, recorder.host);
        total.columns += stats.columns;
        total.precipitation.snowed += stats.precipitation.snowed;
        total.precipitation.froze += stats.precipitation.froze;
    }
    usize snowed = 0;
    usize layered = 0;
    for (i32 x = 0; x < 16; ++x) {
        for (i32 z = 0; z < 16; ++z) {
            const auto state = world.at({x, -60, z});
            if (name_at(state) == "minecraft:snow") {
                ++snowed;
                const auto p = packs().blocks->find_property(packs().blocks->block_of(state), "layers");
                layered += packs().blocks->property_value(state, *p) != "1" ? 1 : 0;
            }
        }
    }
    // 1 - (1 - 1/4096)^8192 = 0.865 of the 231 dry columns; one in sixteen
    // ticks picks a column at all.
    const f64 expected = 231.0 * (1.0 - std::pow(1.0 - 1.0 / 4096.0, kTicks));
    WARN("snow on " << snowed << " of 231 dry columns after " << kTicks << " ticks (model "
                    << expected << "), " << total.columns << " columns picked (model "
                    << kTicks / 16 << "), " << total.precipitation.froze << " cells frozen");
    CHECK(std::abs(static_cast<f64>(snowed) - expected) < 5.0 * std::sqrt(expected * 0.135));
    CHECK(layered == 0);  // snowAccumulationHeight is 1
    CHECK(std::abs(static_cast<f64>(total.columns) - kTicks / 16.0) < 5.0 * std::sqrt(kTicks / 16.0));
    // The pond froze from the edge: the corners first, the middle last if at all.
    usize edge_ice = 0;
    for (const BlockPos p : {BlockPos{5, -61, 5}, BlockPos{9, -61, 9}, BlockPos{5, -61, 9}, BlockPos{9, -61, 5}}) {
        edge_ice += name_at(world.at(p)) == "minecraft:ice" ? 1 : 0;
    }
    CHECK(edge_ice >= 2);
    CHECK(total.precipitation.froze > 0);
}

TEST_CASE("rain fills a cauldron and lays no snow; farmland learns it is raining") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    World world{"minecraft:plains"};
    for (i32 x = 0; x < 16; ++x) {
        for (i32 z = 0; z < 16; ++z) {
            world.set({x, -60, z}, state_of("minecraft:cauldron"));
        }
    }
    WeatherSession session{*packs().blocks, *packs().registries, nullptr, 11};
    Recorder       recorder;
    usize          filled = 0;
    for (i32 t = 0; t < 8192; ++t) {
        filled += session.tick(*world.level, world.chunks, world.selected, world.state, nullptr,
                               recorder.host)
                      .precipitation.cauldrons;
    }
    // About two picks a cauldron, one fill in twenty: some, and not many.
    WARN(filled << " cauldron fills in 8192 ticks over 256 cauldrons (model "
                << 256.0 * (8192.0 / 4096.0) * 0.05 << ")");
    CHECK(filled > 5);
    CHECK(filled < 60);
    CHECK(name_at(world.at({3, -59, 3})) != "minecraft:snow");
    // Is it raining on the top of a cauldron? On the cauldron itself — no, it is
    // under nothing but is the motion-blocking top; above it, yes.
    CHECK(session.is_raining_at(world.chunks, {3, -59, 3}, world.state.weather));
    world.state.weather.rain_level = 0.1F;
    CHECK_FALSE(session.is_raining_at(world.chunks, {3, -59, 3}, world.state.weather));
}

TEST_CASE("a bed: refused by day, slept in at night, and the night skipped after a hundred ticks") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    World world{"minecraft:plains"};
    world.state.weather = cmd::Weather{};
    world.set({5, -60, 0}, state_of("minecraft:red_bed", {{"part", "head"}, {"facing", "east"}}));
    world.set({4, -60, 0}, state_of("minecraft:red_bed", {{"part", "foot"}, {"facing", "east"}}));
    WeatherSession session{*packs().blocks, *packs().registries, nullptr, 3};
    Recorder       recorder;
    recorder.players.push_back(WeatherPlayer{1, net::Uuid{1, 2}, Vec3d{4.5, -60.0, -1.5}, 0.0F, 0.0F,
                                             false, false, true});
    const auto run = [&](i32 ticks) {
        for (i32 i = 0; i < ticks; ++i) {
            (void)session.tick(*world.level, world.chunks, world.selected, world.state, nullptr,
                               recorder.host);
        }
    };

    world.state.day_time = 1000;
    session.request_bed(1, {4, -60, 0});
    run(1);
    CHECK(recorder.said("block.minecraft.set_spawn"));
    CHECK(recorder.said("block.minecraft.bed.no_sleep"));
    CHECK_FALSE(session.sleeping(1));

    recorder.messages.clear();
    recorder.spawn_changed = false;  // the same bed: no second "respawn point set"
    world.state.day_time   = 18000;
    session.request_bed(1, {4, -60, 0});
    run(1);
    CHECK_FALSE(recorder.said("block.minecraft.set_spawn"));
    REQUIRE(session.sleeping(1));
    CHECK(recorder.count(net::clientbound::kEntityMetadata) == 1);
    REQUIRE_FALSE(recorder.placed.empty());
    CHECK(recorder.placed.back().second.y == -60.0 + 0.6875);
    const auto head = world.at({5, -60, 0});
    const auto occupied = packs().blocks->find_property(packs().blocks->block_of(head), "occupied");
    CHECK(packs().blocks->property_value(head, *occupied) == "true");
    // One player of one asleep: the count is announced — skipping the night.
    CHECK(recorder.said("sleep.skipping_night"));

    run(98);
    CHECK(world.state.day_time == 18000);  // not yet: a hundred ticks in bed
    run(2);
    CHECK(world.state.day_time == 24000);
    CHECK_FALSE(session.sleeping(1));
    CHECK(recorder.count(net::clientbound::kEntityAnimation) == 1);
    // Stood up beside the head, on the side away from where they look.
    CHECK(recorder.placed.back().second.x == 5.5);
    CHECK(recorder.placed.back().second.z == -0.5);
    const auto after = world.at({5, -60, 0});
    CHECK(packs().blocks->property_value(after, *occupied) == "false");
    // And the bed is where they come back.
    const auto stand = session.bed_respawn(*world.level, {5, -60, 0}, 0.0F);
    REQUIRE(stand.has_value());
    world.set({5, -60, 0}, registry::kAirState);
    CHECK_FALSE(session.bed_respawn(*world.level, {5, -60, 0}, 0.0F).has_value());
}

TEST_CASE("a monster in the box keeps a player awake; one a fifth further does not") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    World world{"minecraft:plains"};
    world.state.weather  = cmd::Weather{};
    world.state.day_time = 18000;
    world.set({5, -60, 0}, state_of("minecraft:red_bed", {{"part", "head"}, {"facing", "east"}}));
    world.set({4, -60, 0}, state_of("minecraft:red_bed", {{"part", "foot"}, {"facing", "east"}}));
    WeatherSession     session{*packs().blocks, *packs().registries, nullptr, 5};
    Recorder           recorder;
    recorder.players.push_back(WeatherPlayer{1, net::Uuid{1, 2}, Vec3d{4.5, -60.0, -1.5}, 0.0F, 0.0F,
                                             false, false, true});
    entity::EntityWorld mobs{*packs().registries, 500};
    auto zombie = mobs.spawn("minecraft:zombie", Vec3d{13.7, -60.0, 0.5}, net::Uuid{9, 9});
    REQUIRE(zombie.has_value());
    session.request_bed(1, {4, -60, 0});
    (void)session.tick(*world.level, world.chunks, world.selected, world.state, &mobs, recorder.host);
    CHECK(recorder.said("block.minecraft.bed.not_safe"));
    CHECK_FALSE(session.sleeping(1));
    mobs.mutable_state(*zombie)->position = Vec3d{13.9, -60.0, 0.5};
    session.request_bed(1, {4, -60, 0});
    (void)session.tick(*world.level, world.chunks, world.selected, world.state, &mobs, recorder.host);
    CHECK(session.sleeping(1));
}

TEST_CASE("a storm's bolt goes to the rod, and a pig under one becomes a zombified piglin") {
    if (!packs().blocks) {
        SKIP("registry.ovpack is not built");
    }
    World world{"minecraft:plains"};
    world.state.weather.thundering    = true;
    world.state.weather.thunder_level = 1.0F;
    WeatherSession session{*packs().blocks, *packs().registries, nullptr, 13};
    session.set_thunder_chance(1);  // every tick, so the test does not wait a day
    Recorder recorder;

    // A rod on a pillar in the next chunk: every bolt in range goes to it.
    for (i32 y = -60; y <= -57; ++y) {
        world.set({20, y, 20}, state_of("minecraft:stone"));
    }
    world.set({20, -56, 20}, state_of("minecraft:lightning_rod"));
    const WeatherStats stats =
        session.tick(*world.level, world.chunks, world.selected, world.state, nullptr, recorder.host);
    CHECK(stats.strikes == 1);
    CHECK(stats.rods == 1);
    CHECK(recorder.count(net::clientbound::kSpawnEntity) == 1);

    // Without the rod, a pig standing in the open is the target, and is converted.
    world.set({20, -56, 20}, registry::kAirState);
    entity::EntityWorld mobs{*packs().registries, 500};
    REQUIRE(mobs.spawn("minecraft:pig", Vec3d{8.5, -60.0, 8.5}, net::Uuid{3, 3}).has_value());
    world.state.difficulty = 2;
    for (i32 i = 0; i < 40 && recorder.converted.empty(); ++i) {
        (void)session.tick(*world.level, world.chunks, world.selected, world.state, &mobs,
                           recorder.host);
    }
    REQUIRE_FALSE(recorder.converted.empty());
    CHECK(recorder.converted.front() == "minecraft:zombified_piglin");
    // Every bolt goes away again.
    for (i32 i = 0; i < 60; ++i) {
        (void)session.tick(*world.level, world.chunks, world.selected, world.state, &mobs,
                           recorder.host);
    }
    CHECK(recorder.count(net::clientbound::kRemoveEntities) > 0);
}
