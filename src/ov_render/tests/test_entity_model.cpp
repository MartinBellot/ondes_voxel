// The entity model, the walk cycle and the mesh, asserted rather than eyeballed.
//
// Everything here is a number the renderer will use, so everything here can be
// wrong in a way a screenshot hides. A back-facing quad is invisible; a mob a
// sixteenth too tall looks fine; a mirrored arm looks like an arm. The tests
// below pin the three things that no picture would catch: the winding after the
// model-to-world reflection, the box the posed model occupies, and the phase of
// the walk cycle.
#include "ov/render/entity_mesh.hpp"
#include "ov/render/entity_model.hpp"
#include "ov/render/entity_pose.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using ov::f32;
using ov::i32;
using ov::u32;
using ov::usize;
using ov::Vec3f;
using Catch::Approx;

namespace {

/// One bone, one 8x8x8 cube at the origin, on a 64x32 sheet. Everything about
/// the mesher can be asserted on this and nothing else.
constexpr std::string_view kOneCube = R"({
  "format": 1,
  "source": "test",
  "models": {
    "cube": {
      "texture_width": 64, "texture_height": 32,
      "bones": [
        {"name": "body", "parent": "", "pivot": [0, 0, 0], "render": true,
         "cubes": [{"origin": [-4, 0, -4], "size": [8, 8, 8], "uv": [0, 0],
                    "inflate": 0, "mirror": false}]}
      ]
    }
  }
})";

[[nodiscard]] std::span<const ov::u8> bytes_of(std::string_view text) {
    return {reinterpret_cast<const ov::u8*>(text.data()), text.size()};
}

[[nodiscard]] ov::render::EntityModelSet parse_or_fail(std::string_view text) {
    auto set = ov::render::EntityModelSet::parse(bytes_of(text));
    REQUIRE(set.has_value());
    return std::move(*set);
}

}  // namespace

TEST_CASE("an entity model file is read and its hierarchy resolved", "[entity]") {
    const auto set = parse_or_fail(R"({
      "format": 1, "source": "test", "refused": ["chicken/body: rotation par cube"],
      "models": {
        "two": {
          "texture_width": 64, "texture_height": 64,
          "bones": [
            {"name": "body", "parent": "", "pivot": [0, 24, 0], "render": true,
             "cubes": [{"origin": [-4, 12, -2], "size": [8, 12, 4], "uv": [16, 16],
                        "inflate": 0, "mirror": false}]},
            {"name": "head", "parent": "body", "pivot": [0, 24, 0], "render": true,
             "cubes": [{"origin": [-4, 24, -4], "size": [8, 8, 8], "uv": [0, 0],
                        "inflate": 0.5, "mirror": true}]},
            {"name": "waist", "parent": "body", "pivot": [0, 12, 0], "render": false}
          ]
        }
      }
    })");

    const ov::render::EntityModel* model = set.find("two");
    REQUIRE(model != nullptr);
    CHECK(model->bones.size() == 3);
    CHECK(model->bone("head") == 1);
    CHECK(model->bone("nothing") == -1);
    CHECK(model->bones[0].parent == -1);
    CHECK(model->bones[1].parent == 0);
    CHECK(model->bones[2].render == false);
    CHECK(model->bones[1].cubes[0].inflate == Approx(0.5F));
    CHECK(model->bones[1].cubes[0].mirror);

    // The refusals the generator recorded travel with the file, so the client
    // can name what it will not draw.
    REQUIRE(set.refused().size() == 1);
    CHECK(set.refused()[0] == "chicken/body: rotation par cube");
}

TEST_CASE("a bone whose parent is not declared first is refused", "[entity]") {
    const auto set = ov::render::EntityModelSet::parse(bytes_of(R"({
      "format": 1, "models": {
        "bad": {"texture_width": 64, "texture_height": 32, "bones": [
          {"name": "head", "parent": "body", "pivot": [0, 0, 0]},
          {"name": "body", "parent": "", "pivot": [0, 0, 0]}
        ]}}
    })"));
    REQUIRE_FALSE(set.has_value());
    CHECK(set.error() == ov::render::EntityModelError::BadHierarchy);
}

TEST_CASE("a format version this build does not read is refused", "[entity]") {
    const auto set = ov::render::EntityModelSet::parse(bytes_of(R"({"format": 99})"));
    REQUIRE_FALSE(set.has_value());
    CHECK(set.error() == ov::render::EntityModelError::UnknownFormat);
}

TEST_CASE("rest bounds are in blocks, and inflate counts", "[entity]") {
    const auto                     set   = parse_or_fail(kOneCube);
    const ov::render::EntityModel* model = set.find("cube");
    REQUIRE(model != nullptr);

    Vec3f min;
    Vec3f max;
    model->rest_bounds(min, max);
    // Eight units is half a block, from y = 0 to y = 0.5.
    CHECK(min.y == Approx(0.0F));
    CHECK(max.y == Approx(0.5F));
    CHECK(min.x == Approx(-0.25F));
    CHECK(max.x == Approx(0.25F));
}

