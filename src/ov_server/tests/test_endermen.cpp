// ── mobs-5 ── The endermen's session (endermen.hpp): anger by a stare or a
// hit, water and rain, carrying, what the clients see, the Anvil record. The
// numbers are the real server's (docs/provenance/mobs-5.md).
#include "../src/endermen.hpp"
#include "../src/mob_combat.hpp"

#include "ov/gameplay/enderman.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

[[nodiscard]] std::filesystem::path pack() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] const registry::Registries* registries() {
    static const auto loaded = registry::Registries::load(pack());
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const auto loaded = registry::BlockRegistry::load(pack());
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] registry::BlockStateId state(const std::string& name) {
    return blocks()->default_state(blocks()->find_block(name).value());
}

/// Flat stone at y = −1; at y = 0, `surface` everywhere, water in the pool
/// around the origin when there is one, and whatever was written.
struct Ground {
    registry::BlockStateId                                      stone = state("minecraft:stone");
    registry::BlockStateId                                      surface{registry::kAirState};
    bool                                                        pool{false};
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> written;

    [[nodiscard]] registry::BlockStateId at(i32 x, i32 y, i32 z) const {
        if (const auto it = written.find({x, y, z}); it != written.end()) {
            return it->second;
        }
        if (y == -1) {
            return stone;
        }
        if (y == 0 && pool && std::abs(x) <= 1 && std::abs(z) <= 1) {
            return state("minecraft:water");
        }
        return y == 0 ? surface : registry::kAirState;
    }
    static registry::BlockStateId look_up(void* context, i32 x, i32 y, i32 z) {
        return static_cast<const Ground*>(context)->at(x, y, z);
    }
};

struct Scene {
    entity::EntityWorld  world{*registries(), 1000};
    MobCombat            combat{*registries(), nullptr};
    Endermen             endermen{*registries(), *blocks(), &combat};
    Ground               ground;
    gameplay::CollisionWorld collisions{*blocks(), &Ground::look_up, &ground};
    bool                 raining{false};
    std::vector<i32>     packets;
    EndermenHost         host;
    i64                  now{1000};

    Scene() {
        host.raining_at = [this](BlockPos) { return raining; };
        host.block_at   = [this](BlockPos p) { return ground.at(p.x, p.y, p.z); };
        host.broadcast  = [this](i32 id, std::span<const u8>) { packets.push_back(id); };
    }

    [[nodiscard]] i32 spawn(Vec3d at = Vec3d{0.5, 0.0, 0.5}) {
        const auto handle = world.spawn("minecraft:enderman", at, net::Uuid{});
        REQUIRE(handle.has_value());
        const entity::EntityState* s    = world.state(*handle);
        const gameplay::MobKind*   kind = gameplay::mob_kind("minecraft:enderman");
        REQUIRE(kind != nullptr);
        world.set_logic(*handle, std::make_unique<gameplay::Mob>(*kind, s->width, s->height,
                                                                 s->network_id));
        return s->network_id;
    }
    [[nodiscard]] entity::EntityState& of(i32 id) { return *world.mutable_state(world.find(id)); }
    [[nodiscard]] const gameplay::MobBrain& brain(i32 id) {
        return dynamic_cast<gameplay::Mob*>(world.logic(world.find(id)))->brain();
    }
    void tick(std::span<const EndermanWatcher> watchers = {}, bool griefing = true) {
        endermen.tick(world, collisions, watchers, griefing, now++, -64, host);
    }
};

/// A player eight blocks east of an enderman at the origin, looking at its
/// eyes — or `off` degrees above them.
[[nodiscard]] EndermanWatcher watcher(f64 off = 0.0, bool masked = false) {
    constexpr f64 kDegrees = 3.14159265358979323846 / 180.0;
    EndermanWatcher w;
    w.player = 7;
    w.eye    = Vec3d{8.5, 1.62, 0.5};
    w.yaw    = 90.0F;  // facing −x
    w.pitch  = static_cast<f32>(-std::atan2(2.55 - 1.62, 8.0) / kDegrees - off);
    w.masked = masked;
    return w;
}

}  // namespace

TEST_CASE("a stare angers an enderman, a carved pumpkin does not", "[server][mobs5]") {
    REQUIRE(registries() != nullptr);
    REQUIRE(blocks() != nullptr);
    {
        Scene           scene;
        const i32       id = scene.spawn();
        EndermanWatcher look[]{watcher()};
        scene.tick(look);
        CHECK(scene.endermen.angry(id));
        CHECK(scene.endermen.target_of(id) == 7);
        CHECK(scene.brain(id).target_player == 7);
        // What the clients were told: screaming and stared at (17, 18).
        CHECK(std::count(scene.packets.begin(), scene.packets.end(),
                         net::clientbound::kEntityMetadata) >= 1);
    }
    {
        Scene           scene;
        const i32       id = scene.spawn();
        EndermanWatcher look[]{watcher(0.0, true)};
        scene.tick(look);
        CHECK_FALSE(scene.endermen.angry(id));
    }
    {
        Scene           scene;
        const i32       id = scene.spawn();
        EndermanWatcher look[]{watcher(7.0)};  // well above the eyes
        scene.tick(look);
        CHECK_FALSE(scene.endermen.angry(id));
    }
}

