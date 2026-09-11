// The ender dragon's rules. See dragon.hpp for the sources and for what is
// fitted rather than documented.
#include "ov/gameplay/dragon.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ov::gameplay {

namespace {

constexpr f64 kPi = std::numbers::pi;

/// Squared distances the phases test a target against: a target nearer than 10
/// or further than 150 is replaced.
constexpr f64 kNearTarget = 10.0 * 10.0;
constexpr f64 kFarTarget  = 150.0 * 150.0;

[[nodiscard]] f32 wrap_degrees(f32 angle) noexcept {
    angle = std::fmod(angle, 360.0F);
    if (angle >= 180.0F) {
        angle -= 360.0F;
    }
    if (angle < -180.0F) {
        angle += 360.0F;
    }
    return angle;
}

/// The heading as a horizontal unit vector: yaw 0 looks south (+z), yaw 90
/// west (−x) — the game's convention for every entity.
[[nodiscard]] Vec3d forward_of(f32 yaw) noexcept {
    const f64 radians = static_cast<f64>(yaw) * kPi / 180.0;
    return Vec3d{-std::sin(radians), 0.0, std::cos(radians)};
}

[[nodiscard]] f64 horizontal_distance(Vec3d a, Vec3d b) noexcept {
    const f64 dx = a.x - b.x;
    const f64 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

/// One ring's angular position of node `k` of `n`, in degrees.
[[nodiscard]] f64 ring_angle(usize k, usize n) noexcept {
    return 360.0 * static_cast<f64>(k) / static_cast<f64>(n);
}

[[nodiscard]] f64 angle_gap(f64 a, f64 b) noexcept {
    f64 gap = std::fmod(std::abs(a - b), 360.0);
    return gap > 180.0 ? 360.0 - gap : gap;
}

}  // namespace

std::string_view dragon_phase_name(DragonPhase phase) noexcept {
    switch (phase) {
        case DragonPhase::HoldingPattern: return "holding_pattern";
        case DragonPhase::StrafePlayer: return "strafe_player";
        case DragonPhase::LandingApproach: return "landing_approach";
        case DragonPhase::Landing: return "landing";
        case DragonPhase::Takeoff: return "takeoff";
        case DragonPhase::SittingFlaming: return "sitting_flaming";
        case DragonPhase::SittingScanning: return "sitting_scanning";
        case DragonPhase::SittingAttacking: return "sitting_attacking";
        case DragonPhase::ChargingPlayer: return "charging_player";
        case DragonPhase::Dying: return "dying";
        case DragonPhase::Hover: return "hover";
    }
    return "unknown";
}

bool dragon_sitting(DragonPhase phase) noexcept {
    return phase == DragonPhase::SittingFlaming || phase == DragonPhase::SittingScanning ||
           phase == DragonPhase::SittingAttacking;
}

// ── The graph ───────────────────────────────────────────────────────────────

DragonGraph::DragonGraph() noexcept {
    // Rings of 12, 8 and 4 nodes, radii 60, 40 and 20, each ring starting on
    // +x and turning towards +z (the documented numbering: node 0 at
    // (60, 0), node 3 at (0, 60), node 12 at (40, 0), node 20 at (20, 0)).
    struct Ring {
        usize first;
        usize count;
        f64   radius;
    };
    constexpr std::array<Ring, 3> kRings{Ring{0, 12, 60.0}, Ring{12, 8, 40.0}, Ring{20, 4, 20.0}};
    for (const Ring& ring : kRings) {
        for (usize k = 0; k < ring.count; ++k) {
            // In float, as the game's positions are: 60 × sin 30° is 30 in
            // float and 29.999… in double. Which of the two the game floors is
            // not documented; the documented picture cannot tell one block.
            const f64 radians = ring_angle(k, ring.count) * kPi / 180.0;
            const f32 radius  = static_cast<f32>(ring.radius);
            const f32 x       = radius * static_cast<f32>(std::cos(radians));
            const f32 z       = radius * static_cast<f32>(std::sin(radians));
            nodes_[ring.first + k] = BlockPos{static_cast<i32>(std::floor(x)), 73,
                                              static_cast<i32>(std::floor(z))};
        }
    }

    // The links: not documented; rebuilt from geometry (see the header). Each
    // node leads to its two ring neighbours; each outer node to the nearest
    // middle node(s), each middle node to the nearest inner node(s), both ways.
    const auto link = [this](usize a, usize b) {
        links_[a] |= 1U << b;
        links_[b] |= 1U << a;
    };
    for (const Ring& ring : kRings) {
        for (usize k = 0; k < ring.count; ++k) {
            link(ring.first + k, ring.first + (k + 1) % ring.count);
        }
    }
    const auto join = [&](const Ring& outer, const Ring& inner) {
        for (usize k = 0; k < outer.count; ++k) {
            const f64 angle = ring_angle(k, outer.count);
            f64       best  = 360.0;
            for (usize j = 0; j < inner.count; ++j) {
                best = std::min(best, angle_gap(angle, ring_angle(j, inner.count)));
            }
            for (usize j = 0; j < inner.count; ++j) {
                if (angle_gap(angle, ring_angle(j, inner.count)) <= best + 1e-6) {
                    link(outer.first + k, inner.first + j);
                }
            }
        }
    };
    join(kRings[0], kRings[1]);
    join(kRings[1], kRings[2]);
}

void DragonGraph::place(const std::function<i32(i32 x, i32 z)>& top) {
    for (usize i = 0; i < kDragonNodeCount; ++i) {
        // Above the highest motion-blocking block: 15 for the middle ring, 5
        // for the others, never under 73.
        const i32 highest = top(nodes_[i].x, nodes_[i].z) - 1;
        const i32 above   = (i >= 12 && i < 20) ? 15 : 5;
        nodes_[i].y       = std::max(73, highest + above);
    }
    placed_ = true;
}

usize DragonGraph::closest(Vec3d at, i32 crystals) const noexcept {
    const f64 x    = std::floor(at.x);
    const f64 y    = std::floor(at.y);
    const f64 z    = std::floor(at.z);
    f64       best = 100.0 * 100.0;
    usize     out  = 0;
    for (usize i = 0; i < kDragonNodeCount; ++i) {
        if (!enabled(i, crystals)) {
            continue;
        }
        const f64 dx = static_cast<f64>(nodes_[i].x) - x;
        const f64 dy = static_cast<f64>(nodes_[i].y) - y;
        const f64 dz = static_cast<f64>(nodes_[i].z) - z;
        const f64 d  = dx * dx + dy * dy + dz * dz;
        if (d < best) {
            best = d;
            out  = i;
        }
    }
    return out;
}

usize DragonGraph::closest_enabled(usize index, i32 crystals) const noexcept {
    if (enabled(index, crystals)) {
        return index;
    }
    const BlockPos p = nodes_[index];
    return closest(Vec3d{static_cast<f64>(p.x), static_cast<f64>(p.y), static_cast<f64>(p.z)},
                   crystals);
}

usize DragonGraph::find_path(usize from, usize to, i32 crystals,
                             std::span<u8> out) const noexcept {
    if (from >= kDragonNodeCount || to >= kDragonNodeCount || out.empty()) {
        return 0;
    }
    usize goal = to;
    if (!enabled(to, crystals)) {
        goal = closest_enabled(to, crystals);
        if (goal == from) {
            return 0;
        }
    }
    if (goal == from) {
        out[0] = static_cast<u8>(from);
        return 1;
    }
    // Dijkstra over at most 24 nodes: the weights are the distances between
    // node centres.
    std::array<f64, kDragonNodeCount>  cost{};
    std::array<i32, kDragonNodeCount>  previous{};
    std::array<bool, kDragonNodeCount> done{};
    cost.fill(std::numeric_limits<f64>::infinity());
    previous.fill(-1);
    cost[from] = 0.0;
    for (usize round = 0; round < kDragonNodeCount; ++round) {
        usize current = kDragonNodeCount;
        f64   least   = std::numeric_limits<f64>::infinity();
        for (usize i = 0; i < kDragonNodeCount; ++i) {
            if (!done[i] && enabled(i, crystals) && cost[i] < least) {
                least   = cost[i];
                current = i;
            }
        }
        if (current == kDragonNodeCount || current == goal) {
            break;
        }
        done[current] = true;
        for (usize next = 0; next < kDragonNodeCount; ++next) {
            if ((links_[current] & (1U << next)) == 0 || !enabled(next, crystals) || done[next]) {
                continue;
            }
            const f64 dx = static_cast<f64>(nodes_[next].x - nodes_[current].x);
            const f64 dy = static_cast<f64>(nodes_[next].y - nodes_[current].y);
            const f64 dz = static_cast<f64>(nodes_[next].z - nodes_[current].z);
            const f64 c  = cost[current] + std::sqrt(dx * dx + dy * dy + dz * dz);
            if (c < cost[next]) {
                cost[next]     = c;
                previous[next] = static_cast<i32>(current);
            }
        }
    }
    if (previous[goal] < 0) {
        return 0;
    }
    std::array<u8, kDragonNodeCount> reversed{};
    usize                            length = 0;
    for (i32 at = static_cast<i32>(goal); at >= 0 && length < kDragonNodeCount;
         at = previous[static_cast<usize>(at)]) {
        reversed[length++] = static_cast<u8>(at);
        if (static_cast<usize>(at) == from) {
            break;
        }
    }
    const usize written = std::min(length, out.size());
    for (usize i = 0; i < written; ++i) {
        out[i] = reversed[length - 1 - i];
    }
    return written;
}

// ── The dragon ──────────────────────────────────────────────────────────────

Dragon::Dragon(Vec3d spawn, f32 yaw, f32 health, bool previously_killed,
               DragonFlight flight) noexcept
    : flight_(flight),
      position_(spawn),
      yaw_(yaw),
      health_(health),
      previously_killed_(previously_killed) {}

void Dragon::set_phase(DragonPhase phase) noexcept {
    if (phase == phase_ && !phase_entered_) {
        return;
    }
    if (phase_ == DragonPhase::SittingFlaming && phase != DragonPhase::SittingFlaming) {
        breath_pending_ = true;
    }
    phase_         = phase;
    phase_ticks_   = 0;
    phase_entered_ = false;
    phase_dirty_   = true;
    target_.reset();
    path_length_ = 0;
    path_index_  = 0;
    switch (phase) {
        case DragonPhase::StrafePlayer:
            fireball_charge_ = 0;
            break;
        case DragonPhase::SittingFlaming:
            ++flame_count_;
            break;
        case DragonPhase::ChargingPlayer:
            time_since_charge_ = 0;
            break;
        default:
            break;
    }
}

bool Dragon::needs_target() const noexcept {
    if (!target_) {
        return true;
    }
    const f64 d = (*target_ - position_).length_squared();
    return d < kNearTarget || d > kFarTarget;
}

Vec3d Dragon::fountain(const DragonSurroundings& world) const noexcept {
    return Vec3d{0.5, static_cast<f64>(world.fountain_top), 0.5};
}

const DragonPlayer* Dragon::player(const DragonSurroundings& world, i32 id) const {
    for (const DragonPlayer& p : world.players) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

f32 Dragon::bearing_to(Vec3d point) const noexcept {
    const f64 dx = point.x - position_.x;
    const f64 dz = point.z - position_.z;
    return static_cast<f32>(-std::atan2(dx, dz) * 180.0 / kPi);
}

AABB Dragon::box() const noexcept {
    return AABB::from_entity(position_, 16.0, 8.0);
}

std::array<AABB, kDragonPartCount> Dragon::parts() const noexcept {
    // Laid out from the documented overall size, not measured (see the
    // header): the head ahead, the tail behind, the wings to either side.
    const Vec3d f = forward_of(yaw_);
    const Vec3d r{f.z, 0.0, -f.x};
    const auto  at = [&](f64 ahead, f64 side, f64 up) {
        return position_ + f * ahead + r * side + Vec3d{0.0, up, 0.0};
    };
    return {
        AABB::from_entity(at(6.5, 0.0, 1.5), 1.0, 1.0),   // head
        AABB::from_entity(at(4.5, 0.0, 1.0), 3.0, 3.0),   // neck
        AABB::from_entity(at(0.5, 0.0, 0.0), 5.0, 3.0),   // body
        AABB::from_entity(at(-3.5, 0.0, 0.5), 2.0, 2.0),  // tail
        AABB::from_entity(at(-5.5, 0.0, 0.5), 2.0, 2.0),
        AABB::from_entity(at(-7.5, 0.0, 0.5), 2.0, 2.0),
        AABB::from_entity(at(0.0, 4.5, 2.0), 4.0, 2.0),   // wings
        AABB::from_entity(at(0.0, -4.5, 2.0), 4.0, 2.0),
    };
}

Vec3d Dragon::head_centre() const noexcept {
    return parts()[static_cast<usize>(DragonPart::Head)].centre();
}

void Dragon::next_waypoint(math::LegacyRandomSource& random) {
    if (path_done()) {
        return;
    }
    const BlockPos node = graph_.node(path_[path_index_]);
    ++path_index_;
    const f32 lift = random.next_float() * 20.0F;
    target_        = Vec3d{static_cast<f64>(node.x), static_cast<f64>(node.y) + static_cast<f64>(lift),
                    static_cast<f64>(node.z)};
}

void Dragon::circle_path(const DragonSurroundings& world, math::LegacyRandomSource& random,
                         bool map_inner) {
    bool&     direction = map_inner ? strafe_clockwise_ : clockwise_;
    const i32 crystals  = world.crystals;
    const i32 first     = static_cast<i32>(graph_.closest(position_, crystals));
    i32       last      = first;
    if (random.next_int(8) == 0) {
        direction = !direction;
        last += 6;
    }
    last += direction ? 1 : -1;
    if (!map_inner || crystals > 0) {
        last %= 12;
        if (last < 0) {
            last += 12;
        }
    } else {
        last = ((last - 12) & 7) + 12;
    }
    path_length_ = graph_.find_path(static_cast<usize>(first), static_cast<usize>(last), crystals,
                                    path_);
    path_index_  = path_length_ > 0 ? 1 : 0;
}

void Dragon::tick(const DragonSurroundings& world, math::LegacyRandomSource& random,
                  DragonOutput& out) {
    ++since_hurt_;
    if (invulnerable_ > 0) {
        --invulnerable_;
    }

    if (health_ <= 0.0F) {
        // The death animation: rising, the experience from tick 155, gone at
        // 200 (docs/provenance/dragon.md § 5).
        ++death_time_;
        const i32 total = experience();
        if (death_time_ > 150 && death_time_ % 5 == 0) {
            out.experience += static_cast<i32>(std::floor(static_cast<f32>(total) * 0.08F));
        }
        position_.y += 0.1;
        velocity_ = Vec3d{};
        if (death_time_ == kDragonDeathAnimation) {
            out.experience_last = static_cast<i32>(std::floor(static_cast<f32>(total) * 0.2F));
            out.dead            = true;
        }
        return;
    }

    tick_phase(world, random, out);
    if (health_ <= 0.0F) {
        return;
    }
    if (dragon_sitting(phase_)) {
        velocity_ = Vec3d{};
    } else if (phase_ != DragonPhase::Hover) {
        fly(world);
    }
    if (phase_dirty_) {
        out.phase_changed = true;
        phase_dirty_      = false;
    }
    if (breath_pending_) {
        out.breath_stop = true;
        breath_pending_ = false;
    }
}

void Dragon::tick_phase(const DragonSurroundings& world, math::LegacyRandomSource& random,
                        DragonOutput& out) {
    ++phase_ticks_;
    switch (phase_) {
        case DragonPhase::HoldingPattern:
            holding_pattern(world, random);
            break;
        case DragonPhase::StrafePlayer:
            strafe(world, random, out);
            break;
        case DragonPhase::LandingApproach:
            landing_approach(world, random);
            break;
        case DragonPhase::Landing:
            if (!target_) {
                target_ = fountain(world);
            }
            if ((*target_ - position_).length_squared() < 1.0) {
                flame_count_ = 0;
                position_    = *target_;
                set_phase(DragonPhase::SittingScanning);
            }
            break;
        case DragonPhase::SittingScanning:
            sitting_scanning(world);
            break;
        case DragonPhase::SittingAttacking:
            if (phase_ticks_ == 1) {
                out.roar = true;
            }
            if (phase_ticks_ >= 40) {
                set_phase(DragonPhase::SittingFlaming);
            }
            break;
        case DragonPhase::SittingFlaming:
            if (phase_ticks_ == 10 && !world.peaceful) {
                const Vec3d ahead   = forward_of(yaw_) * 2.5;
                const Vec3d head    = head_centre();
                out.breath_start    = true;
                out.breath_from     = Vec3d{head.x + ahead.x, head.y, head.z + ahead.z};
            }
            if (phase_ticks_ >= 200) {
                set_phase(flame_count_ >= 4 ? DragonPhase::Takeoff : DragonPhase::SittingScanning);
            }
            break;
        case DragonPhase::ChargingPlayer:
            charging();
            break;
        case DragonPhase::Takeoff:
            takeoff(world, random);
            break;
        case DragonPhase::Dying:
            dying(world, out);
            break;
        case DragonPhase::Hover:
            break;
    }
}

void Dragon::holding_pattern(const DragonSurroundings& world, math::LegacyRandomSource& random) {
    if (!needs_target()) {
        return;
    }
    const i32 crystals = world.crystals;
    if (path_length_ > 0 && path_done()) {
        if (random.next_int(crystals + 3) == 0) {
            set_phase(DragonPhase::LandingApproach);
            return;
        }
        // The strafe: likelier the closer a player is to the fountain and the
        // fewer crystals are left — one draw on each.
        const Vec3d          top{0.0, static_cast<f64>(world.fountain_top - 1), 0.0};
        const DragonPlayer*  nearest = nullptr;
        f64                  best    = std::numeric_limits<f64>::infinity();
        for (const DragonPlayer& p : world.players) {
            const f64 d = (p.feet - top).length_squared();
            if (d < best) {
                best    = d;
                nearest = &p;
            }
        }
        if (nearest != nullptr) {
            const i32 far = static_cast<i32>(std::floor(best / 512.0));
            if (random.next_int(std::max(far + 2, 1)) == 0 ||
                random.next_int(crystals + 2) == 0) {
                attack_target_ = nearest->id;
                set_phase(DragonPhase::StrafePlayer);
                return;
            }
        }
    }
    if (path_length_ == 0 || path_done()) {
        circle_path(world, random, false);
        if (path_length_ == 0) {
            return;
        }
    }
    next_waypoint(random);
}

void Dragon::strafe(const DragonSurroundings& world, math::LegacyRandomSource& random,
                    DragonOutput& out) {
    const DragonPlayer* victim = attack_target_ ? player(world, *attack_target_) : nullptr;
    if (victim == nullptr) {
        set_phase(DragonPhase::HoldingPattern);
        return;
    }
    const i32 crystals = world.crystals;
    if (phase_ticks_ == 1) {
        // A path towards the node nearest the player.
        const usize first = graph_.closest(position_, crystals);
        const usize last  = graph_.closest(victim->feet, crystals);
        path_length_      = graph_.find_path(first, last, crystals, path_);
        path_index_       = path_length_ > 0 ? 1 : 0;
        next_waypoint(random);
    }
    const f64 across = horizontal_distance(position_, victim->feet);
    const f64 lift   = std::min(across / 80.0 - 0.6, 10.0);
    if (path_length_ == 0 || path_done()) {
        target_      = victim->feet + Vec3d{0.0, lift, 0.0};
        path_length_ = 0;
        path_index_  = 0;
    }
    if (needs_target()) {
        if (path_length_ == 0) {
            circle_path(world, random, true);
        }
        if (path_length_ > 0 && !path_done()) {
            next_waypoint(random);
        }
    }

    const Vec3d aim = victim->feet + Vec3d{0.0, victim->eye_height * 0.5, 0.0};
    const Vec3d eye = victim->feet + Vec3d{0.0, victim->eye_height, 0.0};
    if ((aim - position_).length_squared() > 64.0 * 64.0 ||
        (world.sees && !world.sees(head_centre(), eye))) {
        fireball_charge_ = std::max(0, fireball_charge_ - 1);
        return;
    }
    ++fireball_charge_;
    if (fireball_charge_ < 5) {
        return;
    }
    if (std::abs(wrap_degrees(bearing_to(victim->feet) - yaw_)) > 9.5F) {
        return;
    }
    const Vec3d from       = head_centre();
    const Vec3d direction  = (aim - from).normalized();
    out.fireball           = true;
    out.fireball_from      = from;
    out.fireball_direction = direction;
    set_phase(DragonPhase::HoldingPattern);
}

void Dragon::landing_approach(const DragonSurroundings& world, math::LegacyRandomSource& random) {
    if (!needs_target()) {
        return;
    }
    const i32 crystals = world.crystals;
    if (path_length_ == 0) {
        const Vec3d         top = fountain(world);
        const DragonPlayer* nearest = nullptr;
        f64                 best    = std::numeric_limits<f64>::infinity();
        for (const DragonPlayer& p : world.players) {
            const f64 d = (p.feet - top).length_squared();
            if (d < best) {
                best    = d;
                nearest = &p;
            }
        }
        const usize start = graph_.closest(position_, crystals);
        usize       last  = 0;
        if (nearest != nullptr) {
            const f64 magnitude =
                std::sqrt(nearest->feet.x * nearest->feet.x + nearest->feet.z * nearest->feet.z);
            const f64 nx = magnitude < 1.0e-4 ? 0.0 : nearest->feet.x / magnitude;
            const f64 nz = magnitude < 1.0e-4 ? 0.0 : nearest->feet.z / magnitude;
            last         = graph_.closest(Vec3d{-nx * 40.0, 105.0, -nz * 40.0}, crystals);
        } else {
            last = graph_.closest(Vec3d{40.0, top.y, 0.0}, crystals);
        }
        path_length_ = graph_.find_path(start, last, crystals, path_);
        path_index_  = path_length_ > 0 ? 1 : 0;
        if (path_length_ == 0) {
            return;
        }
    }
    if (path_done()) {
        set_phase(DragonPhase::Landing);
        return;
    }
    next_waypoint(random);
}

f32 Dragon::wire_yaw() const noexcept {
    return wrap_degrees(yaw_ + 180.0F);
}

void Dragon::sitting_scanning(const DragonSurroundings& world) {
    if (world.peaceful) {
        set_phase(DragonPhase::Takeoff);
        return;
    }
    const Vec3d head = head_centre();
    const auto  seen = [&](const DragonPlayer& p) {
        return !world.sees || world.sees(head, p.feet + Vec3d{0.0, p.eye_height, 0.0});
    };
    const DragonPlayer* near = nullptr;
    f64                 best = std::numeric_limits<f64>::infinity();
    for (const DragonPlayer& p : world.players) {
        const Vec3d d = p.feet - position_;
        if (std::abs(d.x) > 20.0 || std::abs(d.z) > 20.0 || std::abs(d.y) > 10.0 || !seen(p)) {
            continue;
        }
        if (d.length_squared() < best) {
            best = d.length_squared();
            near = &p;
        }
    }
    if (near != nullptr) {
        if (phase_ticks_ > 25) {
            set_phase(DragonPhase::SittingAttacking);
            return;
        }
        const f32 gap = wrap_degrees(bearing_to(near->feet) - yaw_);
        if (std::abs(gap) > 9.5F) {
            yaw_ = wrap_degrees(yaw_ + std::clamp(gap, -flight_.turn_landing, flight_.turn_landing));
        }
        return;
    }
    if (phase_ticks_ < 100) {
        return;
    }
    const DragonPlayer* charge = nullptr;
    best                       = 150.0 * 150.0;
    for (const DragonPlayer& p : world.players) {
        const f64 d = (p.feet - position_).length_squared();
        if (d < best && seen(p)) {
            best   = d;
            charge = &p;
        }
    }
    if (charge == nullptr) {
        set_phase(DragonPhase::Takeoff);
        return;
    }
    set_phase(DragonPhase::ChargingPlayer);
    target_ = charge->feet;
}

void Dragon::charging() {
    if (time_since_charge_ > 0) {
        ++time_since_charge_;
    }
    if (time_since_charge_ > 10) {
        set_phase(DragonPhase::HoldingPattern);
        return;
    }
    if (needs_target()) {
        ++time_since_charge_;
    }
}

void Dragon::takeoff(const DragonSurroundings& world, math::LegacyRandomSource& random) {
    const i32   crystals = world.crystals;
    const Vec3d top      = fountain(world);
    if (phase_ticks_ == 1) {
        // Away from the fountain, the way the head points, climbing.
        const f64   distance = (position_ - top).length();
        const f64   pitch    = -45.0 / std::max(distance / 4.0, 1.0);
        const Vec3d ahead    = forward_of(yaw_) * std::cos(pitch * kPi / 180.0);
        const usize first    = graph_.closest(position_, crystals);
        i32 last = static_cast<i32>(graph_.closest(Vec3d{ahead.x * 40.0, 105.0, ahead.z * 40.0},
                                                   crystals));
        if (crystals > 0) {
            last %= 12;
        } else {
            last = ((last - 12) & 7) + 12;
        }
        path_length_ =
            graph_.find_path(first, static_cast<usize>(last), crystals, path_);
        if (path_length_ >= 2) {
            path_index_ = 1;
        } else {
            path_index_ = 0;
        }
        next_waypoint(random);
        return;
    }
    if (path_length_ > 0 && (position_ - top).length_squared() >= 100.0) {
        set_phase(DragonPhase::HoldingPattern);
    }
}

void Dragon::dying(const DragonSurroundings& world, DragonOutput& /*out*/) {
    if (!target_) {
        target_ = fountain(world);
    }
    const f64 d = (*target_ - position_).length_squared();
    if (d >= kNearTarget && d <= kFarTarget) {
        health_ = 1.0F;
    } else {
        health_ = 0.0F;
    }
}

void Dragon::fly(const DragonSurroundings& /*world*/) {
    if (!target_) {
        velocity_ = velocity_ * flight_.drag;
        position_ += velocity_;
        return;
    }
    const Vec3d to         = *target_ - position_;
    const f64   horizontal = std::sqrt(to.x * to.x + to.z * to.z);

    // Vertical: towards the target's height, the change per tick capped by the
    // phase (documented for the landing and the charge, fitted otherwise).
    f64 cap = flight_.climb;
    if (phase_ == DragonPhase::Landing) {
        cap = flight_.climb_landing;
    } else if (phase_ == DragonPhase::ChargingPlayer || phase_ == DragonPhase::Dying) {
        cap = flight_.climb_fast;
    }
    const f64 climb = std::clamp(to.y / std::max(horizontal, 1.0), -cap, cap);
    velocity_.y     = velocity_.y * flight_.vertical_drag + climb;

    // Heading: towards the target, at most `turn` degrees a tick.
    const f32 wanted = bearing_to(*target_);
    const f32 gap    = wrap_degrees(wanted - yaw_);
    const f32 step   = phase_ == DragonPhase::Landing
                           ? std::clamp(gap, -flight_.turn_landing, flight_.turn_landing)
                           : std::clamp(gap * flight_.turn_gain, -flight_.turn, flight_.turn);
    yaw_ = wrap_degrees(yaw_ + step);

    // Forward: thrust along the heading, drag on what it had.
    const Vec3d ahead = forward_of(yaw_);
    f64         push  = flight_.thrust;
    if (flight_.turn_slowdown > 0.0) {
        const f64 left = static_cast<f64>(wrap_degrees(wanted - yaw_)) * kPi / 180.0;
        push *= std::max(0.1, 1.0 - flight_.turn_slowdown * (1.0 - std::cos(left)));
    }
    if (phase_ == DragonPhase::Landing) {
        // Slowing into the fountain: measured landing horizontal speed p10
        // 0.11, p50 0.44 — it spirals in, it does not overshoot.
        push *= std::clamp(horizontal / 16.0, 0.05, 1.0);
    }
    if (in_wall_) {
        push *= 0.8;
    }
    velocity_.x = velocity_.x * flight_.drag + ahead.x * push;
    velocity_.z = velocity_.z * flight_.drag + ahead.z * push;
    position_ += velocity_;
}

f32 Dragon::hurt(DragonPart part, f32 damage, DragonHurtSource source) noexcept {
    if (health_ <= 0.0F || phase_ == DragonPhase::Dying) {
        return 0.0F;
    }
    if (source == DragonHurtSource::Other) {
        return 0.0F;
    }
    const bool sitting = dragon_sitting(phase_);
    if (sitting && source == DragonHurtSource::PlayerProjectile) {
        return 0.0F;  // bounces off, alight
    }
    if (part != DragonPart::Head) {
        damage = damage / 4.0F + std::min(damage, 1.0F);
    }
    if (damage < 0.01F) {
        return 0.0F;
    }
    // A living entity's hurt cooldown: for ten ticks after a hit, only what
    // exceeds that hit lands.
    f32 applied = damage;
    if (invulnerable_ > 10) {
        if (damage <= last_hurt_) {
            return 0.0F;
        }
        applied = damage - last_hurt_;
    } else {
        invulnerable_ = 20;
    }
    last_hurt_  = damage;
    health_     = health_ - applied;
    since_hurt_ = 0;

    if (sitting) {
        sitting_damage_ += std::floor(applied);
        if (sitting_damage_ > kDragonSittingDamageLimit) {
            sitting_damage_ = 0.0F;
            if (health_ > 0.0F) {
                set_phase(DragonPhase::Takeoff);
            }
        }
        if (health_ <= 0.0F) {
            health_ = 0.0F;  // killed on its perch: the animation plays there
        }
        return applied;
    }
    if (health_ <= 0.0F) {
        health_ = 1.0F;
        set_phase(DragonPhase::Dying);
        return applied;
    }
    if (phase_ == DragonPhase::HoldingPattern || phase_ == DragonPhase::StrafePlayer) {
        // A point just behind itself: the next tick turns it away onto a new
        // path.
        target_      = position_ - forward_of(yaw_);
        path_length_ = 0;
        path_index_  = 0;
    }
    return applied;
}

void Dragon::crystal_destroyed(std::optional<i32> player) noexcept {
    if (phase_ == DragonPhase::HoldingPattern && player) {
        attack_target_ = player;
        set_phase(DragonPhase::StrafePlayer);
    }
}

void Dragon::lose_crystal() noexcept {
    (void)hurt(DragonPart::Head, kDragonCrystalLoss, DragonHurtSource::Explosion);
}

void Dragon::heal(f32 amount) noexcept {
    if (health_ > 0.0F && phase_ != DragonPhase::Dying) {
        health_ = std::min(200.0F, health_ + amount);
    }
}

}  // namespace ov::gameplay
