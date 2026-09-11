// The End's features: the spikes, the small islands, the chorus, the gateway.
//
// Four configured-feature types that no other dimension uses, each a small
// algorithm with its own draws. Written from the game's documentation of the
// End (the Minecraft Wiki's "End spike", "End island", "Chorus plant", "End
// gateway" pages for the shapes) and checked block for block against an End the
// real server generated (docs/provenance/end.md, `tools/ov_endparity`).
//
// What they cannot do here, named:
//
//   * a spike's End crystal is an *entity*, and a `FeatureLevel` holds blocks.
//     The spike is placed, bedrock on top included; the crystal is the server's
//     to spawn (src/ov_server/src/end_travel.*), where the dragon fight lives.
//   * a gateway's exit (`exit`, `exact`) is block-entity data; the block is
//     placed and the exit is not recorded.
#define OV_LOG_CATEGORY "worldgen"

#include "feature_json.hpp"
#include "ov/base/log.hpp"
#include "ov/worldgen/end.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace ov::worldgen {

namespace {

/// A block's default state by name, or nothing.
[[nodiscard]] std::optional<registry::BlockStateId> state_named(
    const registry::BlockRegistry& blocks, std::string_view name) {
    const auto block = blocks.find_block(name);
    if (!block) {
        return std::nullopt;
    }
    return blocks.default_state(*block);
}

/// The same state with one boolean property set.
[[nodiscard]] registry::BlockStateId with_flag(const registry::BlockRegistry& blocks,
                                               registry::BlockStateId state, std::string_view name,
                                               bool value) {
    const auto property = blocks.find_property(blocks.block_of(state), name);
    if (!property) {
        return state;
    }
    const std::string_view wanted = value ? "true" : "false";
    for (usize index = 0; index < property->values.size(); ++index) {
        if (property->values[index] == wanted) {
            return blocks.with_property(state, *property, static_cast<u16>(index));
        }
    }
    return state;
}

[[nodiscard]] bool is_air_at(const registry::BlockRegistry& blocks, const FeatureLevel& level,
                             i32 x, i32 y, i32 z) {
    if (level.outside_build_height(y)) {
        return true;
    }
    return blocks.is_air(blocks.block_of(level.block_at(x, y, z)));
}

// ── end_spike ───────────────────────────────────────────────────────────────

class EndSpikeFeature final : public Feature {
public:
    EndSpikeFeature(registry::BlockStateId obsidian, registry::BlockStateId bedrock,
                    registry::BlockStateId iron_bars)
        : obsidian_(obsidian), bedrock_(bedrock), iron_bars_(iron_bars) {}

