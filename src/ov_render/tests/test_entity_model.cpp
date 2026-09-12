// The entity model, the walk cycle and the mesh, asserted rather than eyeballed.
//
// Everything here is a number the renderer will use, so everything here can be
// wrong in a way a screenshot hides. A back-facing quad is invisible; a mob a
// sixteenth too tall looks fine; a mirrored arm looks like an arm; a head that
// looks up when the game's looks down looks like a head. The tests below pin
// what no picture would catch: the winding after the model-to-world
// reflection, the rest rotation that lays a cow's body on its legs, the sign of
// every animated angle, and the three colours a vertex carries.
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
/// the format-1 mesher can be asserted on this and nothing else.
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

/// The same kind of geometry in format 2: one quad facing the model's front
/// (−Z), wound clockwise seen from outside, on a bone that lies at rest turned
/// a quarter about X — which is how the game's cow carries its body.
constexpr std::string_view kQuadModel = R"({
  "format": 2,
  "source": "test",
  "models": {
    "slab": {
      "texture_width": 64, "texture_height": 32,
      "bones": [
        {"name": "root", "parent": "", "pivot": [0, 0, 0], "render": false, "quads": []},
        {"name": "body", "parent": "root", "pivot": [0, -5, 2], "rotation": [-90, 0, 0],
         "scale": [1, 1, 1], "visible": true, "render": true,
         "quads": [{"n": [0, 0, -1],
                    "v": [[-6, -15, -7, 0, 0], [6, -15, -7, 12, 0],
                          [6, 3, -7, 12, 18], [-6, 3, -7, 0, 18]]}]},
        {"name": "hidden", "parent": "root", "pivot": [0, 0, 0], "visible": false,
         "render": true,
         "quads": [{"n": [0, 1, 0],
                    "v": [[0, 0, 0, 0, 0], [1, 0, 0, 1, 0], [1, 0, 1, 1, 1], [0, 0, 1, 0, 1]]}]}
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

/// A humanoid's six bones, named as the game names them.
constexpr std::string_view kHumanoid = R"({
  "format": 2, "models": {"h": {
    "texture_width": 64, "texture_height": 64, "bones": [
      {"name": "root", "parent": "", "pivot": [0, 0, 0]},
      {"name": "head", "parent": "root", "pivot": [0, 0, 0]},
      {"name": "hat", "parent": "root", "pivot": [0, 0, 0]},
      {"name": "body", "parent": "root", "pivot": [0, 0, 0]},
      {"name": "right_arm", "parent": "root", "pivot": [-5, -2, 0]},
      {"name": "left_arm", "parent": "root", "pivot": [5, -2, 0]},
      {"name": "right_leg", "parent": "root", "pivot": [-1.9, -12, 0]},
      {"name": "left_leg", "parent": "root", "pivot": [1.9, -12, 0]}
    ]}}
})";

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

    CHECK(set.format() == ov::render::EntityModelSet::kLegacyFormat);
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

    REQUIRE(set.refused().size() == 1);
    CHECK(set.refused()[0] == "chicken/body: rotation par cube");
}

TEST_CASE("a format-2 model carries quads, rest rotations and hidden bones", "[entity]") {
    const auto set = parse_or_fail(kQuadModel);
    CHECK(set.format() == ov::render::EntityModelSet::kFormat);
    const ov::render::EntityModel* model = set.find("slab");
    REQUIRE(model != nullptr);
    REQUIRE(model->bones.size() == 3);
    const ov::render::EntityBone& body = model->bones[1];
    CHECK(body.parent == 0);
    CHECK(body.rest_rotation.x == Approx(-90.0F));
    REQUIRE(body.quads.size() == 1);
    CHECK(body.quads[0].u[2] == Approx(12.0F));
    CHECK(body.quads[0].position[2].y == Approx(3.0F));
    CHECK_FALSE(model->bones[2].visible);
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
    CHECK(min.y == Approx(0.0F));
    CHECK(max.y == Approx(0.5F));
    CHECK(min.x == Approx(-0.25F));
    CHECK(max.x == Approx(0.25F));
}