TEST_CASE("every emitted face faces outwards after the model-to-world reflection",
          "[entity]") {
    // The one thing a screenshot cannot tell you: a mob wound the wrong way is
    // not wrong-looking, it is *absent*. The transform from model space to the
    // world has determinant -1, so this checks the sign that survives it.
    const auto                     set   = parse_or_fail(kOneCube);
    const ov::render::EntityModel* model = set.find("cube");
    REQUIRE(model != nullptr);

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Static,
                           ov::render::WalkState{}, poses);

    for (const f32 yaw : {0.0F, 37.0F, 90.0F, 180.0F, 271.0F}) {
        ov::render::EntityPlacement placement;
        placement.position = Vec3f{10.0F, -60.0F, 5.0F};
        placement.body_yaw = yaw;

        std::vector<ov::render::EntityVertex> vertices;
        const u32 quads = ov::render::emit_entity(*model, poses, placement, vertices);
        REQUIRE(quads == 6);
        REQUIRE(vertices.size() == 24);

        // The cube's centre, which every outward normal must point away from.
        const Vec3f centre{placement.position.x, placement.position.y + 0.25F,
                           placement.position.z};

        for (usize quad = 0; quad < quads; ++quad) {
            const auto& v0 = vertices[quad * 4 + 0];
            const auto& v1 = vertices[quad * 4 + 1];
            const auto& v2 = vertices[quad * 4 + 2];
            const Vec3f p0{v0.x, v0.y, v0.z};
            const Vec3f p1{v1.x, v1.y, v1.z};
            const Vec3f p2{v2.x, v2.y, v2.z};
            const Vec3f normal   = (p1 - p0).cross(p2 - p0);
            const Vec3f outwards = p0 - centre;
            INFO("yaw " << yaw << " quad " << quad);
            CHECK(normal.dot(outwards) > 0.0F);
        }
    }
}

TEST_CASE("yaw zero points the model's front at +Z", "[entity]") {
    // Minecraft's convention: an entity at yaw 0 faces south. The model's front
    // is -Z. Getting this backwards renders every mob walking away from you.
    const auto set = parse_or_fail(R"({
      "format": 1, "models": {"nose": {
        "texture_width": 64, "texture_height": 32, "bones": [
          {"name": "body", "parent": "", "pivot": [0, 0, 0], "render": true,
           "cubes": [{"origin": [-1, 0, -10], "size": [2, 2, 2], "uv": [0, 0],
                      "inflate": 0, "mirror": false}]}]}}
    })");
    const ov::render::EntityModel* model = set.find("nose");
    REQUIRE(model != nullptr);

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Static,
                           ov::render::WalkState{}, poses);

    ov::render::EntityPlacement placement;
    Vec3f                       min;
    Vec3f                       max;
    ov::render::entity_bounds(*model, poses, placement, min, max);
    // The nose is 10 units in front, which lands at +0.5 blocks and further.
    CHECK(min.z > 0.4F);

    placement.body_yaw = 90.0F;  // west
    ov::render::entity_bounds(*model, poses, placement, min, max);
    CHECK(max.x < -0.4F);
}

TEST_CASE("the walk cycle is driven by distance, not by time", "[entity]") {
    ov::render::WalkState standing;
    for (int tick = 0; tick < 100; ++tick) {
        ov::render::advance_walk(standing, 0.0F);
    }
    CHECK(standing.amount == Approx(0.0F).margin(1e-6));
    CHECK(standing.swing == Approx(0.0F).margin(1e-6));

    // A zombie's movement speed is 0.23 blocks per tick — measured, and in the
    // registry pack as `generic.movement_speed`. Four times that is 0.92, below
    // the filter's ceiling of one, so a walking zombie settles there rather
    // than saturating: its legs swing at 92 % of the full amplitude.
    ov::render::WalkState walking;
    for (int tick = 0; tick < 200; ++tick) {
        ov::render::advance_walk(walking, 0.23F);
    }
    CHECK(walking.amount == Approx(0.92F).margin(1e-3));
    // At 0.92 units of swing a tick, a full stride — 360 / 38.17 = 9.43 units —
    // takes 10.25 ticks, which is 0.51 s at 20 Hz.
    CHECK(walking.swing > 180.0F);
    CHECK(walking.swing < 185.0F);
    CHECK(9.4308F / walking.amount == Approx(10.25F).margin(0.01F));
}

