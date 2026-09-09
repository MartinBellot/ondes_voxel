#include "player_level.hpp"

#include <utility>

namespace ov::server {

PlayerLevel::PlayerLevel(PlayerLevelHooks hooks) : hooks_{std::move(hooks)} {}

registry::BlockStateId PlayerLevel::block_at(BlockPos pos) const {
    if (!shape_.contains_y(pos.y) || !hooks_.block_at) {
        return registry::kAirState;
    }
    return hooks_.block_at(pos);
}

bool PlayerLevel::is_loaded(BlockPos pos) const {
    return hooks_.is_loaded && shape_.contains_y(pos.y) && hooks_.is_loaded(pos);
}

void PlayerLevel::set_block(BlockPos pos, registry::BlockStateId state) {
    if (!shape_.contains_y(pos.y) || !hooks_.set_block) {
        return;
    }
    hooks_.set_block(pos, state);
    ++writes_;
}

void PlayerLevel::schedule_tick(BlockPos pos, std::string_view what, i64 delay,
                                world::TickQueue queue, world::TickPriority priority) {
    if (!hooks_.schedule_tick) {
        return;
    }
    hooks_.schedule_tick(pos, what, delay, queue, priority);
}

bool PlayerLevel::has_scheduled_tick(BlockPos pos, std::string_view what,
                                     world::TickQueue queue) const {
    return hooks_.has_scheduled_tick && hooks_.has_scheduled_tick(pos, what, queue);
}

i64 PlayerLevel::game_time() const { return hooks_.game_time ? hooks_.game_time() : 0; }

}  // namespace ov::server
