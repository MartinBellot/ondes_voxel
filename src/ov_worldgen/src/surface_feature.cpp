#define OV_LOG_CATEGORY "worldgen"

// The rest of the surface: `vines`, `forest_rock`, `lake`, `ice_spike`.
//
// Claimed through parse_terrain_feature in terrain_feature.cpp, which also
// names the types this generator refuses on purpose.

#include "overworld_feature.hpp"

#include "ov/base/log.hpp"

#include <array>
#include <cmath>

namespace ov::worldgen {

namespace {

[[nodiscard]] f64 number_field(Json node, std::string_view key, f64 fallback) {
    f64 value = fallback;
    if (node.at_key(key).get(value) == simdjson::SUCCESS) {
        return value;
    }
    i64 whole = 0;
    if (node.at_key(key).get(whole) == simdjson::SUCCESS) {
        return static_cast<f64>(whole);
    }
    return fallback;
}

// ── vines ───────────────────────────────────────────────────────────────────

/// One vine on the first side, in `Direction` order and never the floor,
/// whose neighbour can carry it.
///
/// "Can carry it" is `MultifaceBlock.canAttachTo`: the neighbour's support
/// shape is a full face towards the vine. That is answered from the measured
/// sturdy faces, which are the same full-face question.
class VinesFeature final : public Feature {
public:
    explicit VinesFeature(registry::BlockId vine) : vine_(vine) {}

    [[nodiscard]] std::string_view type_name() const override { return "vines"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom&,
               BlockPos at) const override {
        const auto& blocks = *context.blocks;
        if (!blocks.is_air(blocks.block_of(level.block_at(at.x, at.y, at.z)))) {
            return false;
        }
        using Face = registry::BlockRegistry::Face;
        struct Side {
            std::array<i32, 3> offset;
            Face               facing_back;
            std::string_view   property;
        };
        // `Direction.values()` minus DOWN: up, north, south, west, east.
        static constexpr std::array<Side, 5> kSides{{
            {{0, 1, 0}, Face::Down, "up"},
            {{0, 0, -1}, Face::South, "north"},
            {{0, 0, 1}, Face::North, "south"},
            {{-1, 0, 0}, Face::East, "west"},
            {{1, 0, 0}, Face::West, "east"},
        }};
        for (const Side& side : kSides) {
            const auto neighbour =
                level.block_at(at.x + side.offset[0], at.y + side.offset[1], at.z + side.offset[2]);
            if (!blocks.face_is_sturdy(neighbour, side.facing_back)) {
                continue;
            }
            const auto state =
                with_property_value(blocks, blocks.default_state(vine_), side.property, "true");
            (void)level.set_block(at.x, at.y, at.z, state);
            return true;
        }
        return false;
    }

private:
    registry::BlockId vine_;
};

// ── forest_rock ─────────────────────────────────────────────────────────────

/// `BlockBlobFeature`: sink to the first dirt or stone, then three small
/// balls, each shifted a little down and sideways from the last.
class ForestRockFeature final : public Feature {
public:
    ForestRockFeature(registry::BlockStateId state, std::vector<u16> dirt, std::vector<u16> stone)
        : state_(state), dirt_(std::move(dirt)), stone_(std::move(stone)) {}