TEST_CASE("a rest rotation lays the body on its side, as the game's cow is laid", "[entity]") {
    // The bug the first entity work shipped: the Bedrock converter dropped the
    // bind-pose rotation, and every cow, pig and sheep stood on its hind legs
    // with its body upright, 1.81 blocks tall. The slab below is 18 units long
    // in y and 0 in z at rest; turned a quarter about X it must become 18 long
    // in z and flat in y.
    const auto                     set   = parse_or_fail(kQuadModel);
    const ov::render::EntityModel* model = set.find("slab");
    REQUIRE(model != nullptr);

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Static,
                           ov::render::WalkState{}, poses);
    ov::render::EntityPlacement placement;
    Vec3f                       min;
    Vec3f                       max;
    ov::render::entity_bounds(*model, poses, placement, min, max);
    CHECK((max.y - min.y) * 16.0F == Approx(0.0F).margin(1e-3));
    CHECK((max.z - min.z) * 16.0F == Approx(18.0F).margin(1e-3));
    CHECK((max.x - min.x) * 16.0F == Approx(12.0F).margin(1e-3));
}

TEST_CASE("a hidden bone draws nothing, and nor do its children", "[entity]") {
    const auto                     set   = parse_or_fail(kQuadModel);
    const ov::render::EntityModel* model = set.find("slab");
    REQUIRE(model != nullptr);

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Static,
                           ov::render::WalkState{}, poses);
    std::vector<ov::render::EntityVertex> vertices;
    CHECK(ov::render::emit_entity(*model, poses, ov::render::EntityPlacement{}, vertices) == 1);

    poses[1].hidden = true;
    vertices.clear();
    CHECK(ov::render::emit_entity(*model, poses, ov::render::EntityPlacement{}, vertices) == 0);
}

TEST_CASE("every emitted face faces outwards after the model-to-world reflection",
          "[entity]") {
    // The one thing a screenshot cannot tell you: a mob wound the wrong way is
    // not wrong-looking, it is *absent*. The transform from model space to the
    // world has determinant -1, so this checks the sign that survives it —
    // for a format-1 box and for a format-2 face under a rest rotation.
    const auto                     set   = parse_or_fail(kOneCube);
    const ov::render::EntityModel* model = set.find("cube");
    REQUIRE(model != nullptr);

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Static,
                           ov::render::WalkState{}, poses);

    for (const f32 yaw : {0.0F, 37.0F, 90.0F, 180.0F, 271.0F}) {
        for (const f32 roll : {0.0F, 40.0F}) {
            ov::render::EntityPlacement placement;
            placement.position = Vec3f{10.0F, -60.0F, 5.0F};
            placement.body_yaw = yaw;
            placement.roll     = roll;
            placement.scale    = 0.7F;
            placement.origin   = Vec3f{0.0F, 3.0F, 0.0F};

            std::vector<ov::render::EntityVertex> vertices;
            const u32 quads = ov::render::emit_entity(*model, poses, placement, vertices);
            REQUIRE(quads == 6);

            Vec3f min;
            Vec3f max;
            ov::render::entity_bounds(*model, poses, placement, min, max);
            const Vec3f centre = (min + max) * 0.5F;

            for (usize quad = 0; quad < quads; ++quad) {
                const auto& v0 = vertices[quad * 4 + 0];
                const auto& v1 = vertices[quad * 4 + 1];
                const auto& v2 = vertices[quad * 4 + 2];
                const Vec3f p0{v0.x, v0.y, v0.z};
                const Vec3f p1{v1.x, v1.y, v1.z};
                const Vec3f p2{v2.x, v2.y, v2.z};
                const Vec3f normal   = (p1 - p0).cross(p2 - p0);
                const Vec3f outwards = p0 - centre;
                INFO("yaw " << yaw << " roll " << roll << " quad " << quad);
                CHECK(normal.dot(outwards) > 0.0F);
            }
        }
    }

    // Format 2: the front face, stored clockwise, must come out facing +Z at
    // yaw 0 — towards whoever the entity looks at.
    const auto                     quad_set = parse_or_fail(kQuadModel);
    const ov::render::EntityModel* slab     = quad_set.find("slab");
    REQUIRE(slab != nullptr);
    std::vector<ov::render::BonePose> slab_poses(slab->bones.size());
    slab_poses[1].rotation = Vec3f{90.0F, 0.0F, 0.0F};  // undo the rest: stand it up
    std::vector<ov::render::EntityVertex> vertices;
    REQUIRE(ov::render::emit_entity(*slab, slab_poses, ov::render::EntityPlacement{}, vertices) ==
            1);
    const Vec3f p0{vertices[0].x, vertices[0].y, vertices[0].z};
    const Vec3f p1{vertices[1].x, vertices[1].y, vertices[1].z};
    const Vec3f p2{vertices[2].x, vertices[2].y, vertices[2].z};
    CHECK((p1 - p0).cross(p2 - p0).z > 0.0F);
}