TEST_CASE("anger ends when its time is up", "[server][mobs5]") {
    Scene           scene;
    const i32       id = scene.spawn();
    EndermanWatcher look[]{watcher()};
    scene.tick(look);
    REQUIRE(scene.endermen.angry(id));
    scene.now += gameplay::kAngerMaxTicks;
    scene.tick();
    CHECK_FALSE(scene.endermen.angry(id));
    CHECK(scene.brain(id).target_player == 0);
}

TEST_CASE("water hurts and moves it; so does the rain", "[server][mobs5]") {
    for (const bool rain : {false, true}) {
        Scene scene;
        scene.ground.pool = !rain;
        scene.raining     = rain;
        const i32   id     = scene.spawn();
        const Vec3d before = scene.of(id).position;
        scene.tick();
        // 1 `drown` damage through its window (enderman, enderman2)…
        CHECK(scene.of(id).health == 39.0F);
        REQUIRE(scene.endermen.hurts().size() == 1);
        CHECK(scene.endermen.hurts()[0].kind == gameplay::DamageKind::Drown);
        // …and out: a teleport, never into the pool.
        const Vec3d after = scene.of(id).position;
        CHECK((after.x != before.x || after.z != before.z));
        CHECK(std::abs(after.x - before.x) <= gameplay::kTeleportHalfSpan);
        if (!rain) {
            CHECK((std::abs(after.x) > 1.5 || std::abs(after.z) > 1.5));
        }
    }
}

TEST_CASE("a hit angers it at the attacker, and it teleports", "[server][mobs5]") {
    Scene       scene;
    const i32   id     = scene.spawn();
    const Vec3d before = scene.of(id).position;
    scene.endermen.on_hurt(id, 42, scene.now);
    scene.tick();
    CHECK(scene.endermen.target_of(id) == 42);
    CHECK(scene.brain(id).target_player == 42);
    CHECK(scene.of(id).position.x != before.x);
}

TEST_CASE("it takes what is holdable at its feet, and puts it down on sturdy ground",
          "[server][mobs5]") {
    Scene scene;
    scene.ground.surface = state("minecraft:dandelion");
    const i32 id         = scene.spawn();
    CHECK(scene.endermen.holdable(state("minecraft:dandelion")));
    CHECK_FALSE(scene.endermen.holdable(state("minecraft:stone")));

    // Without mobGriefing, nothing.
    scene.endermen.set_chances(1.0F, 0.0F);
    for (i32 i = 0; i < 20; ++i) {
        scene.tick({}, false);
    }
    CHECK_FALSE(scene.endermen.carried(id));
    CHECK(scene.endermen.edits().empty());

    // With it, a dandelion at the feet's level (the region's lower layer).
    for (i32 i = 0; i < 50 && !scene.endermen.carried(id); ++i) {
        scene.tick();
    }
    REQUIRE(scene.endermen.carried(id));
    CHECK(*scene.endermen.carried(id) == state("minecraft:dandelion"));
    REQUIRE(scene.endermen.edits().size() == 1);
    CHECK(scene.endermen.edits()[0].pos.y == 0);
    CHECK(blocks()->is_air(blocks()->block_of(scene.endermen.edits()[0].state)));
    scene.endermen.clear_edits();

    // Down again, on bare stone: air above a sturdy top.
    scene.ground.surface = registry::kAirState;
    scene.endermen.set_chances(0.0F, 1.0F);
    for (i32 i = 0; i < 50 && scene.endermen.carried(id); ++i) {
        scene.tick();
    }
    CHECK_FALSE(scene.endermen.carried(id));
    REQUIRE(scene.endermen.edits().size() == 1);
    CHECK(scene.endermen.edits()[0].pos.y == 0);
    CHECK(scene.endermen.edits()[0].state == state("minecraft:dandelion"));
}

TEST_CASE("carriedBlockState in the Anvil record, and back", "[server][mobs5]") {
    Scene     scene;
    const i32 id = scene.spawn();
    nbt::Tag  in = nbt::Tag::make_compound();
    nbt::Tag  carried = nbt::Tag::make_compound();
    (void)carried.put("Name", nbt::Tag{std::string{"minecraft:grass_block"}});
    nbt::Tag props = nbt::Tag::make_compound();
    (void)props.put("snowy", nbt::Tag{std::string{"false"}});
    (void)carried.put("Properties", std::move(props));
    (void)in.put("carriedBlockState", std::move(carried));
    scene.endermen.read(scene.of(id), in);
    REQUIRE(scene.endermen.carried(id));
    CHECK(blocks()->block_name(blocks()->block_of(*scene.endermen.carried(id))) ==
          "minecraft:grass_block");

    nbt::Tag out = nbt::Tag::make_compound();
    scene.endermen.write(scene.of(id), out);
    const nbt::Tag* written = out.find("carriedBlockState");
    REQUIRE(written != nullptr);
    CHECK(written->find("Name")->as_string() == "minecraft:grass_block");

    // One that carries nothing writes no key, and takes a stale one away.
    const i32 other = scene.spawn(Vec3d{10.5, 0.0, 10.5});
    nbt::Tag  bare  = nbt::Tag::make_compound();
    (void)bare.put("carriedBlockState", nbt::Tag{i32{1}});
    scene.endermen.write(scene.of(other), bare);
    CHECK(bare.find("carriedBlockState") == nullptr);
}