    [[nodiscard]] std::string_view type_name() const override { return "forest_rock"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto& blocks = *context.blocks;
        BlockPos    pos    = at;
        while (pos.y > level.min_y() + 3) {
            const auto below = blocks.block_of(level.block_at(pos.x, pos.y - 1, pos.z));
            if (!blocks.is_air(below) && (holds(dirt_, below) || holds(stone_, below))) {
                break;
            }
            pos = pos.below();
        }
        if (pos.y <= level.min_y() + 3) {
            return false;
        }
        for (i32 i = 0; i < 3; ++i) {
            const i32 rx = random.next_int(2);
            const i32 ry = random.next_int(2);
            const i32 rz = random.next_int(2);
            const f32 reach = static_cast<f32>(rx + ry + rz) * 0.333F + 0.5F;
            const f64 limit = static_cast<f64>(reach * reach);
            for (i32 z = pos.z - rz; z <= pos.z + rz; ++z) {
                for (i32 y = pos.y - ry; y <= pos.y + ry; ++y) {
                    for (i32 x = pos.x - rx; x <= pos.x + rx; ++x) {
                        const f64 dx = static_cast<f64>(x - pos.x);
                        const f64 dy = static_cast<f64>(y - pos.y);
                        const f64 dz = static_cast<f64>(z - pos.z);
                        if (dx * dx + dy * dy + dz * dz <= limit) {
                            (void)level.set_block(x, y, z, state_);
                        }
                    }
                }
            }
            const i32 sx = random.next_int(2);
            const i32 sy = random.next_int(2);
            const i32 sz = random.next_int(2);
            pos          = pos.offset(-1 + sx, -sy, -1 + sz);
        }
        return true;
    }

private:
    registry::BlockStateId state_;
    std::vector<u16>       dirt_;
    std::vector<u16>       stone_;
};

// ── lake ────────────────────────────────────────────────────────────────────

/// `LakeFeature`, deprecated in the game and still what puts lava lakes on
/// the surface and underground.
///
/// Four to seven ellipsoids inside a 16 by 8 by 16 box, a check that the
/// hollow is sealed — no liquid above the waterline, only solid (or the same
/// fluid) below it — then the fill and a barrier shell.
///
/// Not reproduced: the block and fluid ticks the game schedules on the air it
/// cuts, and the "freeze the top" pass, which only a water lake runs; vanilla
/// 1.20.1 configures no water lake.
class LakeFeature final : public Feature {
public:
    LakeFeature(StateProviderRef fluid, StateProviderRef barrier, std::vector<u16> cannot_replace,
                std::vector<u16> lava_stone_cannot_replace)
        : fluid_(std::move(fluid)),
          barrier_(std::move(barrier)),
          cannot_replace_(std::move(cannot_replace)),
          lava_stone_cannot_replace_(std::move(lava_stone_cannot_replace)) {}

    [[nodiscard]] std::string_view type_name() const override { return "lake"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        const auto& blocks = *context.blocks;
        if (origin.y <= level.min_y() + 4) {
            return false;
        }
        const BlockPos base = origin.below(4);
        std::array<bool, 2048> hollow{};
        const auto index = [](i32 x, i32 z, i32 y) {
            return static_cast<usize>((x * 16 + z) * 8 + y);
        };

        const i32 blobs = random.next_int(4) + 4;
        for (i32 j = 0; j < blobs; ++j) {
            const f64 width_x = random.next_double() * 6.0 + 3.0;
            const f64 width_y = random.next_double() * 4.0 + 2.0;
            const f64 width_z = random.next_double() * 6.0 + 3.0;
            const f64 centre_x = random.next_double() * (16.0 - width_x - 2.0) + 1.0 + width_x / 2.0;
            const f64 centre_y = random.next_double() * (8.0 - width_y - 4.0) + 2.0 + width_y / 2.0;
            const f64 centre_z = random.next_double() * (16.0 - width_z - 2.0) + 1.0 + width_z / 2.0;
            for (i32 x = 1; x < 15; ++x) {
                for (i32 z = 1; z < 15; ++z) {
                    for (i32 y = 1; y < 7; ++y) {
                        const f64 o = (static_cast<f64>(x) - centre_x) / (width_x / 2.0);
                        const f64 p = (static_cast<f64>(y) - centre_y) / (width_y / 2.0);
                        const f64 q = (static_cast<f64>(z) - centre_z) / (width_z / 2.0);
                        if (o * o + p * p + q * q < 1.0) {
                            hollow[index(x, z, y)] = true;
                        }
                    }
                }
            }
        }

        const auto rim = [&](i32 x, i32 z, i32 y) {
            if (hollow[index(x, z, y)]) {
                return false;
            }
            return (x < 15 && hollow[index(x + 1, z, y)]) || (x > 0 && hollow[index(x - 1, z, y)]) ||
                   (z < 15 && hollow[index(x, z + 1, y)]) || (z > 0 && hollow[index(x, z - 1, y)]) ||
                   (y < 7 && hollow[index(x, z, y + 1)]) || (y > 0 && hollow[index(x, z, y - 1)]);
        };

        const auto fill = fluid_->state(level, random, base);
        for (i32 x = 0; x < 16; ++x) {
            for (i32 z = 0; z < 16; ++z) {
                for (i32 y = 0; y < 8; ++y) {
                    if (!rim(x, z, y)) {
                        continue;
                    }
                    const auto state = level.block_at(base.x + x, base.y + y, base.z + z);
                    const auto block = blocks.block_of(state);
                    if (y >= 4 && is_liquid(blocks, block)) {
                        return false;
                    }
                    if (y < 4 && !is_solid(blocks, state) && state != fill) {
                        return false;
                    }
                }
            }
        }

        const auto air = blocks.default_state(*blocks.find_block("minecraft:air"));
        for (i32 x = 0; x < 16; ++x) {
            for (i32 z = 0; z < 16; ++z) {
                for (i32 y = 0; y < 8; ++y) {
                    if (!hollow[index(x, z, y)]) {
                        continue;
                    }
                    const BlockPos at{base.x + x, base.y + y, base.z + z};
                    const auto     here = blocks.block_of(level.block_at(at.x, at.y, at.z));
                    if (holds(cannot_replace_, here)) {
                        continue;
                    }
                    (void)level.set_block(at.x, at.y, at.z, y >= 4 ? air : fill);
                }
            }
        }

        const auto shell = barrier_->state(level, random, base);
        if (!blocks.is_air(blocks.block_of(shell))) {
            for (i32 x = 0; x < 16; ++x) {
                for (i32 z = 0; z < 16; ++z) {
                    for (i32 y = 0; y < 8; ++y) {
                        if (!rim(x, z, y)) {
                            continue;
                        }
                        // The coin is tossed only in the upper half.
                        if (y >= 4 && random.next_int(2) == 0) {
                            continue;
                        }
                        const BlockPos at{base.x + x, base.y + y, base.z + z};
                        const auto     here = level.block_at(at.x, at.y, at.z);
                        if (!is_solid(blocks, here) ||
                            holds(lava_stone_cannot_replace_, blocks.block_of(here))) {
                            continue;
                        }
                        (void)level.set_block(at.x, at.y, at.z, shell);
                    }
                }
            }
        }
        return true;
    }

private:
    [[nodiscard]] static bool is_liquid(const registry::BlockRegistry& blocks,
                                        registry::BlockId               block) {
        const auto name = blocks.block_name(block);
        return name == "minecraft:water" || name == "minecraft:lava";
    }

