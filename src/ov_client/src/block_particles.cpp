#include "ov/client/block_particles.hpp"

#include <algorithm>
#include <cmath>

namespace ov::client {

namespace {

// ── The constants, each named. None was measured against the real client
// yet; docs/provenance/cassage-bloc.md keeps the list. ──

/// Every block particle is drawn at 0.6 of its texture's colour.
constexpr f32 kBrightness = 0.6F;
/// Lifetime in ticks is 4 / (r * 0.9 + 0.1) for r uniform in [0, 1): 4 to 40.
constexpr f32 kLifetimeNumerator = 4.0F;
/// Half-width: 0.1 * (r * 0.5 + 0.5) * 2, then halved for a block particle.
constexpr f32 kBaseSize = 0.1F;
/// Initial speed: a random push of up to 0.4 an axis added to the caller's,
/// renormalised to (r1 + r2 + 1) * 0.15 * 0.4, plus 0.1 upwards.
constexpr f64 kJitter    = 0.4;
constexpr f64 kSpeedUnit = 0.15;
constexpr f64 kSpeedDamp = 0.4;
constexpr f64 kLift      = 0.1;
/// Per tick: gravity 0.04, drag 0.98, and 0.7 more horizontally on the ground.
constexpr f64 kGravity      = 0.04;
constexpr f64 kFriction     = 0.98;
constexpr f64 kGroundDrag   = 0.7;
/// The collision box is 0.2 on a side.
constexpr f64 kHalfExtent = 0.1;
/// A crack is slower and smaller: speed times 0.2 (the lift kept), size 0.6.
constexpr f64 kCrackPower = 0.2;
constexpr f32 kCrackScale = 0.6F;
/// A crack particle is placed 0.1 in from the edges of the face and 0.1 out.
constexpr f64 kCrackInset = 0.1;
/// A destroy grid has at least two cells a side and one per quarter block.
constexpr f64 kCellSize = 0.25;

[[nodiscard]] std::array<f32, 3> tint_of(u32 tint) noexcept {
    return {kBrightness * static_cast<f32>((tint >> 16U) & 0xFFU) / 255.0F,
            kBrightness * static_cast<f32>((tint >> 8U) & 0xFFU) / 255.0F,
            kBrightness * static_cast<f32>(tint & 0xFFU) / 255.0F};
}

}  // namespace

BlockParticle& BlockParticles::spawn(Vec3d at, Vec3d push, const ParticleSprite& sprite,
                                     u32 tint) {
    if (live_.size() >= kMaxParticles) {
        live_.erase(live_.begin());
    }
    BlockParticle p;
    p.position = at;
    p.previous = at;

    // Every draw in its own statement: the order is the stream's order.
    const f32 life_draw = random_.next_float();
    p.lifetime          = static_cast<i32>(kLifetimeNumerator / (life_draw * 0.9F + 0.1F));
    const f32 size_draw = random_.next_float();
    p.size              = kBaseSize * (size_draw * 0.5F + 0.5F) * 2.0F / 2.0F;

    const f64 jx = random_.next_double();
    const f64 jy = random_.next_double();
    const f64 jz = random_.next_double();
    Vec3d     v{push.x + (jx * 2.0 - 1.0) * kJitter, push.y + (jy * 2.0 - 1.0) * kJitter,
            push.z + (jz * 2.0 - 1.0) * kJitter};
    const f64 s1     = random_.next_double();
    const f64 s2     = random_.next_double();
    const f64 speed  = (s1 + s2 + 1.0) * kSpeedUnit;
    const f64 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length > 0.0) {
        v = Vec3d{v.x / length * speed * kSpeedDamp, v.y / length * speed * kSpeedDamp + kLift,
                  v.z / length * speed * kSpeedDamp};
    }
    p.velocity = v;

    // A quarter of the sprite at a random offset, mirrored horizontally as the
    // game samples it.
    const f32 uo = random_.next_float() * 3.0F;
    const f32 vo = random_.next_float() * 3.0F;
    const f32 du = sprite.u1 - sprite.u0;
    const f32 dv = sprite.v1 - sprite.v0;
    p.uv         = {sprite.u0 + du * (uo + 1.0F) / 4.0F, sprite.v0 + dv * vo / 4.0F,
                    sprite.u0 + du * uo / 4.0F, sprite.v0 + dv * (vo + 1.0F) / 4.0F};
    p.colour     = tint_of(tint);
    p.half_extent = kHalfExtent;
    live_.push_back(p);
    return live_.back();
}

void BlockParticles::destroy(BlockPos pos, std::span<const AABB> shape,
                             const ParticleSprite& sprite, u32 tint) {
    for (const AABB& box : shape) {
        const f64 dx = std::min(1.0, box.max.x - box.min.x);
        const f64 dy = std::min(1.0, box.max.y - box.min.y);
        const f64 dz = std::min(1.0, box.max.z - box.min.z);
        const i32 nx = std::max(2, static_cast<i32>(std::ceil(dx / kCellSize)));
        const i32 ny = std::max(2, static_cast<i32>(std::ceil(dy / kCellSize)));
        const i32 nz = std::max(2, static_cast<i32>(std::ceil(dz / kCellSize)));
        for (i32 ix = 0; ix < nx; ++ix) {
            for (i32 iy = 0; iy < ny; ++iy) {
                for (i32 iz = 0; iz < nz; ++iz) {
                    const f64 fx = (static_cast<f64>(ix) + 0.5) / static_cast<f64>(nx);
                    const f64 fy = (static_cast<f64>(iy) + 0.5) / static_cast<f64>(ny);
                    const f64 fz = (static_cast<f64>(iz) + 0.5) / static_cast<f64>(nz);
                    const Vec3d at{static_cast<f64>(pos.x) + fx * dx + box.min.x,
                                   static_cast<f64>(pos.y) + fy * dy + box.min.y,
                                   static_cast<f64>(pos.z) + fz * dz + box.min.z};
                    (void)spawn(at, Vec3d{fx - 0.5, fy - 0.5, fz - 0.5}, sprite, tint);
                }
            }
        }
    }
}