TEST_CASE("yaw zero points the model's front at +Z", "[entity]") {
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
    CHECK(min.z > 0.4F);

    placement.body_yaw = 90.0F;  // west
    ov::render::entity_bounds(*model, poses, placement, min, max);
    CHECK(max.x < -0.4F);
}

TEST_CASE("the placement origin lifts a living model to the game's 1.501 blocks", "[entity]") {
    // A format-2 model keeps the game's own origin: its feet are 24.016 units
    // below it. The renderer's offset travels in the placement.
    const auto                     set   = parse_or_fail(kOneCube);
    const ov::render::EntityModel* model = set.find("cube");
    REQUIRE(model != nullptr);
    std::vector<ov::render::BonePose> poses(model->bones.size());
    ov::render::EntityPlacement       placement;
    placement.origin = Vec3f{0.0F, 24.016F, 0.0F};
    placement.scale  = 0.5F;
    placement.lift   = 0.375F;
    Vec3f min;
    Vec3f max;
    ov::render::entity_bounds(*model, poses, placement, min, max);
    // (0 + 24.016) × 0.5 / 16 + 0.375
    CHECK(min.y == Approx(24.016F * 0.5F / 16.0F + 0.375F));
}

TEST_CASE("the walk cycle is driven by distance, not by time", "[entity]") {
    ov::render::WalkState standing;
    for (int tick = 0; tick < 100; ++tick) {
        ov::render::advance_walk(standing, 0.0F);
    }
    CHECK(standing.amount == Approx(0.0F).margin(1e-6));
    CHECK(standing.swing == Approx(0.0F).margin(1e-6));

    ov::render::WalkState walking;
    for (int tick = 0; tick < 200; ++tick) {
        ov::render::advance_walk(walking, 0.23F);
    }
    CHECK(walking.amount == Approx(0.92F).margin(1e-3));
    CHECK(walking.swing > 180.0F);
    CHECK(walking.swing < 185.0F);
    CHECK(9.4308F / walking.amount == Approx(10.25F).margin(0.01F));
}

TEST_CASE("a humanoid swings its arms and legs in opposition, in the game's sign",
          "[entity]") {
    const auto                     set   = parse_or_fail(kHumanoid);
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

    // The head obeys the packet: yaw unchanged through the reflection, pitch
    // negated — a head pitched up by 10° (−10 in the game's convention) turns
    // +10° about this project's X.
    CHECK(pose_of("head").rotation.y == Approx(30.0F));
    CHECK(pose_of("head").rotation.x == Approx(10.0F));
    // The hat is a sibling in the game's model and must follow the head.
    CHECK(pose_of("hat").rotation.x == Approx(pose_of("head").rotation.x));

    // At swing 0 the cosine is 1: the game's left arm at +1 rad and right leg at
    // +1.4, each negated through the reflection.
    CHECK(pose_of("left_arm").rotation.x == Approx(-57.3F));
    CHECK(pose_of("right_arm").rotation.x == Approx(57.3F));
    CHECK(pose_of("right_leg").rotation.x == Approx(-80.0F));
    CHECK(pose_of("left_leg").rotation.x == Approx(80.0F));
    CHECK(pose_of("left_arm").rotation.x * pose_of("left_leg").rotation.x < 0.0F);

    state.swing = 180.0F / 38.17F;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Humanoid, state, poses);
    CHECK(pose_of("left_arm").rotation.x == Approx(57.3F).margin(0.05F));
}

