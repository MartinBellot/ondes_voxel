// Minecarts against the real game, tick by tick.
//
// scripts/measure_rails.py `carts` records fourteen lanes on a real 1.20.1
// server, each sample labelled by a witness TNT's fuse so its tick is exact.
// When that file is present every lane is rebuilt here from the track states
// vanilla wrote and replayed through `step_minecart`; the frozen cases are
// numbers copied from it, so the rule is held without the file.
#include "ov/gameplay/minecart.hpp"

#include "tnt_gravity_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <simdjson.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

constexpr i32 kY = -50;

struct Rules {
    Signals signals;
    Rails   rails;
    Rules() : signals{*test::pack_blocks(), *test::pack_registries()}, rails{*test::pack_blocks(), signals} {}
};

[[nodiscard]] const Rules& rules() {
    static const Rules instance;
    return instance;
}

[[nodiscard]] registry::BlockStateId parse(std::string_view text) {
    const registry::BlockRegistry& blocks = *test::pack_blocks();
    const auto                     open   = text.find('[');
    const auto block = blocks.find_block(text.substr(0, open));
    REQUIRE(block);
    registry::BlockStateId state = blocks.default_state(*block);
    if (open == std::string_view::npos) {
        return state;
    }
    std::string_view rest = text.substr(open + 1, text.size() - open - 2);
    while (!rest.empty()) {
        const auto comma    = rest.find(',');
        const auto pair     = rest.substr(0, comma);
        const auto eq       = pair.find('=');
        const auto property = blocks.find_property(*block, pair.substr(0, eq));
        REQUIRE(property);
        for (u16 i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == pair.substr(eq + 1)) {
                state = blocks.with_property(state, *property, i);
            }
        }
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    return state;
}

/// A cart as the entity table measures it: 0.98 wide, 0.7 tall.
[[nodiscard]] entity::EntityState cart_at(Vec3d position, Vec3d velocity) {
    entity::EntityState state;
    state.position = position;
    state.velocity = velocity;
    state.width    = 0.98F;
    state.height   = 0.7F;
    return state;
}

struct Lane {
    test::TestLevel level{*test::pack_blocks(), kY - 1};
    gameplay::CollisionWorld collisions{*test::pack_blocks(), &test::TestLevel::look_up, &level};

    void rail(i32 x, i32 y, i32 z, std::string_view state) {
        for (i32 sy = kY; sy < y; ++sy) {
            level.place(BlockPos{x, sy, z}, test::state_of("minecraft:stone"));
        }
        level.place(BlockPos{x, y, z}, parse(state));
    }

    void step(entity::EntityState& cart, MinecartBody& body) const {
        (void)step_minecart(cart, body, rules().rails, level, collisions);
    }
};

}  // namespace

TEST_CASE("an empty cart on a straight rail", "[minecart]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    Lane lane;
    for (i32 x = 0; x < 100; ++x) {
        lane.rail(x, kY, 0, "minecraft:rail[shape=east_west]");
    }
    // Measured, lane straight_fast: summoned at 1.0, it moves 0.4 a tick and
    // keeps 0.96 of its velocity. It sits a sixteenth above the rail's cell.
    auto         cart = cart_at(Vec3d{2.5, kY, 0.5}, Vec3d{1.0, 0.0, 0.0});
    MinecartBody body;
    lane.step(cart, body);
    CHECK(cart.position.x == Catch::Approx(2.9).margin(1e-12));
    CHECK(cart.position.y == Catch::Approx(-49.9375).margin(1e-12));
    CHECK(cart.velocity.x == Catch::Approx(0.96).margin(1e-12));
    CHECK(cart.velocity.y == 0.0);
    lane.step(cart, body);
    CHECK(cart.position.x == Catch::Approx(3.3).margin(1e-12));
    CHECK(cart.velocity.x == Catch::Approx(0.9216).margin(1e-12));
}

TEST_CASE("a ridden cart takes three-quarter steps and keeps its speed", "[minecart]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    Lane lane;
    for (i32 x = 0; x < 100; ++x) {
        lane.rail(x, kY, 0, "minecraft:rail[shape=east_west]");
    }
    auto         cart = cart_at(Vec3d{2.5, kY, 0.5}, Vec3d{0.3, 0.0, 0.0});
    MinecartBody body;
    body.ridden = true;
    lane.step(cart, body);
    // Lane passenger: 2.725 and 0.2991.
    CHECK(cart.position.x == Catch::Approx(2.725).margin(1e-12));
    CHECK(cart.velocity.x == Catch::Approx(0.2991).margin(1e-12));
}

