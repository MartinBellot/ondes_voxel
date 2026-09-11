// ── mobs-2 ── Drowned conversion. See the header.
#include "ov/gameplay/conversion.hpp"

namespace ov::gameplay {

DrowningEvent drowning_tick(DrowningState& state, bool eyes_in_water) noexcept {
    if (state.converting >= 0) {
        --state.converting;
        return state.converting < 0 ? DrowningEvent::Converted : DrowningEvent::None;
    }
    if (!eyes_in_water) {
        state.in_water = -1;
        return DrowningEvent::None;
    }
    ++state.in_water;
    if (state.in_water >= kDrownStartTicks) {
        state.converting = kDrownConvertTicks;
        return DrowningEvent::Started;
    }
    return DrowningEvent::None;
}

}  // namespace ov::gameplay