TEST_CASE("a pitched-down head moves the face down", "[entity]") {
    // The sign, by geometry rather than by number: a head looking down (the
    // game's positive pitch) must bring the front of the head lower.
    const auto set = parse_or_fail(R"({
      "format": 2, "models": {"m": {
        "texture_width": 64, "texture_height": 64, "bones": [
          {"name": "head", "parent": "", "pivot": [0, 0, 0], "render": true,
           "quads": [{"n": [0, 0, -1], "v": [[-1, 0, -4, 0, 0], [1, 0, -4, 1, 0],
                                              [1, 2, -4, 1, 1], [-1, 2, -4, 0, 1]]}]}]}}
    })");
    const ov::render::EntityModel* model = set.find("m");
    REQUIRE(model != nullptr);
    ov::render::WalkState state;
    std::vector<ov::render::BonePose> poses;
    Vec3f                             min;
    Vec3f                             max;

    ov::render::pose_model(*model, ov::render::EntityAnimation::Humanoid, state, poses);
    ov::render::entity_bounds(*model, poses, ov::render::EntityPlacement{}, min, max);
    const f32 level = max.y;

    state.head_pitch = 45.0F;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Humanoid, state, poses);
    ov::render::entity_bounds(*model, poses, ov::render::EntityPlacement{}, min, max);
    CHECK(max.y < level);
}

TEST_CASE("standing still poses nothing", "[entity]") {
    const auto set = parse_or_fail(R"({
      "format": 2, "models": {"q": {
        "texture_width": 64, "texture_height": 32, "bones": [
          {"name": "body", "parent": "", "pivot": [0, -5, 2], "rotation": [-90, 0, 0]},
          {"name": "head", "parent": "", "pivot": [0, -4, -8]},
          {"name": "right_hind_leg", "parent": "", "pivot": [-4, -12, 7]},
          {"name": "left_hind_leg", "parent": "", "pivot": [4, -12, 7]},
          {"name": "right_front_leg", "parent": "", "pivot": [-4, -12, -6]},
          {"name": "left_front_leg", "parent": "", "pivot": [4, -12, -6]}
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
      "format": 2, "models": {"q": {
        "texture_width": 64, "texture_height": 32, "bones": [
          {"name": "right_hind_leg", "parent": "", "pivot": [-4, -12, 7]},
          {"name": "left_hind_leg", "parent": "", "pivot": [4, -12, 7]},
          {"name": "right_front_leg", "parent": "", "pivot": [-4, -12, -6]},
          {"name": "left_front_leg", "parent": "", "pivot": [4, -12, -6]}
        ]}}
    })");
    const ov::render::EntityModel* model = set.find("q");
    REQUIRE(model != nullptr);

    ov::render::WalkState state;
    state.amount = 1.0F;
    state.swing  = 60.0F / 38.17F;

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Quadruped, state, poses);

    const auto angle = [&](std::string_view name) {
        return poses[static_cast<usize>(model->bone(name))].rotation.x;
    };
    // The game's right hind leg at +cos(60°) × 80 = +40, through the
    // reflection −40; the left front leg with it; the other pair opposite.
    CHECK(angle("right_hind_leg") == Approx(-40.0F).margin(0.05F));
    CHECK(angle("left_front_leg") == Approx(angle("right_hind_leg")));
    CHECK(angle("left_hind_leg") == Approx(-angle("right_hind_leg")));
    CHECK(angle("right_front_leg") == Approx(-angle("right_hind_leg")));
}

TEST_CASE("the blaze's rods start where the game bakes them", "[entity]") {
    // At age 0 the published orbit puts rod 1 at (0, −1.122, 9) and rod 8 at
    // (4.455, 11.960, 2.270) in the game's space — exactly the pivots the
    // running client's own blaze model was dumped with.
    const auto set = parse_or_fail(R"({
      "format": 2, "models": {"b": {
        "texture_width": 64, "texture_height": 32, "bones": [
          {"name": "part1", "parent": "", "pivot": [0, 1.122, 9]},
          {"name": "part8", "parent": "", "pivot": [4.455, -11.96, 2.27]}
        ]}}
    })");
    const ov::render::EntityModel* model = set.find("b");
    REQUIRE(model != nullptr);
    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Blaze, ov::render::WalkState{},
                           poses);
    for (const ov::render::BonePose& pose : poses) {
        CHECK(pose.offset.x == Approx(0.0F).margin(2e-3));
        CHECK(pose.offset.y == Approx(0.0F).margin(2e-3));
        CHECK(pose.offset.z == Approx(0.0F).margin(2e-3));
    }
}