    [[nodiscard]] std::string_view type_name() const override { return "end_spike"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        // Every chunk of the central biome runs the feature, and each spike is
        // built by the one chunk its centre falls in.
        bool placed = false;
        for (const EndSpike& spike : end_spikes(context.level_seed)) {
            if ((spike.centre_x >> 4) != (at.x >> 4) || (spike.centre_z >> 4) != (at.z >> 4)) {
                continue;
            }
            build(*context.blocks, level, random, spike);
            placed = true;
        }
        return placed;
    }

private:
    void build(const registry::BlockRegistry& blocks, FeatureLevel& level, FeatureRandom& random,
               const EndSpike& spike) const {
        const i32 r = spike.radius;
        // The column of the box: obsidian inside the disc (distance from the
        // low corner of each block, squared, at most r^2 + 1) up to the height;
        // everything else in the box above y = 65 cleared to air.
        for (i32 z = spike.centre_z - r; z <= spike.centre_z + r; ++z) {
            for (i32 y = level.min_y(); y <= spike.height + 10; ++y) {
                for (i32 x = spike.centre_x - r; x <= spike.centre_x + r; ++x) {
                    const f64 dx   = static_cast<f64>(x) - static_cast<f64>(spike.centre_x);
                    const f64 dz   = static_cast<f64>(z) - static_cast<f64>(spike.centre_z);
                    const f64 dist = dx * dx + dz * dz;
                    if (dist <= static_cast<f64>(r * r + 1) && y < spike.height) {
                        (void)level.set_block(x, y, z, obsidian_);
                    } else if (y > 65) {
                        (void)level.set_block(x, y, z, registry::kAirState);
                    }
                }
            }
        }

        if (spike.guarded) {
            // A cage of iron bars five wide and four tall round the top, its
            // bars joined along the sides and across the roof.
            for (i32 j = -2; j <= 2; ++j) {
                for (i32 k = -2; k <= 2; ++k) {
                    for (i32 l = 0; l <= 3; ++l) {
                        const bool edge_x = std::abs(j) == 2;
                        const bool edge_z = std::abs(k) == 2;
                        const bool roof   = l == 3;
                        if (!edge_x && !edge_z && !roof) {
                            continue;
                        }
                        const bool along_x = j == -2 || j == 2 || roof;
                        const bool along_z = k == -2 || k == 2 || roof;
                        auto       bars    = iron_bars_;
                        bars = with_flag(blocks, bars, "north", along_x && k != -2);
                        bars = with_flag(blocks, bars, "south", along_x && k != 2);
                        bars = with_flag(blocks, bars, "west", along_z && j != -2);
                        bars = with_flag(blocks, bars, "east", along_z && j != 2);
                        (void)level.set_block(spike.centre_x + j, spike.height + l,
                                              spike.centre_z + k, bars);
                    }
                }
            }
        }

        // The crystal's yaw is the one draw the feature makes. The crystal
        // itself is an entity (see the top of the file); the draw is kept so
        // the generator's stream is the game's.
        (void)random.next_float();
        (void)level.set_block(spike.centre_x, spike.height, spike.centre_z, bedrock_);
    }

    registry::BlockStateId obsidian_;
    registry::BlockStateId bedrock_;
    registry::BlockStateId iron_bars_;
};

// ── end_island ──────────────────────────────────────────────────────────────

class EndIslandFeature final : public Feature {
public:
    explicit EndIslandFeature(registry::BlockStateId end_stone) : end_stone_(end_stone) {}

    [[nodiscard]] std::string_view type_name() const override { return "end_island"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        // A stack of discs, each smaller than the one above, hanging downwards.
        f32 radius = static_cast<f32>(random.next_int(3)) + 4.0F;
        for (i32 dy = 0; radius > 0.5F; --dy) {
            const i32 low  = static_cast<i32>(std::floor(-radius));
            const i32 high = static_cast<i32>(std::ceil(radius));
            for (i32 dx = low; dx <= high; ++dx) {
                for (i32 dz = low; dz <= high; ++dz) {
                    if (static_cast<f32>(dx * dx + dz * dz) <= (radius + 1.0F) * (radius + 1.0F)) {
                        (void)level.set_block(at.x + dx, at.y + dy, at.z + dz, end_stone_);
                    }
                }
            }
            const f32 shrink = static_cast<f32>(random.next_int(2)) + 0.5F;
            radius -= shrink;
        }
        return true;
    }

private:
    registry::BlockStateId end_stone_;
};

// ── chorus_plant ────────────────────────────────────────────────────────────

class ChorusPlantFeature final : public Feature {
public:
    ChorusPlantFeature(registry::BlockStateId plant, registry::BlockStateId flower,
                       registry::BlockId end_stone)
        : plant_(plant), flower_(flower), end_stone_(end_stone) {}

