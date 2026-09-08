#include "scene.hpp"

#include <algorithm>
#include <utility>

namespace ov::demo {

namespace {

/// Index 0 is always air, so a zeroed scene is an empty one — the same
/// convention the block registry uses for state 0.
constexpr u8 kAir = 0;

}  // namespace

Scene::Scene(std::vector<BlockKind> palette) : palette_(std::move(palette)) {
    const usize count = static_cast<usize>(kSceneSize) * kSceneHeight * kSceneSize;
    blocks_.assign(count, kAir);
    sky_.assign(count, 15);
    block_.assign(count, 0);
}

bool Scene::inside(i32 x, i32 y, i32 z) {
    return x >= 0 && x < kSceneSize && y >= 0 && y < kSceneHeight && z >= 0 && z < kSceneSize;
}

usize Scene::index(i32 x, i32 y, i32 z) {
    // YZX, the order Minecraft uses everywhere from Anvil to the chunk packet.
    return (static_cast<usize>(y) * kSceneSize + static_cast<usize>(z)) * kSceneSize +
           static_cast<usize>(x);
}

void Scene::set(i32 x, i32 y, i32 z, u8 kind) {
    if (inside(x, y, z)) {
        blocks_[index(x, y, z)] = kind;
    }
}

u8 Scene::at(i32 x, i32 y, i32 z) const {
    return inside(x, y, z) ? blocks_[index(x, y, z)] : kAir;
}

void Scene::relight() {
    std::ranges::fill(sky_, 0);
    std::ranges::fill(block_, 0);

    // Sky light straight down until something solid stops it. Crude, and
    // deliberately so: the real propagation is the server's job and arrives
    // over the wire.
    for (i32 z = 0; z < kSceneSize; ++z) {
        for (i32 x = 0; x < kSceneSize; ++x) {
            u8 level = 15;
            for (i32 y = kSceneHeight - 1; y >= 0; --y) {
                const u8 kind = at(x, y, z);
                if (kind != kAir && palette_[kind].solid) {
                    level = 0;
                }
                sky_[index(x, y, z)] = level;
            }
        }
    }

    // Block light, spread with a simple flood so an emitter lights its
    // surroundings rather than one face.
    for (i32 y = 0; y < kSceneHeight; ++y) {
        for (i32 z = 0; z < kSceneSize; ++z) {
            for (i32 x = 0; x < kSceneSize; ++x) {
                const u8 kind = at(x, y, z);
                if (kind != kAir && palette_[kind].emission > 0) {
                    block_[index(x, y, z)] = palette_[kind].emission;
                }
            }
        }
    }
    for (u8 pass = 0; pass < 15; ++pass) {
        for (i32 y = 0; y < kSceneHeight; ++y) {
            for (i32 z = 0; z < kSceneSize; ++z) {
                for (i32 x = 0; x < kSceneSize; ++x) {
                    const u8 here = block_[index(x, y, z)];
                    if (here <= 1) {
                        continue;
                    }
                    for (u8 face = 0; face < kDirectionCount; ++face) {
                        const Vec3i offset = direction_offset(static_cast<Direction>(face));
                        const i32   nx     = x + offset.x;
                        const i32   ny     = y + offset.y;
                        const i32   nz     = z + offset.z;
                        if (!inside(nx, ny, nz)) {
                            continue;
                        }
                        const u8 kind = at(nx, ny, nz);
                        if (kind != kAir && palette_[kind].solid) {
                            continue;
                        }
                        block_[index(nx, ny, nz)] =
                            std::max<u8>(block_[index(nx, ny, nz)], static_cast<u8>(here - 1));
                    }
                }
            }
        }
    }
}

bool Scene::occludes(Vec3i position, Direction) const {
    const u8 kind = at(position.x, position.y, position.z);
    return kind != kAir && palette_[kind].solid;
}

bool Scene::casts_ambient_occlusion(Vec3i position) const {
    const u8 kind = at(position.x, position.y, position.z);
    return kind != kAir && palette_[kind].solid;
}

u8 Scene::sky_light(Vec3i position) const {
    if (!inside(position.x, position.y, position.z)) {
        // Outside the scene is open sky, so the edge faces are lit rather than
        // black. A real chunk asks its neighbour instead.
        return 15;
    }
    return sky_[index(position.x, position.y, position.z)];
}

u8 Scene::block_light(Vec3i position) const {
    if (!inside(position.x, position.y, position.z)) {
        return 0;
    }
    return block_[index(position.x, position.y, position.z)];
}

Scene build_demo_scene() {
    using render::RenderLayer;
    using render::TintChannel;

    // The palette is chosen to exercise the whole pipeline rather than to look
    // pretty: a tinted top face, a directional log, a cutout that mips and one
    // that does not, and a light source for the block-light channel.
    std::vector<BlockKind> palette{
        BlockKind{"minecraft:air", {}, RenderLayer::Solid, TintChannel::None, false, 0},
        BlockKind{"minecraft:stone", {}, RenderLayer::Solid, TintChannel::None, true, 0},
        BlockKind{"minecraft:dirt", {}, RenderLayer::Solid, TintChannel::None, true, 0},
        BlockKind{"minecraft:grass_block",
                  {{"snowy", "false"}},
                  RenderLayer::Solid,
                  TintChannel::Grass,
                  true,
                  0},
        BlockKind{
            "minecraft:oak_log", {{"axis", "y"}}, RenderLayer::Solid, TintChannel::None, true, 0},
        BlockKind{"minecraft:oak_leaves",
                  {{"distance", "1"}, {"persistent", "false"}, {"waterlogged", "false"}},
                  RenderLayer::CutoutMipped,
                  TintChannel::Foliage,
                  false,
                  0},
        BlockKind{"minecraft:glass", {}, RenderLayer::Cutout, TintChannel::None, false, 0},
        BlockKind{"minecraft:glowstone", {}, RenderLayer::Solid, TintChannel::None, true, 15},
        BlockKind{"minecraft:cobblestone", {}, RenderLayer::Solid, TintChannel::None, true, 0},
    };

    constexpr u8 kStone     = 1;
    constexpr u8 kDirt      = 2;
    constexpr u8 kGrass     = 3;
    constexpr u8 kLog       = 4;
    constexpr u8 kLeaves    = 5;
    constexpr u8 kGlass     = 6;
    constexpr u8 kGlowstone = 7;
    constexpr u8 kCobble    = 8;

    Scene scene(std::move(palette));

    // Ground: stone, dirt, then a grassed surface.
    for (i32 z = 0; z < kSceneSize; ++z) {
        for (i32 x = 0; x < kSceneSize; ++x) {
            scene.set(x, 0, z, kStone);
            scene.set(x, 1, z, kStone);
            scene.set(x, 2, z, kDirt);
            scene.set(x, 3, z, kGrass);
        }
    }

    // A step, so that ambient occlusion has an inside corner to crease.
    for (i32 z = 0; z < 6; ++z) {
        for (i32 x = 0; x < 6; ++x) {
            scene.set(x, 4, z, kCobble);
            if (x < 3 && z < 3) {
                scene.set(x, 5, z, kCobble);
            }
        }
    }

    // Two trees: a log trunk under a cube of leaves. The leaves are the cutout
    // layer, and seeing through them is the check.
    for (const auto& base : {std::pair<i32, i32>{11, 4}, std::pair<i32, i32>{4, 11}}) {
        const i32 tx = base.first;
        const i32 tz = base.second;
        for (i32 y = 4; y <= 7; ++y) {
            scene.set(tx, y, tz, kLog);
        }
        for (i32 dy = 0; dy <= 2; ++dy) {
            for (i32 dz = -2; dz <= 2; ++dz) {
                for (i32 dx = -2; dx <= 2; ++dx) {
                    if (dx == 0 && dz == 0 && dy < 2) {
                        continue;
                    }
                    if (std::abs(dx) + std::abs(dz) + dy > 4) {
                        continue;
                    }
                    scene.set(tx + dx, 7 + dy, tz + dz, kLeaves);
                }
            }
        }
    }

    // A pane of glass, and a light behind it.
    for (i32 y = 4; y <= 6; ++y) {
        for (i32 x = 9; x <= 13; ++x) {
            scene.set(x, y, 14, kGlass);
        }
    }
    scene.set(11, 5, 15, kGlowstone);

    scene.relight();
    return scene;
}

}  // namespace ov::demo