void BlockParticles::crack(BlockPos pos, Direction face, const AABB& bounds,
                           const ParticleSprite& sprite, u32 tint) {
    const auto across = [&](f64 lo, f64 hi) {
        const f64 draw = random_.next_double();
        return draw * (hi - lo - kCrackInset * 2.0) + kCrackInset + lo;
    };
    Vec3d at{static_cast<f64>(pos.x), static_cast<f64>(pos.y), static_cast<f64>(pos.z)};
    const f64 ox = across(bounds.min.x, bounds.max.x);
    const f64 oy = across(bounds.min.y, bounds.max.y);
    const f64 oz = across(bounds.min.z, bounds.max.z);
    at            = Vec3d{at.x + ox, at.y + oy, at.z + oz};
    switch (face) {
        case Direction::Down: at.y = pos.y + bounds.min.y - kCrackInset; break;
        case Direction::Up: at.y = pos.y + bounds.max.y + kCrackInset; break;
        case Direction::North: at.z = pos.z + bounds.min.z - kCrackInset; break;
        case Direction::South: at.z = pos.z + bounds.max.z + kCrackInset; break;
        case Direction::West: at.x = pos.x + bounds.min.x - kCrackInset; break;
        case Direction::East: at.x = pos.x + bounds.max.x + kCrackInset; break;
    }
    BlockParticle& p = spawn(at, Vec3d{0.0, 0.0, 0.0}, sprite, tint);
    p.velocity       = Vec3d{p.velocity.x * kCrackPower, (p.velocity.y - kLift) * kCrackPower + kLift,
                       p.velocity.z * kCrackPower};
    p.size *= kCrackScale;
    p.half_extent *= static_cast<f64>(kCrackScale);
}

void BlockParticles::tick(const gameplay::CollisionWorld* world) {
    usize kept = 0;
    for (usize i = 0; i < live_.size(); ++i) {
        BlockParticle p = live_[i];
        p.previous      = p.position;
        if (p.age++ >= p.lifetime) {
            continue;
        }
        p.velocity.y -= kGravity * static_cast<f64>(p.gravity);
        Vec3d moved = p.velocity;
        if (world != nullptr) {
            const AABB box{Vec3d{p.position.x - p.half_extent, p.position.y,
                                 p.position.z - p.half_extent},
                           Vec3d{p.position.x + p.half_extent, p.position.y + p.half_extent * 2.0,
                                 p.position.z + p.half_extent}};
            moved = world->slide(box, p.velocity);
        }
        p.on_ground = moved.y != p.velocity.y && p.velocity.y < 0.0;
        if (moved.x != p.velocity.x) {
            p.velocity.x = 0.0;
        }
        if (moved.z != p.velocity.z) {
            p.velocity.z = 0.0;
        }
        p.position = Vec3d{p.position.x + moved.x, p.position.y + moved.y, p.position.z + moved.z};
        p.velocity = Vec3d{p.velocity.x * kFriction, p.velocity.y * kFriction,
                           p.velocity.z * kFriction};
        if (p.on_ground) {
            p.velocity.x *= kGroundDrag;
            p.velocity.z *= kGroundDrag;
        }
        live_[kept++] = p;
    }
    live_.resize(kept);
}

void BlockParticles::build(Vec3f camera_right, Vec3f camera_up, f32 partial,
                           const ParticleLight& light,
                           std::vector<render::EntityVertex>& out) const {
    for (const BlockParticle& p : live_) {
        const f64 t = static_cast<f64>(partial);
        const Vec3f centre{static_cast<f32>(p.previous.x + (p.position.x - p.previous.x) * t),
                           static_cast<f32>(p.previous.y + (p.position.y - p.previous.y) * t),
                           static_cast<f32>(p.previous.z + (p.position.z - p.previous.z) * t)};
        const Vec3f right = camera_right * p.size;
        const Vec3f up    = camera_up * p.size;
        const std::array<u8, 3> lit = light ? light(centre) : std::array<u8, 3>{255, 255, 255};
        std::array<u8, 4> colour{};
        for (usize c = 0; c < 3; ++c) {
            colour[c] = static_cast<u8>(std::clamp(p.colour[c] * static_cast<f32>(lit[c]), 0.0F,
                                                   255.0F));
        }
        colour[3] = 255;
        // Bottom-left, bottom-right, top-right, top-left as the camera sees it.
        const std::array<Vec3f, 4> corners{centre - right - up, centre + right - up,
                                           centre + right + up, centre - right + up};
        const std::array<f32, 4> u{p.uv[0], p.uv[2], p.uv[2], p.uv[0]};
        const std::array<f32, 4> v{p.uv[3], p.uv[3], p.uv[1], p.uv[1]};
        for (usize k = 0; k < 4; ++k) {
            render::EntityVertex vertex;
            vertex.x      = corners[k].x;
            vertex.y      = corners[k].y;
            vertex.z      = corners[k].z;
            vertex.u      = u[k];
            vertex.v      = v[k];
            vertex.colour = colour;
            out.push_back(vertex);
        }
    }
}

}  // namespace ov::client