    [[nodiscard]] std::string_view type_name() const override { return "chorus_plant"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto& blocks = *context.blocks;
        if (!is_air_at(blocks, level, at.x, at.y, at.z) ||
            level.outside_build_height(at.y - 1) ||
            blocks.block_of(level.block_at(at.x, at.y - 1, at.z)) != end_stone_) {
            return false;
        }
        set_plant(blocks, level, at);
        grow(blocks, level, random, at, at, 8, 0);
        return true;
    }

private:
    /// North, east, south, west: `Direction.Plane.HORIZONTAL`'s order, which a
    /// random direction is drawn from.
    static constexpr std::array<std::array<i32, 2>, 4> kHorizontal{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
    static constexpr std::array<std::string_view, 4> kSideNames{"north", "east", "south", "west"};

    [[nodiscard]] bool is_chorus(const registry::BlockRegistry& blocks, const FeatureLevel& level,
                                 i32 x, i32 y, i32 z) const {
        if (level.outside_build_height(y)) {
            return false;
        }
        const auto block = blocks.block_of(level.block_at(x, y, z));
        return block == blocks.block_of(plant_) || block == blocks.block_of(flower_);
    }

    /// The plant's state for its neighbours, as the block computes it on
    /// placement: joined to chorus on every side, and down to end stone too.
    void set_plant(const registry::BlockRegistry& blocks, FeatureLevel& level, BlockPos at) const {
        auto state = plant_;
        const bool down = is_chorus(blocks, level, at.x, at.y - 1, at.z) ||
                          (!level.outside_build_height(at.y - 1) &&
                           blocks.block_of(level.block_at(at.x, at.y - 1, at.z)) == end_stone_);
        state = with_flag(blocks, state, "down", down);
        state = with_flag(blocks, state, "up", is_chorus(blocks, level, at.x, at.y + 1, at.z));
        for (usize side = 0; side < 4; ++side) {
            state = with_flag(blocks, state, kSideNames[side],
                              is_chorus(blocks, level, at.x + kHorizontal[side][0], at.y,
                                        at.z + kHorizontal[side][1]));
        }
        (void)level.set_block(at.x, at.y, at.z, state);
    }

    [[nodiscard]] bool neighbours_empty(const registry::BlockRegistry& blocks,
                                        const FeatureLevel& level, BlockPos at,
                                        i32 except) const {
        for (i32 side = 0; side < 4; ++side) {
            if (side == except) {
                continue;
            }
            if (!is_air_at(blocks, level, at.x + kHorizontal[static_cast<usize>(side)][0], at.y,
                           at.z + kHorizontal[static_cast<usize>(side)][1])) {
                return false;
            }
        }
        return true;
    }

    // NOLINTNEXTLINE(misc-no-recursion) — four levels deep at most.
    void grow(const registry::BlockRegistry& blocks, FeatureLevel& level, FeatureRandom& random,
              BlockPos branch, BlockPos origin, i32 spread, i32 iteration) const {
        i32 height = random.next_int(4) + 1;
        if (iteration == 0) {
            ++height;
        }
        for (i32 j = 0; j < height; ++j) {
            const BlockPos up{branch.x, branch.y + j + 1, branch.z};
            if (!neighbours_empty(blocks, level, up, -1)) {
                return;
            }
            set_plant(blocks, level, up);
            set_plant(blocks, level, BlockPos{up.x, up.y - 1, up.z});
        }

        bool branched = false;
        if (iteration < 4) {
            i32 branches = random.next_int(4);
            if (iteration == 0) {
                ++branches;
            }
            for (i32 l = 0; l < branches; ++l) {
                const i32      side = random.next_int(4);
                const auto&    step = kHorizontal[static_cast<usize>(side)];
                const BlockPos next{branch.x + step[0], branch.y + height, branch.z + step[1]};
                const i32      opposite = (side + 2) % 4;
                if (std::abs(next.x - origin.x) < spread && std::abs(next.z - origin.z) < spread &&
                    is_air_at(blocks, level, next.x, next.y, next.z) &&
                    is_air_at(blocks, level, next.x, next.y - 1, next.z) &&
                    neighbours_empty(blocks, level, next, opposite)) {
                    branched = true;
                    set_plant(blocks, level, next);
                    set_plant(blocks, level, BlockPos{next.x - step[0], next.y, next.z - step[1]});
                    grow(blocks, level, random, next, origin, spread, iteration + 1);
                }
            }
        }
        if (!branched) {
            (void)level.set_block(branch.x, branch.y + height, branch.z, flower_);
        }
    }

    registry::BlockStateId plant_;
    registry::BlockStateId flower_;
    registry::BlockId      end_stone_;
};

// ── end_gateway ─────────────────────────────────────────────────────────────

class EndGatewayFeature final : public Feature {
public:
    EndGatewayFeature(registry::BlockStateId gateway, registry::BlockStateId bedrock)
        : gateway_(gateway), bedrock_(bedrock) {}

