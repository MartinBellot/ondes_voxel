#include "ov/gameplay/dig_controller.hpp"

#include <cmath>

namespace ov::gameplay {

i32 DigController::stage() const noexcept {
    if (!destroying_) {
        return -1;
    }
    // floor(count * 10) - 1: the first tenth shows nothing.
    return static_cast<i32>(progress_ * 10.0F) - 1;
}

std::optional<BlockPos> DigController::digging() const noexcept {
    if (!destroying_) {
        return std::nullopt;
    }
    return pos_;
}

void DigController::destroy(const DigTarget& target, DigOutcome& out) {
    out.broken       = target.pos;
    out.broken_state = target.state;
}

void DigController::stop(DigOutcome& out) {
    if (destroying_) {
        out.push(DigStatus::Abort, pos_, face_);
    }
    destroying_ = false;
    progress_   = 0.0F;
}

void DigController::start(const DigInput& input, const DigTarget& target, DigOutcome& out) {
    if (input.creative) {
        if (!input.can_attack_in_creative) {
            return;
        }
        out.push(DigStatus::Start, target.pos, target.face);
        destroy(target, out);
        delay_ = kDestroyDelay;
        return;
    }
    const bool same = destroying_ && target.pos == pos_ && input.held_item == item_;
    if (same) {
        return;
    }
    if (destroying_) {
        out.push(DigStatus::Abort, pos_, face_);
    }
    out.push(DigStatus::Start, target.pos, target.face);
    if (input.progress_per_tick >= 1.0F) {
        // Instant: no count, no cracks, and no delay after it — sweeping a
        // field of grass is one block a tick.
        destroying_ = false;
        destroy(target, out);
        return;
    }
    destroying_ = true;
    pos_        = target.pos;
    face_       = target.face;
    item_       = input.held_item;
    progress_   = 0.0F;
    ticks_      = 0;
}

bool DigController::advance(const DigInput& input, const DigTarget& target, DigOutcome& out) {
    if (delay_ > 0) {
        --delay_;
        return true;
    }
    if (input.creative) {
        if (!input.can_attack_in_creative) {
            return false;
        }
        delay_ = kDestroyDelay;
        out.push(DigStatus::Start, target.pos, target.face);
        destroy(target, out);
        return true;
    }
    const bool same = destroying_ && target.pos == pos_ && input.held_item == item_;
    if (!same) {
        start(input, target, out);
        return true;
    }
    progress_ += input.progress_per_tick;
    if (ticks_ % 4 == 0) {
        out.hit_sound = true;
    }
    ++ticks_;
    if (progress_ >= 1.0F) {
        destroying_ = false;
        out.push(DigStatus::Finish, pos_, face_);
        destroy(target, out);
        progress_ = 0.0F;
        ticks_    = 0;
        delay_    = kDestroyDelay;
    }
    return true;
}

DigOutcome DigController::tick(const DigInput& input) {
    DigOutcome out;
    bool       on_block = input.target && input.target->diggable;

    // The press first, then the hold, in the same tick — which is why a
    // survival dig counts the press's own tick.
    if (input.pressed) {
        if (on_block) {
            start(input, *input.target, out);
        }
        out.swing = true;
        // A block the press broke is air for the hold that follows: the hold
        // aims where the press did, and finds nothing to dig.
        if (out.broken) {
            if (!input.held) {
                stop(out);
            }
            return out;
        }
    }
    if (!input.held) {
        stop(out);
        return out;
    }
    if (!on_block) {
        // Holding, aimed at nothing that can be dug: the count is abandoned.
        if (!input.target) {
            stop(out);
        }
        return out;
    }
    if (advance(input, *input.target, out)) {
        // On the block still there: the tick that broke it has nothing left
        // to chip at.
        out.crack_particle = !out.broken.has_value();
        out.swing          = true;
    }
    return out;
}

}  // namespace ov::gameplay
