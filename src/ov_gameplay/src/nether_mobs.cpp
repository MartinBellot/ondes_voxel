// ── nether-2 ── The Nether's mobs: bartering, volleys, charges, fireballs,
// the ghast's flight. See the header for the sources.
#include "ov/gameplay/nether_mobs.hpp"

#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/mob_logic.hpp"

#include <algorithm>
#include <cmath>

namespace ov::gameplay {

// ── Bartering ───────────────────────────────────────────────────────────────

BarterTable::BarterTable(std::vector<BarterEntry> entries) : entries_(std::move(entries)) {
    for (const BarterEntry& entry : entries_) {
        total_ += std::max(0, entry.weight);
    }
}

BarterDrop BarterTable::draw(math::LegacyRandomSource& random) const {
    BarterDrop drop;
    if (entries_.empty() || total_ <= 0) {
        return drop;
    }
    i32                roll   = random.next_int(total_);
    const BarterEntry* chosen = &entries_.front();
    for (const BarterEntry& entry : entries_) {
        roll -= std::max(0, entry.weight);
        if (roll < 0) {
            chosen = &entry;
            break;
        }
    }
    drop.item   = chosen->item;
    drop.potion = chosen->potion;
    drop.count  = chosen->min_count >= chosen->max_count
                      ? chosen->min_count
                      : random.next_int(chosen->max_count - chosen->min_count + 1) +
                            chosen->min_count;
    if (chosen->soul_speed) {
        // One enchantment in the list (a draw over one), then its level 1..3.
        (void)random.next_int(1);
        drop.soul_speed_level = random.next_int(3) + 1;
    }
    return drop;
}

// ── The blaze's volley ──────────────────────────────────────────────────────

bool BlazeVolley::tick(bool engaged) noexcept {
    if (!engaged) {
        // Out of reach or sight: the clock winds down and the flame goes out.
        if (time > 0) {
            --time;
        }
        if (step != 0 || charged) {
            step    = 0;
            charged = false;
        }
        return false;
    }
    --time;
    if (time > 0) {
        return false;
    }
    ++step;
    if (step == 1) {
        time    = kBlazeChargeTicks;
        charged = true;
        return false;
    }
    if (step <= 1 + kBlazeShotsPerVolley) {
        time = kBlazeShotSpacing;
        return true;
    }
    time    = kBlazeRestTicks;
    step    = 0;
    charged = false;
    return false;
}

// ── The ghast's charge ──────────────────────────────────────────────────────

bool GhastCharge::tick(bool engaged) noexcept {
    if (!engaged) {
        if (charge > 0) {
            --charge;
        }
        return false;
    }
    ++charge;
    if (charge == 20) {
        charge = -40;
        return true;
    }
    return false;
}

// ── Fireballs ───────────────────────────────────────────────────────────────

Vec3d fireball_power(Vec3d direction) noexcept {
    const f64 length =
        std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (length == 0.0) {
        return Vec3d{};
    }
    return Vec3d{direction.x / length * 0.1, direction.y / length * 0.1,
                 direction.z / length * 0.1};
}

void step_fireball(Vec3d& position, Vec3d& velocity, Vec3d power, bool in_water) noexcept {
    position.x += velocity.x;
    position.y += velocity.y;
    position.z += velocity.z;
    const f64 inertia = in_water ? static_cast<f64>(0.8F) : static_cast<f64>(0.95F);
    velocity.x        = (velocity.x + power.x) * inertia;
    velocity.y        = (velocity.y + power.y) * inertia;
    velocity.z        = (velocity.z + power.z) * inertia;
}

// ── The ghast's flight ──────────────────────────────────────────────────────

void GhastFlight::tick(entity::EntityWorld& world, entity::EntityHandle self,
                       const entity::TickContext& context) {
    entity::EntityState* state = world.mutable_state(self);
    if (state == nullptr) {
        return;
    }
    const MobContext* mob = mob_context(context);

    // A destination when it has none, has reached it, or has drifted far.
    const f64 dx = wanted_.x - state->position.x;
    const f64 dy = wanted_.y - state->position.y;
    const f64 dz = wanted_.z - state->position.z;
    const f64 d2 = dx * dx + dy * dy + dz * dz;
    if (!has_wanted_ || d2 < 1.0 || d2 > 3600.0) {
        const f32 rx = random_.next_float();
        const f32 ry = random_.next_float();
        const f32 rz = random_.next_float();
        wanted_      = Vec3d{state->position.x + static_cast<f64>((rx * 2.0F - 1.0F) * 16.0F),
                        state->position.y + static_cast<f64>((ry * 2.0F - 1.0F) * 16.0F),
                        state->position.z + static_cast<f64>((rz * 2.0F - 1.0F) * 16.0F)};
        has_wanted_ = true;
    }

    // A push every few ticks, if the destination is somewhere a ghast fits.
    if (float_duration_-- <= 0) {
        float_duration_ += random_.next_int(5) + 2;
        const f64 ex = wanted_.x - state->position.x;
        const f64 ey = wanted_.y - state->position.y;
        const f64 ez = wanted_.z - state->position.z;
        const f64 d  = std::sqrt(ex * ex + ey * ey + ez * ez);
        bool      free = true;
        if (mob != nullptr && mob->world != nullptr) {
            entity::EntityState probe = *state;
            probe.position            = wanted_;
            free = !mob->world->overlaps(entity_box(probe));
        }
        if (d > 0.0 && free) {
            state->velocity.x += ex / d * 0.1;
            state->velocity.y += ey / d * 0.1;
            state->velocity.z += ez / d * 0.1;
        } else {
            has_wanted_ = false;
        }
    }

    // The body faces where it is going, or its target.
    const Vec3d facing = has_look_ ? look_ : wanted_;
    const f64   fx     = facing.x - state->position.x;
    const f64   fz     = facing.z - state->position.z;
    if (fx != 0.0 || fz != 0.0) {
        const f32 yaw  = static_cast<f32>(-std::atan2(fx, fz) * 180.0 / 3.141592653589793);
        state->yaw     = yaw;
        state->head_yaw = yaw;
    }
    has_look_ = false;

    // No gravity; the air's 0.91 on every axis.
    if (mob != nullptr && mob->world != nullptr) {
        EntityMotionConstants flying;
        flying.gravity       = 0.0;
        flying.vertical_drag = 0.91;
        flying.air_drag      = 0.91;
        const entity::EntityState moved = step_entity(*state, flying, *mob->world);
        state->position                 = moved.position;
        state->velocity                 = moved.velocity;
        state->on_ground                = moved.on_ground;
    }
}

}  // namespace ov::gameplay
