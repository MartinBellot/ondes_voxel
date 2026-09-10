// ── mobs-2 ── A slime's size and its division. See the header for sources.
#include "ov/gameplay/slime.hpp"

namespace ov::gameplay {

i32 draw_slime_size(math::LegacyRandomSource& random, f32 special_multiplier) {
    i32 exponent = random.next_int(3);
    if (exponent < 2) {
        // Drawn only when it could matter, as documented: a size 4 does not
        // spend a second draw.
        const f32 chance = random.next_float();
        if (chance < 0.5F * special_multiplier) {
            ++exponent;
        }
    }
    return 1 << exponent;
}

void slime_children(i32 size, math::LegacyRandomSource& random, std::vector<SlimeChild>& out) {
    out.clear();
    if (size <= 1) {
        return;
    }
    const i32 half  = size / 2;
    const i32 count = 2 + random.next_int(3);
    const f64 step  = static_cast<f64>(size) / 4.0;
    for (i32 k = 0; k < count; ++k) {
        SlimeChild child;
        child.offset = Vec3d{(static_cast<f64>(k % 2) - 0.5) * step, 0.5,
                             (static_cast<f64>(k / 2) - 0.5) * step};
        child.size   = half;
        out.push_back(child);
    }
}

}  // namespace ov::gameplay