TEST_CASE("a cart slides down a slope", "[minecart]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    Lane lane;
    lane.rail(1, kY + 8, 0, "minecraft:rail[shape=east_west]");
    for (i32 i = 0; i < 8; ++i) {
        lane.rail(2 + i, kY + 7 - i, 0, "minecraft:rail[shape=ascending_west]");
    }
    auto         cart = cart_at(Vec3d{2.5, kY + 7, 0.5}, Vec3d{});
    MinecartBody body;
    lane.step(cart, body);
    // Lane slope, tick 1: 1/128 downhill, and the drop turned into speed.
    CHECK(cart.position.x == Catch::Approx(2.5078125).margin(1e-12));
    CHECK(cart.position.y == Catch::Approx(-42.4453125).margin(1e-12));
    CHECK(cart.velocity.x == Catch::Approx(0.007890625).margin(1e-12));
}

TEST_CASE("a powered rail launches a cart from a block", "[minecart]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    Lane lane;
    lane.level.place(BlockPos{0, kY, 0}, test::state_of("minecraft:stone"));
    lane.rail(1, kY, 0, "minecraft:powered_rail[powered=true,shape=east_west]");
    for (i32 x = 2; x < 40; ++x) {
        lane.rail(x, kY, 0, "minecraft:rail[shape=east_west]");
    }
    auto         cart = cart_at(Vec3d{1.5, kY, 0.5}, Vec3d{});
    MinecartBody body;
    // Lane launch: 0.02 away from the stone, then 0.02 × 0.96 + 0.06.
    lane.step(cart, body);
    CHECK(cart.position.x == Catch::Approx(1.5).margin(1e-12));
    CHECK(cart.velocity.x == Catch::Approx(0.02).margin(1e-12));
    lane.step(cart, body);
    CHECK(cart.position.x == Catch::Approx(1.52).margin(1e-12));
    CHECK(cart.velocity.x == Catch::Approx(0.0792).margin(1e-12));
    lane.step(cart, body);
    CHECK(cart.velocity.x == Catch::Approx(0.136032).margin(1e-12));
}

TEST_CASE("off the rails a cart halves its speed on the ground", "[minecart]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    Lane         lane;
    auto         cart = cart_at(Vec3d{2.5, kY, 0.5}, Vec3d{0.3, 0.0, 0.0});
    MinecartBody body;
    lane.step(cart, body);
    CHECK(cart.position.x == Catch::Approx(2.8).margin(1e-12));
    CHECK(cart.velocity.x == Catch::Approx(0.3).margin(1e-12));
    CHECK(cart.on_ground);
    lane.step(cart, body);
    CHECK(cart.position.x == Catch::Approx(2.95).margin(1e-12));
    CHECK(cart.velocity.x == Catch::Approx(0.15).margin(1e-12));
}

TEST_CASE("a furnace cart pushes, capped at 2 and stepping 0.2", "[minecart]") {
    if (test::pack_blocks() == nullptr) {
        SUCCEED("no registry.ovpack");
        return;
    }
    Lane lane;
    for (i32 x = 0; x < 100; ++x) {
        lane.rail(x, kY, 0, "minecraft:rail[shape=east_west]");
    }
    auto         cart = cart_at(Vec3d{2.5, kY, 0.5}, Vec3d{});
    MinecartBody body;
    body.kind   = MinecartKind::Furnace;
    body.fuel   = 3600;
    body.push_x = 1.0;
    lane.step(cart, body);
    CHECK(cart.velocity.x == Catch::Approx(0.96).margin(1e-12));
    lane.step(cart, body);
    CHECK(cart.position.x == Catch::Approx(2.7).margin(1e-12));
    CHECK(cart.velocity.x == Catch::Approx(1.69728).margin(1e-12));
    for (int i = 0; i < 6; ++i) {
        lane.step(cart, body);
    }
    CHECK(cart.velocity.x == Catch::Approx(2.496).margin(1e-12));
}

