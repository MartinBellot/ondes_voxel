#include "ov/render/baked_model.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

namespace {

ResourceLocation location(std::string_view text) {
    auto parsed = ResourceLocation::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

ModelVariant plain_variant() {
    return ModelVariant{.model = location("block/x")};
}

/// A full-block element with all six faces and no explicit uv.
Element full_cube() {
    Element element;
    element.from = {0.0F, 0.0F, 0.0F};
    element.to   = {16.0F, 16.0F, 16.0F};
    for (u8 i = 0; i < kDirectionCount; ++i) {
        FaceDefinition face;
        face.sprite      = "minecraft:block/stone";
        face.cullface    = static_cast<Direction>(i);
        element.faces[i] = face;
    }
    return element;
}

const BakedQuad& quad_facing(const BakedModel& model, Direction direction) {
    const auto it = std::ranges::find_if(
        model.quads, [direction](const BakedQuad& quad) { return quad.facing == direction; });
    REQUIRE(it != model.quads.end());
    return *it;
}

struct Bounds {
    f32 u_min{}, v_min{}, u_max{}, v_max{};
};

Bounds uv_bounds(const BakedQuad& quad) {
    Bounds bounds{quad.vertices[0].u, quad.vertices[0].v, quad.vertices[0].u, quad.vertices[0].v};
    for (const auto& vertex : quad.vertices) {
        bounds.u_min = std::min(bounds.u_min, vertex.u);
        bounds.v_min = std::min(bounds.v_min, vertex.v);
        bounds.u_max = std::max(bounds.u_max, vertex.u);
        bounds.v_max = std::max(bounds.v_max, vertex.v);
    }
    return bounds;
}

/// The quad's geometric normal, from the winding order.
Vec3f quad_normal(const BakedQuad& quad) {
    return (quad.vertices[1].position - quad.vertices[0].position)
        .cross(quad.vertices[3].position - quad.vertices[0].position);
}

/// The texture coordinate the quad carries at a given world position.
std::optional<std::array<f32, 2>> uv_at(const BakedQuad& quad, Vec3f position) {
    for (const auto& vertex : quad.vertices) {
        if ((vertex.position - position).length_squared() < 1e-6F) {
            return std::array<f32, 2>{vertex.u, vertex.v};
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("blockstate rotation turns directions the way the game does", "[bake]") {
    // Both facts are measured from the 1.20.1 assets, not chosen:
    // minecraft:furnace is modelled facing north and uses y=90 for east;
    // minecraft:observer is modelled facing north and uses x=90 for down.
    // Together they fix the handedness of both rotations.
    CHECK(rotate_direction(Direction::North, 0, 90) == Direction::East);
    CHECK(rotate_direction(Direction::North, 0, 180) == Direction::South);
    CHECK(rotate_direction(Direction::North, 0, 270) == Direction::West);
    CHECK(rotate_direction(Direction::North, 90, 0) == Direction::Down);
    CHECK(rotate_direction(Direction::North, 270, 0) == Direction::Up);

    // A rotation about y leaves the vertical faces alone, and one about x
    // leaves the east/west ones alone.
    CHECK(rotate_direction(Direction::Up, 0, 90) == Direction::Up);
    CHECK(rotate_direction(Direction::East, 90, 0) == Direction::East);
}

TEST_CASE("a full cube bakes to six outward quads with whole-sprite uv", "[bake]") {
    Model model;
    model.elements.push_back(full_cube());

    const auto baked = bake(model, plain_variant());
    REQUIRE(baked.quads.size() == 6);

    for (u8 i = 0; i < kDirectionCount; ++i) {
        const auto  direction = static_cast<Direction>(i);
        const auto& quad      = quad_facing(baked, direction);

        const auto bounds = uv_bounds(quad);
        CHECK(bounds.u_min == Approx(0.0F));
        CHECK(bounds.v_min == Approx(0.0F));
        CHECK(bounds.u_max == Approx(16.0F));
        CHECK(bounds.v_max == Approx(16.0F));

        // Winding has to give an outward normal, whatever the face. The uv
        // mapping is not consistently handed — up and down share one, which is
        // a vanilla quirk — so the baker fixes the winding rather than
        // assuming it.
        const Vec3i offset    = direction_offset(direction);
        const Vec3f normal    = quad_normal(quad);
        const f32   alignment = normal.x * static_cast<f32>(offset.x) +
                                normal.y * static_cast<f32>(offset.y) +
                                normal.z * static_cast<f32>(offset.z);
        CHECK(alignment > 0.0F);

        CHECK(quad.cullface == direction);
        CHECK(quad.shade);

        for (const auto& vertex : quad.vertices) {
            CHECK(vertex.position.x >= Approx(0.0F));
            CHECK(vertex.position.x <= Approx(1.0F));
            CHECK(vertex.position.y >= Approx(0.0F));
            CHECK(vertex.position.y <= Approx(1.0F));
        }
    }
}

TEST_CASE("generated uv reproduces the numbers vanilla writes by hand", "[bake]") {
    // block/stairs writes explicit uv for its upper step, an element that spans
    // x 8..16, y 8..16, z 0..16. Those numbers are the corroboration for the
    // per-face uv mapping in docs/PROVENANCE.md: the generator has to produce
    // exactly them when `uv` is omitted.
    Element element;
    element.from = {8.0F, 8.0F, 0.0F};
    element.to   = {16.0F, 16.0F, 16.0F};
    for (u8 i = 0; i < kDirectionCount; ++i) {
        FaceDefinition face;
        face.sprite      = "minecraft:block/oak_planks";
        element.faces[i] = face;
    }

    Model model;
    model.elements.push_back(element);
    const auto baked = bake(model, plain_variant());

    const auto expect = [&baked](Direction direction, Bounds wanted) {
        const auto bounds = uv_bounds(quad_facing(baked, direction));
        CHECK(bounds.u_min == Approx(wanted.u_min));
        CHECK(bounds.v_min == Approx(wanted.v_min));
        CHECK(bounds.u_max == Approx(wanted.u_max));
        CHECK(bounds.v_max == Approx(wanted.v_max));
    };

    expect(Direction::Up, {8.0F, 0.0F, 16.0F, 16.0F});
    expect(Direction::North, {0.0F, 0.0F, 8.0F, 8.0F});
    expect(Direction::South, {8.0F, 0.0F, 16.0F, 8.0F});
    expect(Direction::West, {0.0F, 0.0F, 16.0F, 8.0F});
    expect(Direction::East, {0.0F, 0.0F, 16.0F, 8.0F});
}

TEST_CASE("an explicit uv is used as written", "[bake]") {
    Element        element = full_cube();
    FaceDefinition face;
    face.sprite                                      = "minecraft:block/stone";
    face.uv                                          = std::array<f32, 4>{4.0F, 2.0F, 12.0F, 10.0F};
    element.faces[static_cast<usize>(Direction::Up)] = face;

    Model model;
    model.elements.push_back(element);
    const auto baked  = bake(model, plain_variant());
    const auto bounds = uv_bounds(quad_facing(baked, Direction::Up));

    CHECK(bounds.u_min == Approx(4.0F));
    CHECK(bounds.v_min == Approx(2.0F));
    CHECK(bounds.u_max == Approx(12.0F));
    CHECK(bounds.v_max == Approx(10.0F));
}

TEST_CASE("a face rotation of 180 degrees swaps opposite texture corners", "[bake]") {
    const auto up_face_uv = [](i32 rotation) {
        Element        element = full_cube();
        FaceDefinition face;
        face.sprite   = "minecraft:block/stone";
        face.uv       = std::array<f32, 4>{0.0F, 0.0F, 16.0F, 16.0F};
        face.rotation = rotation;
        element.faces[static_cast<usize>(Direction::Up)] = face;

        Model model;
        model.elements.push_back(element);
        return bake(model, plain_variant());
    };

    const auto zero = up_face_uv(0);
    const auto half = up_face_uv(180);

    const Vec3f corner{0.0F, 1.0F, 0.0F};
    const auto  a = uv_at(quad_facing(zero, Direction::Up), corner);
    const auto  b = uv_at(quad_facing(half, Direction::Up), corner);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    CHECK((*b)[0] == Approx(16.0F - (*a)[0]));
    CHECK((*b)[1] == Approx(16.0F - (*a)[1]));
}

TEST_CASE("uvlock keeps the texture where the world put it", "[bake]") {
    // The definition, stated as a test: with uvlock, a point of the block that
    // did not move must keep the texture coordinate it had. The top face of a
    // cube rotated about y is exactly that case — it is why a rotated fence
    // gate's planks still run the same way as its neighbour's.
    const auto bake_top = [](i32 y_degrees, bool uvlock) {
        Element        element = full_cube();
        FaceDefinition face;
        face.sprite = "minecraft:block/stone";
        face.uv     = std::array<f32, 4>{0.0F, 0.0F, 16.0F, 16.0F};
        element.faces[static_cast<usize>(Direction::Up)] = face;

        Model model;
        model.elements.push_back(element);
        return bake(model, ModelVariant{.model  = ResourceLocation::parse("block/x").value(),
                                        .y      = y_degrees,
                                        .uvlock = uvlock});
    };

    const auto  reference = bake_top(0, false);
    const auto& flat      = quad_facing(reference, Direction::Up);

    const std::array<Vec3f, 4> corners{Vec3f{0.0F, 1.0F, 0.0F}, Vec3f{1.0F, 1.0F, 0.0F},
                                       Vec3f{1.0F, 1.0F, 1.0F}, Vec3f{0.0F, 1.0F, 1.0F}};

    const auto  locked  = bake_top(90, true);
    const auto& rotated = quad_facing(locked, Direction::Up);
    for (const auto& corner : corners) {
        const auto before = uv_at(flat, corner);
        const auto after  = uv_at(rotated, corner);
        REQUIRE(before.has_value());
        REQUIRE(after.has_value());
        CHECK((*after)[0] == Approx((*before)[0]));
        CHECK((*after)[1] == Approx((*before)[1]));
    }

    // Without uvlock the texture turns with the block, which is the default and
    // must not silently be the same thing.
    const auto  unlocked = bake_top(90, false);
    const auto& turned   = quad_facing(unlocked, Direction::Up);
    bool        differs  = false;
    for (const auto& corner : corners) {
        const auto before = uv_at(flat, corner);
        const auto after  = uv_at(turned, corner);
        REQUIRE(before.has_value());
        REQUIRE(after.has_value());
        differs =
            differs || (*after)[0] != Approx((*before)[0]) || (*after)[1] != Approx((*before)[1]);
    }
    CHECK(differs);
}

TEST_CASE("cullface follows the blockstate rotation", "[bake]") {
    Element        element = full_cube();
    FaceDefinition face;
    face.sprite                                         = "minecraft:block/stone";
    face.cullface                                       = Direction::North;
    element.faces                                       = {};
    element.faces[static_cast<usize>(Direction::North)] = face;

    Model model;
    model.elements.push_back(element);
    const auto baked =
        bake(model, ModelVariant{.model = ResourceLocation::parse("block/x").value(), .y = 90});

    REQUIRE(baked.quads.size() == 1);
    CHECK(baked.quads[0].cullface == Direction::East);
    CHECK(baked.quads[0].facing == Direction::East);
}

TEST_CASE("rescale stretches a rotated element back to the block edge", "[bake]") {
    // block/cross: two quads meeting at 45°, each 0.8..15.2 wide, rescaled so
    // that the rotated corners land back on 0.8 and 15.2. Without rescale a
    // flower is visibly narrower than the block it stands in.
    Element element;
    element.from     = {0.8F, 0.0F, 8.0F};
    element.to       = {15.2F, 16.0F, 8.0F};
    element.rotation = ElementRotation{
        .origin = {8.0F, 8.0F, 8.0F}, .axis = Axis::Y, .angle = 45.0F, .rescale = true};
    element.shade = false;

    FaceDefinition face;
    face.sprite                                         = "minecraft:block/dandelion";
    element.faces[static_cast<usize>(Direction::North)] = face;

    Model model;
    model.elements.push_back(element);
    const auto baked = bake(model, plain_variant());

    REQUIRE(baked.quads.size() == 1);
    CHECK_FALSE(baked.quads[0].shade);

    // 7.2 units either side of centre, rotated 45° and rescaled by 1/cos(45°),
    // lands back at 7.2 on both axes: 0.05 and 0.95 in block space.
    f32 min_x = 1e9F;
    f32 max_x = -1e9F;
    f32 min_z = 1e9F;
    f32 max_z = -1e9F;
    for (const auto& vertex : baked.quads[0].vertices) {
        min_x = std::min(min_x, vertex.position.x);
        max_x = std::max(max_x, vertex.position.x);
        min_z = std::min(min_z, vertex.position.z);
        max_z = std::max(max_z, vertex.position.z);
    }
    CHECK(min_x == Approx(0.05F).margin(1e-4));
    CHECK(max_x == Approx(0.95F).margin(1e-4));
    CHECK(min_z == Approx(0.05F).margin(1e-4));
    CHECK(max_z == Approx(0.95F).margin(1e-4));
}

TEST_CASE("a face with no definition emits no quad", "[bake]") {
    Element element;
    element.from = {0.0F, 0.0F, 0.0F};
    element.to   = {16.0F, 16.0F, 16.0F};

    FaceDefinition face;
    face.sprite                                      = "minecraft:block/stone";
    element.faces[static_cast<usize>(Direction::Up)] = face;

    Model model;
    model.elements.push_back(element);
    const auto baked = bake(model, plain_variant());

    CHECK(baked.quads.size() == 1);
    CHECK(baked.quads[0].facing == Direction::Up);
}
