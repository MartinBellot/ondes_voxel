#include "ov/render/terrain_vertex.hpp"

#include <algorithm>
#include <cmath>

namespace ov::render {

namespace {

[[nodiscard]] u64 quantise(f32 value, f32 minimum, f32 scale, u64 bits) noexcept {
    const f32  steps   = (value - minimum) * scale;
    const auto maximum = static_cast<f32>((1ULL << bits) - 1);
    return static_cast<u64>(std::lround(std::clamp(steps, 0.0F, maximum)));
}

[[nodiscard]] u64 field(u64 packed, u32 offset, u32 bits) noexcept {
    return (packed >> offset) & ((1ULL << bits) - 1);
}

}  // namespace

TerrainVertex pack_vertex(const TerrainVertexAttributes& attributes) noexcept {
    const u64 x = quantise(attributes.position.x, kPositionMin, kPositionScale, 11);
    const u64 y = quantise(attributes.position.y, kPositionMin, kPositionScale, 11);
    const u64 z = quantise(attributes.position.z, kPositionMin, kPositionScale, 11);
    const u64 u = quantise(attributes.u, 0.0F, kTexCoordScale, 8);
    const u64 v = quantise(attributes.v, 0.0F, kTexCoordScale, 8);

    const u64 sky   = std::min<u64>(attributes.sky_light, 15);
    const u64 block = std::min<u64>(attributes.block_light, 15);
    const u64 ao    = std::min<u64>(attributes.ao, 3);
    const u64 facing =
        attributes.shade ? static_cast<u64>(attributes.facing) : u64{kFacingUnshaded};
    const u64 tint = static_cast<u64>(attributes.tint) & 0x3ULL;

    TerrainVertex vertex;
    vertex.packed = x | (y << 11) | (z << 22) | (u << 33) | (v << 41) | (sky << 49) |
                    (block << 53) | (ao << 57) | (facing << 59) | (tint << 62);
    return vertex;
}

TerrainVertexAttributes unpack_vertex(TerrainVertex vertex) noexcept {
    TerrainVertexAttributes attributes;
    attributes.position.x =
        kPositionMin + static_cast<f32>(field(vertex.packed, 0, 11)) / kPositionScale;
    attributes.position.y =
        kPositionMin + static_cast<f32>(field(vertex.packed, 11, 11)) / kPositionScale;
    attributes.position.z =
        kPositionMin + static_cast<f32>(field(vertex.packed, 22, 11)) / kPositionScale;
    attributes.u = static_cast<f32>(field(vertex.packed, 33, 8)) / kTexCoordScale;
    attributes.v = static_cast<f32>(field(vertex.packed, 41, 8)) / kTexCoordScale;

    attributes.sky_light   = static_cast<u8>(field(vertex.packed, 49, 4));
    attributes.block_light = static_cast<u8>(field(vertex.packed, 53, 4));
    attributes.ao          = static_cast<u8>(field(vertex.packed, 57, 2));

    const auto facing = static_cast<u8>(field(vertex.packed, 59, 3));
    attributes.shade  = facing < kFacingUnshaded;
    attributes.facing = attributes.shade ? static_cast<Direction>(facing) : Direction::Up;
    attributes.tint   = static_cast<TintChannel>(field(vertex.packed, 62, 2));
    return attributes;
}

}  // namespace ov::render
