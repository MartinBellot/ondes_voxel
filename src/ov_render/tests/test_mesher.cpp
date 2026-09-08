#include "ov/render/mesher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::render;

namespace {

/// A world of exactly the blocks the test names. Everything else is air.
class FakeWorld final : public NeighbourhoodView {
public:
    void add_solid(Vec3i position) { solid_.insert(key(position)); }

    void set_light(Vec3i position, u8 sky, u8 block) {
        light_[key(position)] = std::pair<u8, u8>{sky, block};
    }

    [[nodiscard]] bool occludes(Vec3i position, Direction) const override {
        return solid_.contains(key(position));
    }

    [[nodiscard]] bool casts_ambient_occlusion(Vec3i position) const override {
        return solid_.contains(key(position));
    }

    [[nodiscard]] u8 sky_light(Vec3i position) const override {
        const auto it = light_.find(key(position));
        return it == light_.end() ? 15 : it->second.first;
    }

    [[nodiscard]] u8 block_light(Vec3i position) const override {
        const auto it = light_.find(key(position));
        return it == light_.end() ? 0 : it->second.second;
    }

private:
    [[nodiscard]] static i64 key(Vec3i position) {
        return (static_cast<i64>(position.y) << 40) ^ (static_cast<i64>(position.z) << 20) ^
               position.x;
    }

    std::set<i64>                    solid_;
    std::map<i64, std::pair<u8, u8>> light_;
};

/// A one-element cube with all six faces, each declaring its own cullface.
BakedModel full_cube(const std::string& sprite = "minecraft:block/stone") {
    Element element;
    element.from = {0.0F, 0.0F, 0.0F};
    element.to   = {16.0F, 16.0F, 16.0F};
    for (u8 i = 0; i < kDirectionCount; ++i) {
        FaceDefinition face;
        face.sprite      = sprite;
        face.cullface    = static_cast<Direction>(i);
        element.faces[i] = face;
    }

    Model model;
    model.elements.push_back(element);
    return bake(model, ModelVariant{.model = ResourceLocation::parse("block/x").value()});
}

TextureAtlas single_sprite_atlas(const std::string& sprite) {
    MemoryAssetSource source;
    AtlasBuilder      builder(source);
    builder.add(sprite);
    auto atlas = builder.build();
    REQUIRE(atlas.has_value());
    return std::move(*atlas);
}

}  // namespace

TEST_CASE("the shared index buffer is the quad pattern, repeated", "[mesher]") {
    // One buffer for all terrain, bound once, each section drawing from it with
    // its own vertexOffset. That is what removes every byte of per-section
    // index memory.
    const auto indices = build_shared_quad_indices(2);

    REQUIRE(indices.size() == 12);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 1);
    CHECK(indices[2] == 2);
    CHECK(indices[3] == 0);
    CHECK(indices[4] == 2);
    CHECK(indices[5] == 3);
    CHECK(indices[6] == 4);
    CHECK(indices[11] == 7);
}

TEST_CASE("an isolated block emits all six of its faces", "[mesher]") {
    const auto  atlas = single_sprite_atlas("minecraft:block/stone");
    const auto  model = full_cube();
    FakeWorld   world;
    MeshBuffers mesh;

    emit_block(model, Vec3i{0, 0, 0}, BlockRenderInfo{}, atlas, world, mesh);

    CHECK(mesh[RenderLayer::Solid].size() == 6 * 4);
    CHECK(mesh.total_vertices() == 24);
}

TEST_CASE("a face against a solid neighbour is dropped", "[mesher]") {
    // The single biggest reduction there is: the inside of a hill emits
    // nothing at all.
    const auto atlas = single_sprite_atlas("minecraft:block/stone");
    const auto model = full_cube();

    FakeWorld world;
    world.add_solid(Vec3i{0, 1, 0});   // above
    world.add_solid(Vec3i{0, -1, 0});  // below

    MeshBuffers mesh;
    emit_block(model, Vec3i{0, 0, 0}, BlockRenderInfo{}, atlas, world, mesh);

    CHECK(mesh[RenderLayer::Solid].size() == 4 * 4);
}

