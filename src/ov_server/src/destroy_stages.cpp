#include "destroy_stages.hpp"

namespace ov::server {

std::optional<net::BlockDestroyStage> next_destroy_stage(DestroyStageState& state, i32 entity_id,
                                                         bool counting, net::WirePosition where,
                                                         f32 count) noexcept {
    if (counting) {
        const i8 stage = net::server_destroy_stage(count);
        if (stage == state.sent && where == state.where) {
            return std::nullopt;
        }
        state.sent  = stage;
        state.where = where;
        return net::BlockDestroyStage{entity_id, where, stage};
    }
    if (state.sent == -1) {
        return std::nullopt;
    }
    state.sent = -1;
    return net::BlockDestroyStage{entity_id, state.where, -1};
}

void forget_destroy_stage(DestroyStageState& state) noexcept { state.sent = -1; }

}  // namespace ov::server
