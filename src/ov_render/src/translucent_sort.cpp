#include "ov/render/translucent_sort.hpp"

#include <algorithm>
#include <numeric>

namespace ov::render {

void quad_centres(std::span<const TerrainVertex> vertices, std::vector<Vec3f>& out) {
    out.clear();
    out.reserve(vertices.size() / 4);
    for (usize quad = 0; quad + 3 < vertices.size(); quad += 4) {
        Vec3f sum{};
        for (usize corner = 0; corner < 4; ++corner) {
            const Vec3f p = unpack_vertex(vertices[quad + corner]).position;
            sum           = Vec3f{sum.x + p.x, sum.y + p.y, sum.z + p.z};
        }
        out.push_back(Vec3f{sum.x * 0.25F, sum.y * 0.25F, sum.z * 0.25F});
    }
}

void sort_back_to_front(std::span<const Vec3f> centres, Vec3f eye, std::vector<u32>& order) {
    order.resize(centres.size());
    std::iota(order.begin(), order.end(), 0U);
    const auto distance = [&](u32 quad) {
        const Vec3f& c = centres[quad];
        const f32    dx = c.x - eye.x;
        const f32    dy = c.y - eye.y;
        const f32    dz = c.z - eye.z;
        return dx * dx + dy * dy + dz * dz;
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](u32 a, u32 b) { return distance(a) > distance(b); });
}

void write_quad_indices(std::span<const u32> order, std::vector<u32>& out) {
    out.clear();
    out.reserve(order.size() * 6);
    for (const u32 quad : order) {
        const u32 base = quad * 4;
        out.push_back(base + 0);
        out.push_back(base + 1);
        out.push_back(base + 2);
        out.push_back(base + 0);
        out.push_back(base + 2);
        out.push_back(base + 3);
    }
}

}  // namespace ov::render