TEST_CASE("the entity shade is the game's two lights, not the terrain's five values",
          "[entity]") {
    // min(1, (max(0, n·L0) + max(0, n·L1)) × 0.6 + 0.4), L0 = (0.2, 1, −0.7)
    // and L1 = (−0.2, 1, 0.7) normalised: a top face saturates, a bottom face
    // gets the ambient alone, the sides fall between.
    CHECK(ov::render::entity_shade(Vec3f{0.0F, 1.0F, 0.0F}) == Approx(1.0F));
    CHECK(ov::render::entity_shade(Vec3f{0.0F, -1.0F, 0.0F}) == Approx(0.4F));
    CHECK(ov::render::entity_shade(Vec3f{0.0F, 0.0F, 1.0F}) == Approx(0.7396F).margin(1e-4));
    CHECK(ov::render::entity_shade(Vec3f{0.0F, 0.0F, -1.0F}) == Approx(0.7396F).margin(1e-4));
    CHECK(ov::render::entity_shade(Vec3f{1.0F, 0.0F, 0.0F}) == Approx(0.4970F).margin(1e-4));
}

TEST_CASE("a vertex carries the tint, the overlay and the light apart", "[entity]") {
    const auto                     set   = parse_or_fail(kOneCube);
    const ov::render::EntityModel* model = set.find("cube");
    REQUIRE(model != nullptr);
    std::vector<ov::render::BonePose> poses(model->bones.size());
    ov::render::EntityPlacement       placement;
    placement.tint    = 0xFF808080U;
    placement.overlay = 0xB2FF0000U;  // the game's hurt texel: red, 178
    placement.light   = 0x102030U;
    placement.uv      = ov::render::UvRect{0.5F, 0.25F, 0.75F, 0.5F};

    std::vector<ov::render::EntityVertex> vertices;
    REQUIRE(ov::render::emit_entity(*model, poses, placement, vertices) == 6);
    for (const ov::render::EntityVertex& vertex : vertices) {
        CHECK(vertex.overlay[0] == 255);
        CHECK(vertex.overlay[1] == 0);
        CHECK(vertex.overlay[3] == 178);
        CHECK(vertex.light[0] == 0x10);
        CHECK(vertex.light[2] == 0x30);
        // Every coordinate lands inside the model's rectangle on the atlas.
        CHECK(vertex.u >= 0.5F - 1e-6F);
        CHECK(vertex.u <= 0.75F + 1e-6F);
        CHECK(vertex.v >= 0.25F - 1e-6F);
        CHECK(vertex.v <= 0.5F + 1e-6F);
        // The tint is shaded but never brightened past itself.
        CHECK(vertex.colour[0] <= 0x80);
    }
}

TEST_CASE("the head's texture net is the skin layout", "[entity]") {
    const auto                     set   = parse_or_fail(kOneCube);
    const ov::render::EntityModel* model = set.find("cube");
    REQUIRE(model != nullptr);

    std::vector<ov::render::BonePose> poses;
    ov::render::pose_model(*model, ov::render::EntityAnimation::Static,
                           ov::render::WalkState{}, poses);

    ov::render::EntityPlacement           placement;
    std::vector<ov::render::EntityVertex> vertices;
    ov::render::emit_entity(*model, poses, placement, vertices);

    bool found = false;
    for (usize quad = 0; quad * 4 < vertices.size(); ++quad) {
        bool front = true;
        for (usize corner = 0; corner < 4; ++corner) {
            front = front && vertices[quad * 4 + corner].z > 0.2F;
        }
        if (!front) {
            continue;
        }
        found     = true;
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

    for (const ov::render::EntityVertex& vertex : vertices) {
        CHECK(vertex.u >= 0.0F);
        CHECK(vertex.u <= 32.0F / 64.0F + 1e-6F);
    }
}

TEST_CASE("a bone frame follows its bone", "[entity]") {
    const auto                     set   = parse_or_fail(kHumanoid);
    const ov::render::EntityModel* model = set.find("h");
    REQUIRE(model != nullptr);
    std::vector<ov::render::BonePose> poses(model->bones.size());
    ov::render::EntityPlacement       placement;
    placement.position = Vec3f{3.0F, 64.0F, -2.0F};
    placement.origin   = Vec3f{0.0F, 24.016F, 0.0F};
    ov::render::BoneFrame frame;
    REQUIRE(ov::render::bone_frame(*model, poses, placement, model->bone("right_arm"), frame));
    // The right arm's pivot is 5 units to the entity's right and 22 above its
    // feet (−2 + 24.016); at yaw 0 the entity faces +Z, so its right is −X.
    CHECK(frame.origin.x == Approx(3.0F - 5.0F / 16.0F).margin(1e-4));
    CHECK(frame.origin.y == Approx(64.0F + 22.016F / 16.0F).margin(1e-4));
}