    StateProviderRef fluid_;
    StateProviderRef barrier_;
    std::vector<u16> cannot_replace_;
    std::vector<u16> lava_stone_cannot_replace_;
};

// ── ice_spike ───────────────────────────────────────────────────────────────

class IceSpikeFeature final : public Feature {
public:
    IceSpikeFeature(registry::BlockId snow_block, registry::BlockId ice, registry::BlockId packed,
                    std::vector<u16> dirt)
        : snow_block_(snow_block), ice_(ice), packed_(packed), dirt_(std::move(dirt)) {}

    [[nodiscard]] std::string_view type_name() const override { return "ice_spike"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos origin) const override {
        const auto& blocks = *context.blocks;
        const auto  block  = [&](BlockPos p) { return blocks.block_of(level.block_at(p.x, p.y, p.z)); };
        BlockPos    pos    = origin;
        while (blocks.is_air(block(pos)) && pos.y > level.min_y() + 2) {
            pos = pos.below();
        }
        if (block(pos) != snow_block_) {
            return false;
        }
        pos             = pos.above(random.next_int(4));
        const i32 tall  = random.next_int(4) + 7;
        const i32 width = tall / 4 + random.next_int(2);
        if (width > 1 && random.next_int(60) == 0) {
            pos = pos.above(10 + random.next_int(30));
        }
        const auto packed = blocks.default_state(packed_);
        const auto meltable = [&](BlockPos p) {
            const auto b = block(p);
            return blocks.is_air(b) || holds(dirt_, b) || b == snow_block_ || b == ice_;
        };
        for (i32 k = 0; k < tall; ++k) {
            const f32 radius = (1.0F - static_cast<f32>(k) / static_cast<f32>(tall)) *
                               static_cast<f32>(width);
            const auto reach = static_cast<i32>(std::ceil(radius));
            for (i32 m = -reach; m <= reach; ++m) {
                const f32 g = static_cast<f32>(std::abs(m)) - 0.25F;
                for (i32 n = -reach; n <= reach; ++n) {
                    const f32 h = static_cast<f32>(std::abs(n)) - 0.25F;
                    if ((m != 0 || n != 0) && g * g + h * h > radius * radius) {
                        continue;
                    }
                    if (m == -reach || m == reach || n == -reach || n == reach) {
                        if (random.next_float() > 0.75F) {
                            continue;
                        }
                    }
                    const BlockPos up = pos.offset(m, k, n);
                    if (meltable(up)) {
                        (void)level.set_block(up.x, up.y, up.z, packed);
                    }
                    if (k == 0 || reach <= 1) {
                        continue;
                    }
                    const BlockPos down = pos.offset(m, -k, n);
                    if (meltable(down)) {
                        (void)level.set_block(down.x, down.y, down.z, packed);
                    }
                }
            }
        }

        const i32 foot = std::clamp(width - 1, 0, 1);
        for (i32 o = -foot; o <= foot; ++o) {
            for (i32 m = -foot; m <= foot; ++m) {
                BlockPos cursor = pos.offset(o, -1, m);
                i32      budget = 50;
                if (std::abs(o) == 1 && std::abs(m) == 1) {
                    budget = random.next_int(5);
                }
                while (cursor.y > 50) {
                    const auto b = block(cursor);
                    if (!blocks.is_air(b) && !holds(dirt_, b) && b != snow_block_ && b != ice_ &&
                        b != packed_) {
                        break;
                    }
                    (void)level.set_block(cursor.x, cursor.y, cursor.z, packed);
                    cursor = cursor.below();
                    if (--budget > 0) {
                        continue;
                    }
                    cursor = cursor.below(random.next_int(5) + 1);
                    budget = random.next_int(5);
                }
            }
        }
        return true;
    }

private:
    registry::BlockId snow_block_;
    registry::BlockId ice_;
    registry::BlockId packed_;
    std::vector<u16>  dirt_;
};

}  // namespace

ClaimedFeature parse_surface_feature(std::string_view kind, Json config,
                                     const registry::BlockRegistry& blocks, const BlockTags& tags) {
    if (kind == "vines") {
        auto vine = named_block(blocks, "minecraft:vine");
        if (!vine) return std::unexpected(vine.error());
        return std::static_pointer_cast<const Feature>(std::make_shared<const VinesFeature>(*vine));
    }
    if (kind == "forest_rock") {
        auto field = config.at_key("state");
        if (field.error() != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);
        auto state = parse_block_state(field.value(), blocks);
        if (!state) return std::unexpected(state.error());
        auto dirt  = tag_members(blocks, tags, "minecraft:dirt");
        auto stone = tag_members(blocks, tags, "minecraft:base_stone_overworld");
        if (!dirt) return std::unexpected(dirt.error());
        if (!stone) return std::unexpected(stone.error());
        return std::static_pointer_cast<const Feature>(std::make_shared<const ForestRockFeature>(
            *state, std::move(*dirt), std::move(*stone)));
    }
    if (kind == "lake") {
        auto fluid_field   = config.at_key("fluid");
        auto barrier_field = config.at_key("barrier");
        if (fluid_field.error() != simdjson::SUCCESS || barrier_field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto fluid   = parse_state_provider(fluid_field.value(), blocks, tags);
        auto barrier = parse_state_provider(barrier_field.value(), blocks, tags);
        if (!fluid) return std::unexpected(fluid.error());
        if (!barrier) return std::unexpected(barrier.error());
        auto cannot = tag_members(blocks, tags, "minecraft:features_cannot_replace");
        auto lava   = tag_members(blocks, tags, "minecraft:lava_pool_stone_cannot_replace");
        if (!cannot) return std::unexpected(cannot.error());
        if (!lava) return std::unexpected(lava.error());
        // The water-only freeze pass is not written; a water lake would need it.
        for (const auto state : (*fluid)->possible_states()) {
            if (blocks.block_name(blocks.block_of(state)) == "minecraft:water") {
                OV_LOG_ERROR("worldgen: a water lake needs the freeze pass, which is not written");
                return std::unexpected(FeatureError::Unsupported);
            }
        }
        return std::static_pointer_cast<const Feature>(std::make_shared<const LakeFeature>(
            *fluid, *barrier, std::move(*cannot), std::move(*lava)));
    }
    if (kind == "ice_spike") {
        auto snow   = named_block(blocks, "minecraft:snow_block");
        auto ice    = named_block(blocks, "minecraft:ice");
        auto packed = named_block(blocks, "minecraft:packed_ice");
        auto dirt   = tag_members(blocks, tags, "minecraft:dirt");
        if (!snow || !ice || !packed || !dirt) return std::unexpected(FeatureError::Missing);
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const IceSpikeFeature>(*snow, *ice, *packed, std::move(*dirt)));
    }
    (void)number_field;
    return std::nullopt;
}

}  // namespace ov::worldgen