TEST_CASE("every measured lane, tick by tick", "[minecart][oracle]") {
    const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                      "normalized" / "rails_carts.json";
    if (test::pack_blocks() == nullptr || !std::filesystem::exists(path)) {
        SUCCEED("no measured trajectories: run scripts/measure_rails.py carts");
        return;
    }
    simdjson::dom::parser  parser;
    simdjson::dom::element doc = parser.load(path.string());

    // Samples by lane: the cart's initial z picks its lane.
    struct Sample {
        Vec3d pos;
        Vec3d motion;
    };
    std::map<std::string, std::map<i64, Sample>> samples;
    std::map<i64, std::string>                   lane_by_z;
    for (simdjson::dom::element lane : doc["lanes"]) {
        const std::string_view name = lane["name"].get_string().value();
        for (simdjson::dom::element cart : lane["carts"]) {
            const i64 z = static_cast<i64>(std::floor(cart.at(3).get_double().value()));
            lane_by_z[z] = std::string{name};
        }
    }
    for (simdjson::dom::element sample : doc["samples"]) {
        const std::string_view who = sample["who"].get_string().value();
        if (who != "Minecart" && who != "Minecart with Furnace") {
            continue;
        }
        const f64 z = sample["pos"].at(2).get_double().value();
        // The curves turn south: pick their lane by the region they run in.
        std::string lane;
        if (z >= 270.0) {
            lane = "curve_fast";
        } else if (z >= 190.0) {
            lane = "curve";
        } else {
            const auto it = lane_by_z.find(static_cast<i64>(std::floor(z)));
            if (it == lane_by_z.end()) {
                continue;
            }
            lane = it->second;
        }
        const i64 tick = sample["tick"].get_int64().value();
        Sample    s;
        s.pos    = Vec3d{sample["pos"].at(0).get_double().value(), sample["pos"].at(1).get_double().value(), z};
        s.motion = Vec3d{sample["motion"].at(0).get_double().value(),
                         sample["motion"].at(1).get_double().value(),
                         sample["motion"].at(2).get_double().value()};
        samples[lane].emplace(tick, s);
    }

    usize compared = 0;
    for (simdjson::dom::element lane_doc : doc["lanes"]) {
        const std::string name{lane_doc["name"].get_string().value()};
        // Two carts meeting is an entity collision, which the step does not do.
        if (name == "collide" || samples[name].empty()) {
            continue;
        }
        Lane lane;
        for (simdjson::dom::element cell : lane_doc["track"]) {
            lane.rail(static_cast<i32>(cell.at(0).get_int64().value()),
                      static_cast<i32>(cell.at(1).get_int64().value()),
                      static_cast<i32>(cell.at(2).get_int64().value()),
                      cell.at(3).get_string().value());
        }
        if (name == "launch") {
            // The stone the cart launches from is not a rail, so not in the track.
            lane.level.place(BlockPos{0, kY, 64}, test::state_of("minecraft:stone"));
        }
        simdjson::dom::element spec = lane_doc["carts"].at(0);
        const std::string_view kind = spec.at(0).get_string().value();
        const std::string_view extra = spec.at(5).get_string().value();
        auto cart = cart_at(Vec3d{spec.at(1).get_double().value(), spec.at(2).get_double().value(),
                                  spec.at(3).get_double().value()},
                            Vec3d{spec.at(4).at(0).get_double().value(),
                                  spec.at(4).at(1).get_double().value(),
                                  spec.at(4).at(2).get_double().value()});
        MinecartBody body;
        body.ridden = extra.find("Passengers") != std::string_view::npos;
        if (kind == "furnace_minecart") {
            body.kind   = MinecartKind::Furnace;
            body.fuel   = 3600;
            body.push_x = 1.0;
        }
        f64 worst_pos = 0.0;
        f64 worst_vel = 0.0;
        i64 worst_tick = -1;
        bool reported  = false;
        const i64 last = samples[name].rbegin()->first;
        for (i64 tick = 1; tick <= last; ++tick) {
            lane.step(cart, body);
            if (body.kind == MinecartKind::Furnace && body.fuel > 0) {
                --body.fuel;
            }
            const auto it = samples[name].find(tick);
            if (it == samples[name].end()) {
                continue;
            }
            // A ridden cart reads 0.1F higher than its rail; see the provenance note.
            const f64 lift = body.ridden ? static_cast<f64>(0.1F) : 0.0;
            const f64 dp = std::max({std::abs(cart.position.x - it->second.pos.x),
                                     std::abs(cart.position.y + lift - it->second.pos.y),
                                     std::abs(cart.position.z - it->second.pos.z)});
            const f64 dv = std::max({std::abs(cart.velocity.x - it->second.motion.x),
                                     std::abs(cart.velocity.y - it->second.motion.y),
                                     std::abs(cart.velocity.z - it->second.motion.z)});
            if ((dp > 1e-9 || dv > 1e-9) && !reported) {
                reported = true;
                UNSCOPED_INFO(name << " first differs at tick " << tick << ": ours ("
                                   << cart.position.x << ", " << cart.position.y << ") v "
                                   << cart.velocity.x << ", theirs (" << it->second.pos.x << ", "
                                   << it->second.pos.y << ") v " << it->second.motion.x);
            }
            if (dp > worst_pos || dv > worst_vel) {
                worst_tick = tick;
            }
            worst_pos = std::max(worst_pos, dp);
            worst_vel = std::max(worst_vel, dv);
            ++compared;
        }
        UNSCOPED_INFO(name << ": " << samples[name].size() << " samples, worst position "
                           << worst_pos << ", worst velocity " << worst_vel << " (tick "
                           << worst_tick << ")");
        CHECK(worst_pos < 1e-9);
        CHECK(worst_vel < 1e-9);
    }
    CHECK(compared > 1000);
}