TEST_CASE("a block walled in on all sides emits nothing", "[mesher]") {
    const auto atlas = single_sprite_atlas("minecraft:block/stone");
    const auto model = full_cube();

    FakeWorld world;
    for (u8 i = 0; i < kDirectionCount; ++i) {
        world.add_solid(direction_offset(static_cast<Direction>(i)));
    }

    MeshBuffers mesh;
    emit_block(model, Vec3i{0, 0, 0}, BlockRenderInfo{}, atlas, world, mesh);

    CHECK(mesh.total_vertices() == 0);
}

TEST_CASE("blocks land at their own position", "[mesher]") {
    const auto atlas = single_sprite_atlas("minecraft:block/stone");
    const auto model = full_cube();
    FakeWorld  world;

    MeshBuffers mesh;
    emit_block(model, Vec3i{3, 5, 7}, BlockRenderInfo{}, atlas, world, mesh);

    REQUIRE_FALSE(mesh[RenderLayer::Solid].empty());
    for (const auto& vertex : mesh[RenderLayer::Solid]) {
        const auto attributes = unpack_vertex(vertex);
        CHECK(attributes.position.x >= 3.0F);
        CHECK(attributes.position.x <= 4.0F);
        CHECK(attributes.position.y >= 5.0F);
        CHECK(attributes.position.y <= 6.0F);
        CHECK(attributes.position.z >= 7.0F);
        CHECK(attributes.position.z <= 8.0F);
    }
}

TEST_CASE("a corner with two solid sides is fully occluded", "[mesher]") {
    // The rule that decides whether an inside corner has a crease or looks
    // inflated, checked through the mesher rather than only through ao_level:
    // it is the mapping from a vertex to *which* three neighbours it asks
    // about that is easy to get wrong.
    const auto atlas = single_sprite_atlas("minecraft:block/stone");
    const auto model = full_cube();

    FakeWorld world;
    // Two blocks beside the +X/+Z corner of the top face, and nothing else.
    world.add_solid(Vec3i{1, 1, 0});
    world.add_solid(Vec3i{0, 1, 1});

    MeshBuffers mesh;
    emit_block(model, Vec3i{0, 0, 0}, BlockRenderInfo{}, atlas, world, mesh);

    bool found_dark = false;
    bool found_open = false;
    for (const auto& vertex : mesh[RenderLayer::Solid]) {
        const auto attributes = unpack_vertex(vertex);
        if (attributes.facing != Direction::Up) {
            continue;
        }
        if (attributes.position.x > 0.9F && attributes.position.z > 0.9F) {
            found_dark = found_dark || attributes.ao == 0;
        }
        if (attributes.position.x < 0.1F && attributes.position.z < 0.1F) {
            found_open = found_open || attributes.ao == 3;
        }
    }
    CHECK(found_dark);
    CHECK(found_open);
}

TEST_CASE("layers keep their vertices apart", "[mesher]") {
    const auto atlas = single_sprite_atlas("minecraft:block/stone");
    const auto model = full_cube();
    FakeWorld  world;

    MeshBuffers mesh;
    emit_block(model, Vec3i{0, 0, 0}, BlockRenderInfo{RenderLayer::CutoutMipped}, atlas, world,
               mesh);

    CHECK(mesh[RenderLayer::Solid].empty());
    CHECK(mesh[RenderLayer::CutoutMipped].size() == 24);
}

TEST_CASE("texture coordinates land inside the sprite's rect", "[mesher]") {
    const auto atlas  = single_sprite_atlas("minecraft:block/stone");
    const auto sprite = atlas.uv("minecraft:block/stone");
    const auto model  = full_cube();
    FakeWorld  world;

    MeshBuffers mesh;
    emit_block(model, Vec3i{0, 0, 0}, BlockRenderInfo{}, atlas, world, mesh);

    for (const auto& vertex : mesh[RenderLayer::Solid]) {
        const auto attributes = unpack_vertex(vertex);
        CHECK(attributes.u >= sprite.u0 - 1e-3F);
        CHECK(attributes.u <= sprite.u1 + 1e-3F);
        CHECK(attributes.v >= sprite.v0 - 1e-3F);
        CHECK(attributes.v <= sprite.v1 + 1e-3F);
    }
}
