// ── mobs-2 ── Zombies and husks that drown, over the server's entities.
//
// The rule is ov_gameplay's (conversion.hpp). This keeps a counter per mob by
// wire id and reports which mobs must be replaced this tick; the server does
// the replacing, because only it can spawn and tell clients.
#pragma once

#include "ov/base/types.hpp"
#include "ov/entity/world.hpp"
#include "ov/gameplay/conversion.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/registry/registries.hpp"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace ov::server {

class Drowning {
public:
    explicit Drowning(const registry::Registries& registries);

    struct Conversion {
        entity::EntityHandle handle{entity::kNoEntity};
        std::string_view     to;
    };

    /// Are the eyes at this block in water? A plain function and a context
    /// pointer, so the tick builds nothing on the heap.
    using WaterAt = bool (*)(const void* context, BlockPos pos);

    /// One tick of every zombie and husk. Appends the mobs to replace.
    void tick(const entity::EntityWorld& world, WaterAt water, const void* context,
              std::vector<Conversion>& out);

private:
    i32                                              zombie_{-1};
    i32                                              husk_{-1};
    std::unordered_map<i32, gameplay::DrowningState> states_;
    std::vector<i32>                                 seen_;
};

}  // namespace ov::server
