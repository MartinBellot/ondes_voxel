#include "ov/render/terrain_vertex.hpp"

#include <algorithm>
#include <cmath>

namespace ov::render {

namespace {

[[nodiscard]] u32 quantise(f32 value, f32 minimum, f32 scale, f32 maximum_steps) noexcept {
    const f32 steps = (value - minimum) * scale;
    return static_cast<u32>(std::lround(std::clamp(steps, 0.0F, maximum_steps)));
}

[[nodiscard]] u32 field(u32 word, u32 offset, u32 bits) noexcept {
    return (word >> offset) & ((1u << bits) - 1u);
}

constexpr f32 kU16Max = 65535.0F;

}  // namespace

TerrainVertex pack_vertex(const TerrainVertexAttributes& attributes) noexcept {
    const u32 x = quantise(attributes.position.x, kPositionMin, kPositionScale, kU16Max);
    const u32 y = quantise(attributes.position.y, kPositionMin, kPositionScale, kU16Max);
    const u32 z = quantise(attributes.position.z, kPositionMin, kPositionScale, kU16Max);
    const u32 u = quantise(attributes.u, 0.0F, kU16Max, kU16Max);
    const u32 v = quantise(attributes.v, 0.0F, kU16Max, kU16Max);

    const u32 sky   = std::min<u32>(attributes.sky_light, 15);
    const u32 block = std::min<u32>(attributes.block_light, 15);
    const u32 ao    = std::min<u32>(attributes.ao, 3);
    const u32 facing =
        attributes.shade ? static_cast<u32>(attributes.facing) : u32{kFacingUnshaded};
    TerrainVertex vertex;
    vertex.words[0] = x | (y << 16);
    vertex.words[1] = z | (u << 16);
    vertex.words[2] = v | (sky << 16) | (block << 20) | (ao << 24) | (facing << 26);
    // Byte order red, green, blue from the low end, so the shader's three
    // bitfieldExtracts read them in the order they are named.
    vertex.words[3] = ((attributes.tint_colour >> 16) & 0xFFu) |
                      (((attributes.tint_colour >> 8) & 0xFFu) << 8) |
                      ((attributes.tint_colour & 0xFFu) << 16);
    return vertex;
}

TerrainVertexAttributes unpack_vertex(const TerrainVertex& vertex) noexcept {
    TerrainVertexAttributes attributes;
    attributes.position.x =
        kPositionMin + static_cast<f32>(field(vertex.words[0], 0, 16)) / kPositionScale;
    attributes.position.y =
        kPositionMin + static_cast<f32>(field(vertex.words[0], 16, 16)) / kPositionScale;
    attributes.position.z =
        kPositionMin + static_cast<f32>(field(vertex.words[1], 0, 16)) / kPositionScale;
    attributes.u = static_cast<f32>(field(vertex.words[1], 16, 16)) / kU16Max;
    attributes.v = static_cast<f32>(field(vertex.words[2], 0, 16)) / kU16Max;

    attributes.sky_light   = static_cast<u8>(field(vertex.words[2], 16, 4));
    attributes.block_light = static_cast<u8>(field(vertex.words[2], 20, 4));
    attributes.ao          = static_cast<u8>(field(vertex.words[2], 24, 2));

    const auto facing = static_cast<u8>(field(vertex.words[2], 26, 3));
    attributes.shade  = facing < kFacingUnshaded;
    attributes.facing = attributes.shade ? static_cast<Direction>(facing) : Direction::Up;
    attributes.tint_colour = (field(vertex.words[3], 0, 8) << 16) |
                             (field(vertex.words[3], 8, 8) << 8) | field(vertex.words[3], 16, 8);
    return attributes;
}

}  // namespace ov::render