    [[nodiscard]] std::string_view type_name() const override { return "end_gateway"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom&,
               BlockPos at) const override {
        // The gateway in a 3 x 5 x 3 box: bedrock above and below it and down
        // the four sides of its column, air round it on its own level.
        for (i32 z = at.z - 1; z <= at.z + 1; ++z) {
            for (i32 y = at.y - 2; y <= at.y + 2; ++y) {
                for (i32 x = at.x - 1; x <= at.x + 1; ++x) {
                    const bool same_x = x == at.x;
                    const bool same_y = y == at.y;
                    const bool same_z = z == at.z;
                    const bool ends   = std::abs(y - at.y) == 2;
                    registry::BlockStateId state = registry::kAirState;
                    if (same_x && same_y && same_z) {
                        state = gateway_;
                    } else if (same_y) {
                        state = registry::kAirState;
                    } else if ((ends && same_x && same_z) || ((same_x || same_z) && !ends)) {
                        state = bedrock_;
                    }
                    (void)level.set_block(x, y, z, state);
                }
            }
        }
        return true;
    }

private:
    registry::BlockStateId gateway_;
    registry::BlockStateId bedrock_;
};

}  // namespace

bool is_end_feature(std::string_view kind) noexcept {
    return kind == "end_spike" || kind == "end_island" || kind == "chorus_plant" ||
           kind == "end_gateway";
}

std::expected<FeatureRef, FeatureError> parse_end_feature(std::string_view kind, Json config,
                                                          const registry::BlockRegistry& blocks) {
    const auto need = [&](std::string_view name) -> std::optional<registry::BlockStateId> {
        auto state = state_named(blocks, name);
        if (!state) {
            OV_LOG_ERROR("worldgen: {} needs {}, which the block registry does not have", kind,
                         name);
        }
        return state;
    };

    if (kind == "end_spike") {
        // `spikes: []` is every vanilla world: the ten are computed from the
        // seed. A datapack's own list is refused rather than ignored.
        simdjson::dom::array listed;
        if (config.at_key("spikes").get(listed) == simdjson::SUCCESS && listed.size() != 0) {
            OV_LOG_ERROR("worldgen: end_spike with an explicit spike list is not implemented");
            return std::unexpected(FeatureError::Unsupported);
        }
        if (config.at_key("crystal_beam_target").error() == simdjson::SUCCESS) {
            OV_LOG_ERROR("worldgen: end_spike with a crystal beam target is not implemented");
            return std::unexpected(FeatureError::Unsupported);
        }
        const auto obsidian = need("minecraft:obsidian");
        const auto bedrock  = need("minecraft:bedrock");
        const auto bars     = need("minecraft:iron_bars");
        if (!obsidian || !bedrock || !bars) {
            return std::unexpected(FeatureError::Missing);
        }
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const EndSpikeFeature>(*obsidian, *bedrock, *bars));
    }
    if (kind == "end_island") {
        const auto end_stone = need("minecraft:end_stone");
        if (!end_stone) {
            return std::unexpected(FeatureError::Missing);
        }
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const EndIslandFeature>(*end_stone));
    }
    if (kind == "chorus_plant") {
        const auto plant     = need("minecraft:chorus_plant");
        const auto flower    = need("minecraft:chorus_flower");
        const auto end_stone = blocks.find_block("minecraft:end_stone");
        if (!plant || !flower || !end_stone) {
            return std::unexpected(FeatureError::Missing);
        }
        // The flower a finished branch ends in is fully grown: age 5.
        auto grown = *flower;
        if (const auto age = blocks.find_property(blocks.block_of(grown), "age")) {
            for (usize index = 0; index < age->values.size(); ++index) {
                if (age->values[index] == "5") {
                    grown = blocks.with_property(grown, *age, static_cast<u16>(index));
                }
            }
        }
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const ChorusPlantFeature>(*plant, grown, *end_stone));
    }
    if (kind == "end_gateway") {
        const auto gateway = need("minecraft:end_gateway");
        const auto bedrock = need("minecraft:bedrock");
        if (!gateway || !bedrock) {
            return std::unexpected(FeatureError::Missing);
        }
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const EndGatewayFeature>(*gateway, *bedrock));
    }
    return std::unexpected(FeatureError::Unsupported);
}

}  // namespace ov::worldgen