TEST_CASE("a humanoid swings its arms and legs in opposition", "[entity]") {
    const auto set = parse_or_fail(R"({
      "format": 1, "models": {"h": {
        "texture_width": 64, "texture_height": 64, "bones": [
          {"name": "body", "parent": "", "pivot": [0, 24, 0]},
          {"name": "head", "parent": "body", "pivot": [0, 24, 0]},
          {"name": "leftArm", "parent": "body", "pivot": [5, 22, 0]},
          {"name": "rightArm", "parent": "body", "pivot": [-5, 22, 0]},
          {"name": "leftLeg", "parent": "body", "pivot": [1.9, 12, 0]},
          {"name": "rightLeg", "parent": "body", "pivot": [-1.9, 12, 0]}
        ]}}
    })");
    const ov::render::EntityModel* model = set.find("h");
    REQUIRE(model != nullptr);

    ov::render::WalkState state;
    state.amount     = 1.0F;
    state.swing      = 0.0F;
    state.head_yaw   = 30.0F;
    state.head_pitch = -10.0F;

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Humanoid, state, poses);

    const auto pose_of = [&](std::string_view name) {
        const i32 index = model->bone(name);
        REQUIRE(index >= 0);
        return poses[static_cast<usize>(index)];
    };

    // The head obeys the packet, not the walk.
    CHECK(pose_of("head").rotation.y == Approx(30.0F));
    CHECK(pose_of("head").rotation.x == Approx(-10.0F));

    // At swing 0 the cosine is 1, so the amplitudes appear undiluted: an arm at
    // one radian and a leg at 1.4, in degrees, opposite left to right.
    CHECK(pose_of("leftArm").rotation.x == Approx(57.3F));
    CHECK(pose_of("rightArm").rotation.x == Approx(-57.3F));
    CHECK(pose_of("rightLeg").rotation.x == Approx(80.0F));
    CHECK(pose_of("leftLeg").rotation.x == Approx(-80.0F));

    // An arm and the leg on the same side move against each other, which is
    // what walking looks like and what a wrong sign destroys.
    CHECK(pose_of("leftArm").rotation.x * pose_of("leftLeg").rotation.x < 0.0F);

    // Half a stride later, everything is reversed.
    state.swing = 180.0F / 38.17F;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Humanoid, state, poses);
    CHECK(pose_of("leftArm").rotation.x == Approx(-57.3F).margin(0.05F));
}

TEST_CASE("standing still poses nothing", "[entity]") {
    const auto set = parse_or_fail(R"({
      "format": 1, "models": {"q": {
        "texture_width": 64, "texture_height": 32, "bones": [
          {"name": "body", "parent": "", "pivot": [0, 19, 2]},
          {"name": "head", "parent": "body", "pivot": [0, 20, -8]},
          {"name": "leg0", "parent": "body", "pivot": [-4, 12, 7]},
          {"name": "leg1", "parent": "body", "pivot": [4, 12, 7]},
          {"name": "leg2", "parent": "body", "pivot": [-4, 12, -6]},
          {"name": "leg3", "parent": "body", "pivot": [4, 12, -6]}
        ]}}
    })");
    const ov::render::EntityModel* model = set.find("q");
    REQUIRE(model != nullptr);

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Quadruped,
                           ov::render::WalkState{}, poses);
    for (const ov::render::BonePose& pose : poses) {
        CHECK(pose.rotation.x == Approx(0.0F).margin(1e-6));
        CHECK(pose.rotation.y == Approx(0.0F).margin(1e-6));
        CHECK(pose.rotation.z == Approx(0.0F).margin(1e-6));
    }
}

TEST_CASE("a quadruped moves its legs on the diagonal", "[entity]") {
    const auto set = parse_or_fail(R"({
      "format": 1, "models": {"q": {
        "texture_width": 64, "texture_height": 32, "bones": [
          {"name": "body", "parent": "", "pivot": [0, 19, 2]},
          {"name": "leg0", "parent": "body", "pivot": [-4, 12, 7]},
          {"name": "leg1", "parent": "body", "pivot": [4, 12, 7]},
          {"name": "leg2", "parent": "body", "pivot": [-4, 12, -6]},
          {"name": "leg3", "parent": "body", "pivot": [4, 12, -6]}
        ]}}
    })");
    const ov::render::EntityModel* model = set.find("q");
    REQUIRE(model != nullptr);

    ov::render::WalkState state;
    state.amount = 1.0F;
    state.swing  = 60.0F / 38.17F;  // a third of the way through a stride

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Quadruped, state, poses);

    const auto angle = [&](std::string_view name) {
        return poses[static_cast<usize>(model->bone(name))].rotation.x;
    };
    CHECK(angle("leg0") == Approx(40.0F).margin(0.05F));
    CHECK(angle("leg3") == Approx(angle("leg0")));
    CHECK(angle("leg1") == Approx(-angle("leg0")));
    CHECK(angle("leg2") == Approx(-angle("leg0")));
}

