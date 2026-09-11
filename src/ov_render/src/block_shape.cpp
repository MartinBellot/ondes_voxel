#include "ov/render/block_shape.hpp"

#include <algorithm>

namespace ov::render {

namespace {

/// Collision boxes are stored in thirty-seconds of a block.
constexpr f64 kShapeUnit = 1.0 / 32.0;

void unique_sorted(std::vector<f64>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

}  // namespace

void outline_boxes(const registry::BlockRegistry& blocks, registry::BlockStateId state,
                   const BakedModel& model, std::vector<AABB>& out) {
    if (blocks.is_air(blocks.block_of(state))) {
        return;
    }
    const auto collision = blocks.collision_boxes(state);
    if (!collision.empty()) {
        for (const registry::BlockRegistry::Box& box : collision) {
            const f64 max_y = std::min(1.0, static_cast<f64>(box.max_y) * kShapeUnit);
            const f64 min_y = static_cast<f64>(box.min_y) * kShapeUnit;
            if (max_y <= min_y) {
                continue;
            }
            out.push_back(AABB{Vec3d{static_cast<f64>(box.min_x) * kShapeUnit, min_y,
                                     static_cast<f64>(box.min_z) * kShapeUnit},
                               Vec3d{static_cast<f64>(box.max_x) * kShapeUnit, max_y,
                                     static_cast<f64>(box.max_z) * kShapeUnit}});
        }
        return;
    }
    if (model.quads.empty()) {
        return;
    }
    Vec3d low{1e9, 1e9, 1e9};
    Vec3d high{-1e9, -1e9, -1e9};
    for (const BakedQuad& quad : model.quads) {
        for (const BakedVertex& vertex : quad.vertices) {
            low  = Vec3d{std::min(low.x, static_cast<f64>(vertex.position.x)),
                         std::min(low.y, static_cast<f64>(vertex.position.y)),
                         std::min(low.z, static_cast<f64>(vertex.position.z))};
            high = Vec3d{std::max(high.x, static_cast<f64>(vertex.position.x)),
                         std::max(high.y, static_cast<f64>(vertex.position.y)),
                         std::max(high.z, static_cast<f64>(vertex.position.z))};
        }
    }
    // A flat model — a rail, a carpet of one pixel, a pressure plate seen as
    // one quad — still needs a box to outline.
    constexpr f64 kThinnest = 1.0 / 16.0;
    if (high.y - low.y < kThinnest) {
        high.y = low.y + kThinnest;
    }
    out.push_back(AABB{low, high});
}

AABB bounds_of(std::span<const AABB> boxes) noexcept {
    if (boxes.empty()) {
        return AABB{};
    }
    AABB out = boxes.front();
    for (const AABB& box : boxes) {
        out.min = Vec3d{std::min(out.min.x, box.min.x), std::min(out.min.y, box.min.y),
                        std::min(out.min.z, box.min.z)};
        out.max = Vec3d{std::max(out.max.x, box.max.x), std::max(out.max.y, box.max.y),
                        std::max(out.max.z, box.max.z)};
    }
    return out;
}

void shape_edges(std::span<const AABB> boxes, std::vector<std::array<Vec3d, 2>>& out) {
    if (boxes.empty()) {
        return;
    }
    // The grid every box corner lies on.
    std::array<std::vector<f64>, 3> grid;
    for (const AABB& box : boxes) {
        grid[0].push_back(box.min.x);
        grid[0].push_back(box.max.x);
        grid[1].push_back(box.min.y);
        grid[1].push_back(box.max.y);
        grid[2].push_back(box.min.z);
        grid[2].push_back(box.max.z);
    }
    for (auto& axis : grid) {
        unique_sorted(axis);
    }
    const std::array<i32, 3> cells{static_cast<i32>(grid[0].size()) - 1,
                                   static_cast<i32>(grid[1].size()) - 1,
                                   static_cast<i32>(grid[2].size()) - 1};
    if (cells[0] <= 0 || cells[1] <= 0 || cells[2] <= 0) {
        return;
    }

    // Which grid cells the union fills, tested at their centres.
    std::vector<u8> filled(static_cast<usize>(cells[0] * cells[1] * cells[2]), 0);
    const auto      index = [&](i32 x, i32 y, i32 z) {
        return static_cast<usize>((x * cells[1] + y) * cells[2] + z);
    };
    for (i32 x = 0; x < cells[0]; ++x) {
        for (i32 y = 0; y < cells[1]; ++y) {
            for (i32 z = 0; z < cells[2]; ++z) {
                const Vec3d centre{(grid[0][static_cast<usize>(x)] + grid[0][static_cast<usize>(x + 1)]) / 2.0,
                                   (grid[1][static_cast<usize>(y)] + grid[1][static_cast<usize>(y + 1)]) / 2.0,
                                   (grid[2][static_cast<usize>(z)] + grid[2][static_cast<usize>(z + 1)]) / 2.0};
                for (const AABB& box : boxes) {
                    if (centre.x > box.min.x && centre.x < box.max.x && centre.y > box.min.y &&
                        centre.y < box.max.y && centre.z > box.min.z && centre.z < box.max.z) {
                        filled[index(x, y, z)] = 1;
                        break;
                    }
                }
            }
        }
    }
    const auto at = [&](std::array<i32, 3> c) -> bool {
        for (usize a = 0; a < 3; ++a) {
            if (c[a] < 0 || c[a] >= cells[a]) {
                return false;
            }
        }
        return filled[index(c[0], c[1], c[2])] != 0;
    };

    // An edge along axis `a` at grid line (j, k) of the other two, over cell i
    // of axis a, is drawn when the four cells round it do not make a flat
    // surface: one or three filled, or two across a diagonal.
    for (usize a = 0; a < 3; ++a) {
        const usize b = (a + 1) % 3;
        const usize c = (a + 2) % 3;
        for (i32 j = 0; j <= cells[b]; ++j) {
            for (i32 k = 0; k <= cells[c]; ++k) {
                i32 run_start = -1;
                for (i32 i = 0; i <= cells[a]; ++i) {
                    bool visible = false;
                    if (i < cells[a]) {
                        std::array<bool, 4> around{};
                        for (i32 q = 0; q < 4; ++q) {
                            std::array<i32, 3> cell{};
                            cell[a] = i;
                            cell[b] = j - 1 + (q & 1);
                            cell[c] = k - 1 + (q >> 1);
                            around[static_cast<usize>(q)] = at(cell);
                        }
                        const i32 count = static_cast<i32>(around[0]) + around[1] + around[2] +
                                          around[3];
                        visible = count == 1 || count == 3 ||
                                  (count == 2 && around[0] == around[3]);
                    }
                    if (visible && run_start < 0) {
                        run_start = i;
                    } else if (!visible && run_start >= 0) {
                        std::array<f64, 3> from{};
                        std::array<f64, 3> to{};
                        from[a] = grid[a][static_cast<usize>(run_start)];
                        to[a]   = grid[a][static_cast<usize>(i)];
                        from[b] = to[b] = grid[b][static_cast<usize>(j)];
                        from[c] = to[c] = grid[c][static_cast<usize>(k)];
                        out.push_back({Vec3d{from[0], from[1], from[2]}, Vec3d{to[0], to[1], to[2]}});
                        run_start = -1;
                    }
                }
            }
        }
    }
}

}  // namespace ov::render