TEST_CASE("the face shade is the terrain's five values", "[entity]") {
    CHECK(ov::render::face_shade(Vec3f{0.0F, 1.0F, 0.0F}) == Approx(1.0F));
    CHECK(ov::render::face_shade(Vec3f{0.0F, -1.0F, 0.0F}) == Approx(0.5F));
    CHECK(ov::render::face_shade(Vec3f{0.0F, 0.0F, 1.0F}) == Approx(0.8F));
    CHECK(ov::render::face_shade(Vec3f{1.0F, 0.0F, 0.0F}) == Approx(0.6F));
}

TEST_CASE("a species with no animation is drawn standing, not wrongly", "[entity]") {
    CHECK(ov::render::entity_animation("minecraft:zombie") ==
          ov::render::EntityAnimation::Humanoid);
    CHECK(ov::render::entity_animation("minecraft:chicken") ==
          ov::render::EntityAnimation::Chicken);
    CHECK(ov::render::entity_animation("minecraft:enderman") ==
          ov::render::EntityAnimation::Static);
    CHECK(ov::render::entity_model_name("minecraft:enderman").empty());
    CHECK(ov::render::entity_model_name("minecraft:cow") == "cow");
    CHECK(ov::render::entity_texture_name("minecraft:cow") == "minecraft:entity/cow/cow");
}

TEST_CASE("the head's texture net is the skin layout", "[entity]") {
    // The one assertion that pins the net's column order. On a head cube at
    // uv (0, 0) the face is the second panel of the side row, at u = 8..16 and
    // v = 8..16 — which is where every Minecraft skin has drawn a face since
    // 2009. Reading the panels in another order draws a mob with its ear where
    // its nose should be, and it is not obvious in a screenshot at range.
    const auto                     set   = parse_or_fail(kOneCube);
    const ov::render::EntityModel* model = set.find("cube");
    REQUIRE(model != nullptr);

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Static,
                           ov::render::WalkState{}, poses);

    ov::render::EntityPlacement           placement;
    std::vector<ov::render::EntityVertex> vertices;
    ov::render::emit_entity(*model, poses, placement, vertices);

    // Find the quad whose vertices all sit at the model's front, z = -4 units,
    // which at yaw 0 is world +0.25.
    bool found = false;
    for (usize quad = 0; quad * 4 < vertices.size(); ++quad) {
        bool front = true;
        for (usize corner = 0; corner < 4; ++corner) {
            front = front && vertices[quad * 4 + corner].z > 0.2F;
        }
        if (!front) {
            continue;
        }
        found = true;
        f32 min_u = 1.0F;
        f32 max_u = 0.0F;
        f32 min_v = 1.0F;
        f32 max_v = 0.0F;
        for (usize corner = 0; corner < 4; ++corner) {
            min_u = std::min(min_u, vertices[quad * 4 + corner].u);
            max_u = std::max(max_u, vertices[quad * 4 + corner].u);
            min_v = std::min(min_v, vertices[quad * 4 + corner].v);
            max_v = std::max(max_v, vertices[quad * 4 + corner].v);
        }
        CHECK(min_u == Approx(8.0F / 64.0F));
        CHECK(max_u == Approx(16.0F / 64.0F));
        CHECK(min_v == Approx(8.0F / 32.0F));
        CHECK(max_v == Approx(16.0F / 32.0F));
    }
    CHECK(found);
}

TEST_CASE("a mirrored cube reads the other side of its net", "[entity]") {
    const auto set = parse_or_fail(R"({
      "format": 1, "models": {"m": {
        "texture_width": 64, "texture_height": 32, "bones": [
          {"name": "body", "parent": "", "pivot": [0, 0, 0], "render": true,
           "cubes": [{"origin": [-4, 0, -4], "size": [8, 8, 8], "uv": [0, 0],
                      "inflate": 0, "mirror": true}]}]}}
    })");
    const ov::render::EntityModel* model = set.find("m");
    REQUIRE(model != nullptr);

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Static,
                           ov::render::WalkState{}, poses);
    ov::render::EntityPlacement           placement;
    std::vector<ov::render::EntityVertex> vertices;
    ov::render::emit_entity(*model, poses, placement, vertices);

    // The net is 2*(8+8) = 32 texels across. Mirroring reflects u about its
    // middle, so what was 8..16 becomes 16..24 — the panel on the other side of
    // the face. Every u still lands inside the net.
    for (const ov::render::EntityVertex& vertex : vertices) {
        CHECK(vertex.u >= 0.0F);
        CHECK(vertex.u <= 32.0F / 64.0F + 1e-6F);
    }
}
