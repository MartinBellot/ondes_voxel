// ── nether-2 ── The Nether's feature types.
//
// Nine configured-feature types, each a small algorithm with its own draws.
// Specified from the game's documentation of the blocks and biomes they build
// (the Minecraft Wiki's "Glowstone", "Weeping Vines", "Twisting Vines", "Huge
// fungus", "Basalt Deltas", "Soul Sand Valley", "Crimson/Warped Forest"
// pages) for the shapes and the probabilities, and fixed where the pages are
// silent — the order of the draws, the loop bounds — by measurement against a
// Nether the real 1.20.1 server generated at the same seed
// (`tools/ov_netherparity --full`, docs/provenance/nether-2.md). No third-party
// code and no game code were read.
//
// Conventions shared by every feature here:
//
//   * `empty` is `Level.isEmptyBlock`: air, cave air or void air — and a
//     position outside the level, which the game reads as void air;
//   * a draw written `a - b` in the specification is split into named
//     variables in order: C++ does not sequence the operands of `-`;
//   * `between(r, lo, hi)` is `Mth.nextInt`: `lo` when `lo >= hi`, otherwise
//     one `nextInt(hi - lo + 1) + lo`.
#define OV_LOG_CATEGORY "worldgen"

#include "nether_feature.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

namespace ov::worldgen {

bool ManhattanWalk::next(BlockPos& out) noexcept {
    // A position with z != 0 is followed by its mirror in z, and only then does
    // the walk move on.
    if (z_mirror_) {
        z_mirror_  = false;
        cursor_.z  = centre_.z - (cursor_.z - centre_.z);
        out        = cursor_;
        return true;
    }
    for (;;) {
        if (y_ > max_y_) {
            ++x_;
            if (x_ > max_x_) {
                ++depth_;
                if (depth_ > max_depth_) {
                    return false;
                }
                max_x_ = std::min(rx_, depth_);
                x_     = -max_x_;
            }
            max_y_ = std::min(ry_, depth_ - std::abs(x_));
            y_     = -max_y_;
        }
        const i32 xx = x_;
        const i32 yy = y_;
        const i32 zz = depth_ - std::abs(xx) - std::abs(yy);
        ++y_;
        if (zz <= rz_) {
            z_mirror_ = zz != 0;
            cursor_   = BlockPos{centre_.x + xx, centre_.y + yy, centre_.z + zz};
            out       = cursor_;
            return true;
        }
    }
}

namespace {

/// `Mth.nextInt(random, lo, hi)`.
[[nodiscard]] i32 between(FeatureRandom& random, i32 lo, i32 hi) noexcept {
    return lo >= hi ? lo : random.next_int(hi - lo + 1) + lo;
}

/// `a.nextInt(n) - a.nextInt(n)`, the two draws in order.
[[nodiscard]] i32 spread(FeatureRandom& random, i32 bound) noexcept {
    const i32 plus  = random.next_int(bound);
    const i32 minus = random.next_int(bound);
    return plus - minus;
}

/// `Direction.values()`: down, up, north, south, west, east.
constexpr std::array<std::array<i32, 3>, 6> kDirections{{
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
}};

/// The same question every feature asks, over one registry.
class Reader {
public:
    Reader(const registry::BlockRegistry& blocks, const FeatureLevel& level)
        : blocks_(blocks), level_(level) {}

    [[nodiscard]] registry::BlockId block(BlockPos at) const {
        return blocks_.block_of(state(at));
    }
    [[nodiscard]] registry::BlockStateId state(BlockPos at) const {
        if (at.y < level_.min_y()) {
            return registry::kAirState;
        }
        return level_.block_at(at.x, at.y, at.z);
    }
    [[nodiscard]] bool empty(BlockPos at) const { return blocks_.is_air(block(at)); }

private:
    const registry::BlockRegistry& blocks_;
    const FeatureLevel&            level_;
};

void put(FeatureLevel& level, BlockPos at, registry::BlockStateId state) {
    (void)level.set_block(at.x, at.y, at.z, state);
}

/// One block's default state, or a refusal naming it.
[[nodiscard]] std::expected<registry::BlockStateId, FeatureError> default_of(
    const registry::BlockRegistry& blocks, std::string_view name) {
    auto block = named_block(blocks, name);
    if (!block) {
        return std::unexpected(block.error());
    }
    return blocks.default_state(*block);
}

// ── The vine column, shared by weeping vines, twisting vines and the fungi ──

/// A vine column: plant blocks, capped by a head of age `min_age..max_age`,
/// grown along `dy` (down for weeping, up for twisting) while the next block
/// is empty. The head goes on the last of `length`, or earlier where the
/// column would run into something.
struct VineBlocks {
    registry::BlockStateId head{};
    registry::BlockStateId plant{};
};

void vine_column(const registry::BlockRegistry& blocks, FeatureLevel& level,
                 FeatureRandom& random, BlockPos at, i32 length, i32 min_age, i32 max_age,
                 const VineBlocks& vine, i32 dy) {
    const Reader read{blocks, level};
    BlockPos     pos = at;
    // Measurement arm (nether-2.md § 1): `OV_NETHER_VINE_EXTRA=1` grows one
    // block more than `length`, the other reading of "a column of length n".
    static const i32 extra = [] {
        const char* setting = std::getenv("OV_NETHER_VINE_EXTRA");
        return setting != nullptr && std::string_view{setting} == "1" ? 1 : 0;
    }();
    const i32 last = length - 1 + extra;
    for (i32 i = 0; i <= last; ++i) {
        if (read.empty(pos)) {
            if (i == last || !read.empty(pos.offset(0, dy, 0))) {
                const i32 age = between(random, min_age, max_age);
                put(level, pos,
                    with_property_value(blocks, vine.head, "age", std::to_string(age)));
                break;
            }
            put(level, pos, vine.plant);
        }
        pos = pos.offset(0, dy, 0);
    }
}

// ── glowstone_blob ──────────────────────────────────────────────────────────

/// A cluster hanging from the roof: the origin, then 1500 tries below it, each
/// kept where exactly one of its six neighbours is already glowstone.
class GlowstoneBlobFeature final : public Feature {
public:
    GlowstoneBlobFeature(registry::BlockStateId glowstone, std::array<registry::BlockId, 3> roof)
        : glowstone_(glowstone), roof_(roof) {}

    [[nodiscard]] std::string_view type_name() const override { return "glowstone_blob"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto&  blocks = *context.blocks;
        const Reader read{blocks, level};
        if (!read.empty(at)) {
            return false;
        }
        const registry::BlockId above = read.block(at.above());
        if (std::ranges::find(roof_, above) == roof_.end()) {
            return false;
        }
        put(level, at, glowstone_);
        const registry::BlockId glowstone = blocks.block_of(glowstone_);
        for (i32 i = 0; i < 1500; ++i) {
            const i32      dx  = spread(random, 8);
            const i32      dy  = -random.next_int(12);
            const i32      dz  = spread(random, 8);
            const BlockPos pos = at.offset(dx, dy, dz);
            if (!read.empty(pos)) {
                continue;
            }
            i32 touching = 0;
            for (const auto& d : kDirections) {
                if (read.block(pos.offset(d[0], d[1], d[2])) == glowstone) {
                    ++touching;
                }
                if (touching > 1) {
                    break;
                }
            }
            if (touching == 1) {
                put(level, pos, glowstone_);
            }
        }
        return true;
    }

private:
    registry::BlockStateId           glowstone_;
    std::array<registry::BlockId, 3> roof_;  // netherrack, basalt, blackstone
};

// ── weeping_vines ───────────────────────────────────────────────────────────

/// A patch of nether wart block on the roof, and vines hanging from it.
class WeepingVinesFeature final : public Feature {
public:
    WeepingVinesFeature(registry::BlockStateId wart, registry::BlockId netherrack, VineBlocks vine)
        : wart_(wart), netherrack_(netherrack), vine_(vine) {}

    [[nodiscard]] std::string_view type_name() const override { return "weeping_vines"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto&  blocks = *context.blocks;
        const Reader read{blocks, level};
        if (!read.empty(at)) {
            return false;
        }
        const registry::BlockId wart  = blocks.block_of(wart_);
        const auto              roofy = [&](registry::BlockId block) {
            return block == netherrack_ || block == wart;
        };
        if (!roofy(read.block(at.above()))) {
            return false;
        }

        // The wart: the origin, then 200 tries around it.
        put(level, at, wart_);
        for (i32 i = 0; i < 200; ++i) {
            const i32      dx  = spread(random, 6);
            const i32      up  = random.next_int(2);
            const i32      dn  = random.next_int(5);
            const i32      dz  = spread(random, 6);
            const BlockPos pos = at.offset(dx, up - dn, dz);
            if (!read.empty(pos)) {
                continue;
            }
            i32 touching = 0;
            for (const auto& d : kDirections) {
                if (roofy(read.block(pos.offset(d[0], d[1], d[2])))) {
                    ++touching;
                }
                if (touching > 1) {
                    break;
                }
            }
            if (touching == 1) {
                put(level, pos, wart_);
            }
        }

        // The vines: 100 tries, each a column hanging from roof or wart.
        for (i32 i = 0; i < 100; ++i) {
            const i32      dx  = spread(random, 8);
            const i32      up  = random.next_int(2);
            const i32      dn  = random.next_int(7);
            const i32      dz  = spread(random, 8);
            const BlockPos pos = at.offset(dx, up - dn, dz);
            if (!read.empty(pos) || !roofy(read.block(pos.above()))) {
                continue;
            }
            i32 length = between(random, 1, 8);
            if (random.next_int(6) == 0) {
                length *= 2;
            }
            if (random.next_int(5) == 0) {
                length = 1;
            }
            vine_column(blocks, level, random, pos, length, 17, 25, vine_, -1);
        }
        return true;
    }

private:
    registry::BlockStateId wart_;
    registry::BlockId      netherrack_;
    VineBlocks             vine_;
};

// ── twisting_vines ──────────────────────────────────────────────────────────

class TwistingVinesFeature final : public Feature {
public:
    TwistingVinesFeature(i32 width, i32 height, i32 max_height, std::array<registry::BlockId, 3> floor,
                         VineBlocks vine)
        : width_(width), height_(height), max_height_(max_height), floor_(floor), vine_(vine) {}

    [[nodiscard]] std::string_view type_name() const override { return "twisting_vines"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto&  blocks = *context.blocks;
        const Reader read{blocks, level};
        const auto   invalid = [&](BlockPos pos) {
            if (!read.empty(pos)) {
                return true;
            }
            const registry::BlockId below = read.block(pos.below());
            return std::ranges::find(floor_, below) == floor_.end();
        };
        if (invalid(at)) {
            return false;
        }
        for (i32 i = 0; i < width_ * width_; ++i) {
            const i32 dx  = between(random, -width_, width_);
            const i32 dy  = between(random, -height_, height_);
            const i32 dz  = between(random, -width_, width_);
            BlockPos  pos = at.offset(dx, dy, dz);
            // Down to the first block that is not air, then one back up.
            bool grounded = true;
            do {
                pos = pos.below();
                if (pos.y < level.min_y() || pos.y > level.max_y()) {
                    grounded = false;
                    break;
                }
            } while (read.empty(pos));
            if (!grounded) {
                continue;
            }
            pos = pos.above();
            if (invalid(pos)) {
                continue;
            }
            i32 length = between(random, 1, max_height_);
            if (random.next_int(6) == 0) {
                length *= 2;
            }
            if (random.next_int(5) == 0) {
                length = 1;
            }
            vine_column(blocks, level, random, pos, length, 17, 25, vine_, 1);
        }
        return true;
    }

private:
    i32                              width_;
    i32                              height_;
    i32                              max_height_;
    std::array<registry::BlockId, 3> floor_;  // netherrack, warped nylium, warped wart block
    VineBlocks                       vine_;
};

// ── nether_forest_vegetation ────────────────────────────────────────────────

/// What a root, a sprout or a fungus may stand on.
struct NetherGround {
    std::vector<u16>  nylium;
    std::vector<u16>  dirt;
    registry::BlockId soul_soil{0};
    registry::BlockId farmland{0};
    registry::BlockId mycelium{0};
    registry::BlockId crimson_fungus{0};
    registry::BlockId warped_fungus{0};

    [[nodiscard]] bool supports(registry::BlockId plant, registry::BlockId below) const {
        if (holds(nylium, below) || below == soul_soil || holds(dirt, below) ||
            below == farmland) {
            return true;
        }
        // The fungi take mycelium as well; the roots and sprouts do not.
        return (plant == crimson_fungus || plant == warped_fungus) && below == mycelium;
    }
};

/// Roots, sprouts and fungi scattered over nylium: `spread_width²` tries, each
/// offset by paired draws, its state drawn before the position is tested.
class NetherForestVegetationFeature final : public Feature {
public:
    NetherForestVegetationFeature(StateProviderRef provider, i32 width, i32 height,
                                  NetherGround ground)
        : provider_(std::move(provider)), width_(width), height_(height),
          ground_(std::move(ground)) {}

    [[nodiscard]] std::string_view type_name() const override { return "nether_forest_vegetation"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto&  blocks = *context.blocks;
        const Reader read{blocks, level};
        if (!holds(ground_.nylium, read.block(at.below()))) {
            return false;
        }
        if (at.y < level.min_y() + 1 || at.y + 1 >= level.max_y() + 1) {
            return false;
        }
        i32 placed = 0;
        for (i32 i = 0; i < width_ * width_; ++i) {
            const i32      dx    = spread(random, width_);
            const i32      dy    = spread(random, height_);
            const i32      dz    = spread(random, width_);
            const BlockPos pos   = at.offset(dx, dy, dz);
            const auto     state = provider_->state(level, random, pos);
            if (read.empty(pos) && pos.y > level.min_y() &&
                ground_.supports(blocks.block_of(state), read.block(pos.below()))) {
                put(level, pos, state);
                ++placed;
            }
        }
        return placed > 0;
    }

private:
    StateProviderRef provider_;
    i32              width_;
    i32              height_;
    NetherGround     ground_;
};

// ── huge_fungus ─────────────────────────────────────────────────────────────

/// A crimson or warped fungus tree: a stem of 4 to 13 (doubled one time in
/// twelve), one in sixteen or so 3×3 and ragged, under a hat of wart with
/// shroomlights, weeping vines under the crimson hat.
class HugeFungusFeature final : public Feature {
public:
    struct Config {
        registry::BlockId      base{0};
        registry::BlockStateId stem{};
        registry::BlockStateId hat{};
        registry::BlockStateId decor{};
        BlockPredicateRef      replaceable_blocks;
        bool                   planted{false};
        std::vector<u16>       replaceable;  // #minecraft:replaceable
        bool                   hat_is_wart{false};
        VineBlocks             vine;
    };

    explicit HugeFungusFeature(Config config) : config_(std::move(config)) {}

    [[nodiscard]] std::string_view type_name() const override { return "huge_fungus"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto&  blocks = *context.blocks;
        const Reader read{blocks, level};
        if (read.block(at.below()) != config_.base) {
            return false;
        }
        i32 height = between(random, 4, 13);
        if (random.next_int(12) == 0) {
            height *= 2;
        }
        if (!config_.planted) {
            // The generator's depth, 128 in the Nether, not the level's 256.
            const i32 depth = level.world_height();
            if (at.y + height + 1 >= depth) {
                return false;
            }
        }
        const bool huge = !config_.planted && random.next_float() < 0.06F;
        put(level, at, registry::kAirState);
        stem(blocks, level, random, at, height, huge);
        hat(blocks, level, random, at, height, huge);
        return true;
    }

private:
    [[nodiscard]] bool replaceable(const registry::BlockRegistry& blocks, const FeatureLevel& level,
                                   BlockPos pos, bool check_config) const {
        const Reader read{blocks, level};
        if (holds(config_.replaceable, read.block(pos))) {
            return true;
        }
        return check_config && config_.replaceable_blocks &&
               config_.replaceable_blocks->test(level, pos);
    }

    void stem(const registry::BlockRegistry& blocks, FeatureLevel& level, FeatureRandom& random,
              BlockPos at, i32 height, bool huge) const {
        const i32 reach = huge ? 1 : 0;
        for (i32 j = -reach; j <= reach; ++j) {
            for (i32 k = -reach; k <= reach; ++k) {
                const bool corner = huge && std::abs(j) == reach && std::abs(k) == reach;
                for (i32 l = 0; l < height; ++l) {
                    const BlockPos pos = at.offset(j, l, k);
                    if (!replaceable(blocks, level, pos, true)) {
                        continue;
                    }
                    if (corner) {
                        if (random.next_float() < 0.1F) {
                            put(level, pos, config_.stem);
                        }
                    } else {
                        put(level, pos, config_.stem);
                    }
                }
            }
        }
    }

    void hat(const registry::BlockRegistry& blocks, FeatureLevel& level, FeatureRandom& random,
             BlockPos at, i32 height, bool huge) const {
        const bool wart      = config_.hat_is_wart;
        const i32  hat_depth = std::min(random.next_int(1 + height / 3) + 5, height);
        const i32  start     = height - hat_depth;
        for (i32 k = start; k <= height; ++k) {
            i32 reach = k < height - random.next_int(3) ? 2 : 1;
            if (hat_depth > 8 && k < start + 4) {
                reach = 3;
            }
            if (huge) {
                ++reach;
            }
            for (i32 i = -reach; i <= reach; ++i) {
                for (i32 j = -reach; j <= reach; ++j) {
                    const bool     edge_x = i == -reach || i == reach;
                    const bool     edge_z = j == -reach || j == reach;
                    const bool     inner  = !edge_x && !edge_z && k != height;
                    const bool     corner = edge_x && edge_z;
                    const bool     low    = k < start + 3;
                    const BlockPos pos    = at.offset(i, k, j);
                    if (!replaceable(blocks, level, pos, false)) {
                        continue;
                    }
                    if (low) {
                        if (!inner) {
                            drop_block(blocks, level, random, pos, wart);
                        }
                    } else if (inner) {
                        hat_block(blocks, level, random, pos, 0.1F, 0.2F, wart ? 0.1F : 0.0F);
                    } else if (corner) {
                        hat_block(blocks, level, random, pos, 0.01F, 0.7F, wart ? 0.083F : 0.0F);
                    } else {
                        hat_block(blocks, level, random, pos, 5.0E-4F, 0.98F,
                                  wart ? 0.07F : 0.0F);
                    }
                }
            }
        }
    }

    void hat_block(const registry::BlockRegistry& blocks, FeatureLevel& level,
                   FeatureRandom& random, BlockPos pos, f32 decor, f32 hat, f32 vine) const {
        if (random.next_float() < decor) {
            put(level, pos, config_.decor);
        } else if (random.next_float() < hat) {
            put(level, pos, config_.hat);
            if (random.next_float() < vine) {
                hang_vines(blocks, level, random, pos);
            }
        }
    }

    void drop_block(const registry::BlockRegistry& blocks, FeatureLevel& level,
                    FeatureRandom& random, BlockPos pos, bool vines) const {
        const Reader read{blocks, level};
        if (read.block(pos.below()) == blocks.block_of(config_.hat)) {
            put(level, pos, config_.hat);
            return;
        }
        // A float drawn and compared as a double: 0.15, not 0.15F.
        if (static_cast<f64>(random.next_float()) < 0.15) {
            put(level, pos, config_.hat);
            if (vines && random.next_int(11) == 0) {
                hang_vines(blocks, level, random, pos);
            }
        }
    }

    void hang_vines(const registry::BlockRegistry& blocks, FeatureLevel& level,
                    FeatureRandom& random, BlockPos pos) const {
        const Reader   read{blocks, level};
        const BlockPos below = pos.below();
        if (!read.empty(below)) {
            return;
        }
        i32 length = between(random, 1, 5);
        if (random.next_int(7) == 0) {
            length *= 2;
        }
        vine_column(blocks, level, random, below, length, 23, 25, config_.vine, -1);
    }

    Config config_;
};

// ── basalt_columns ──────────────────────────────────────────────────────────

/// What a column may not stand on, nor grow through.
struct ColumnBlockers {
    std::vector<u16>  cannot_place_on;  // sorted
    registry::BlockId lava{0};
    registry::BlockId basalt{0};
};

class BasaltColumnsFeature final : public Feature {
public:
    BasaltColumnsFeature(IntProviderRef height, IntProviderRef reach, registry::BlockStateId basalt,
                         ColumnBlockers blockers)
        : height_(std::move(height)), reach_(std::move(reach)), basalt_(basalt),
          blockers_(std::move(blockers)) {}

    [[nodiscard]] std::string_view type_name() const override { return "basalt_columns"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto& blocks    = *context.blocks;
        const i32   sea_level = level.sea_level();
        if (!can_place_at(blocks, level, sea_level, at)) {
            return false;
        }
        const i32  height = height_->sample(random);
        const bool small  = random.next_float() < 0.9F;
        const i32  reach  = std::min(height, small ? 5 : 8);
        const i32  tries  = small ? 50 : 15;
        bool       placed = false;
        // `BlockPos.randomBetweenClosed`: each position drawn x, y, z (a
        // one-block-tall box still draws its y), lazily, between the bodies.
        const i32 span = reach * 2 + 1;
        for (i32 n = 0; n < tries; ++n) {
            const i32      rx  = random.next_int(span);
            const i32      ry  = random.next_int(1);
            const i32      rz  = random.next_int(span);
            const BlockPos pos{at.x - reach + rx, at.y + ry, at.z - reach + rz};
            const i32      left = height - manhattan(pos, at);
            if (left >= 0) {
                const i32 column_reach = reach_->sample(random);
                placed |= column(blocks, level, sea_level, pos, left, column_reach);
            }
        }
        return placed;
    }

private:
    [[nodiscard]] static i32 manhattan(BlockPos a, BlockPos b) {
        return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
    }

    [[nodiscard]] bool air_or_lava_ocean(const registry::BlockRegistry& blocks,
                                         const FeatureLevel& level, i32 sea_level,
                                         BlockPos pos) const {
        const Reader            read{blocks, level};
        const registry::BlockId block = read.block(pos);
        return blocks.is_air(block) || (block == blockers_.lava && pos.y <= sea_level);
    }

    [[nodiscard]] bool can_place_at(const registry::BlockRegistry& blocks,
                                    const FeatureLevel& level, i32 sea_level, BlockPos pos) const {
        if (!air_or_lava_ocean(blocks, level, sea_level, pos)) {
            return false;
        }
        const Reader            read{blocks, level};
        const registry::BlockId below = read.block(pos.below());
        return !blocks.is_air(below) && !holds(blockers_.cannot_place_on, below);
    }

    bool column(const registry::BlockRegistry& blocks, FeatureLevel& level, i32 sea_level,
                BlockPos pos, i32 height, i32 reach) const {
        const Reader read{blocks, level};
        bool         placed = false;
        // `BlockPos.betweenClosed`: x fastest, then (a single) y, then z.
        for (i32 z = pos.z - reach; z <= pos.z + reach; ++z) {
            for (i32 x = pos.x - reach; x <= pos.x + reach; ++x) {
                const BlockPos p{x, pos.y, z};
                const i32      distance = manhattan(p, pos);
                std::optional<BlockPos> found;
                if (air_or_lava_ocean(blocks, level, sea_level, p)) {
                    // Down to a surface, at most `distance` steps.
                    BlockPos probe = p;
                    i32      left  = distance;
                    while (probe.y > level.min_y() + 1 && left > 0) {
                        --left;
                        if (can_place_at(blocks, level, sea_level, probe)) {
                            found = probe;
                            break;
                        }
                        probe = probe.below();
                    }
                } else {
                    // Up to air, at most `distance` steps, never through a
                    // blocker.
                    BlockPos probe = p;
                    i32      left  = distance;
                    while (probe.y < level.max_y() + 1 && left > 0) {
                        --left;
                        const registry::BlockId block = read.block(probe);
                        if (holds(blockers_.cannot_place_on, block)) {
                            break;
                        }
                        if (blocks.is_air(block)) {
                            found = probe;
                            break;
                        }
                        probe = probe.above();
                    }
                }
                if (!found) {
                    continue;
                }
                BlockPos m = *found;
                for (i32 j = height - distance / 2; j >= 0; --j) {
                    if (air_or_lava_ocean(blocks, level, sea_level, m)) {
                        put(level, m, basalt_);
                        m      = m.above();
                        placed = true;
                    } else {
                        if (read.block(m) != blockers_.basalt) {
                            break;
                        }
                        m = m.above();
                    }
                }
            }
        }
        return placed;
    }

    IntProviderRef         height_;
    IntProviderRef         reach_;
    registry::BlockStateId basalt_;
    ColumnBlockers         blockers_;
};

// ── basalt_pillar ───────────────────────────────────────────────────────────

class BasaltPillarFeature final : public Feature {
public:
    explicit BasaltPillarFeature(registry::BlockStateId basalt) : basalt_(basalt) {}

    [[nodiscard]] std::string_view type_name() const override { return "basalt_pillar"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto&  blocks = *context.blocks;
        const Reader read{blocks, level};
        if (!read.empty(at) || read.empty(at.above())) {
            return false;
        }
        BlockPos pos   = at;
        bool     north = true;
        bool     south = true;
        bool     west  = true;
        bool     east  = true;
        // Down from the roof to the floor, a ragged skirt of hang-offs on each
        // side until a side's first miss.
        while (read.empty(pos)) {
            if (level.outside_build_height(pos.y)) {
                return true;
            }
            put(level, pos, basalt_);
            north = north && hang_off(level, random, pos.offset(0, 0, -1));
            south = south && hang_off(level, random, pos.offset(0, 0, 1));
            west  = west && hang_off(level, random, pos.offset(-1, 0, 0));
            east  = east && hang_off(level, random, pos.offset(1, 0, 0));
            pos   = pos.below();
        }
        pos = pos.above();
        base_hang_off(level, random, pos.offset(0, 0, -1));
        base_hang_off(level, random, pos.offset(0, 0, 1));
        base_hang_off(level, random, pos.offset(-1, 0, 0));
        base_hang_off(level, random, pos.offset(1, 0, 0));
        pos = pos.below();
        // A spill at the foot, thinning with distance from the pillar.
        for (i32 i = -3; i < 4; ++i) {
            for (i32 j = -3; j < 4; ++j) {
                const i32 k = std::abs(i) * std::abs(j);
                if (random.next_int(10) >= 10 - k) {
                    continue;
                }
                BlockPos spill = pos.offset(i, 0, j);
                i32      steps = 3;
                while (read.empty(spill.below())) {
                    spill = spill.below();
                    --steps;
                    if (steps <= 0) {
                        break;
                    }
                }
                if (!read.empty(spill.below())) {
                    put(level, spill, basalt_);
                }
            }
        }
        return true;
    }

private:
    void base_hang_off(FeatureLevel& level, FeatureRandom& random, BlockPos pos) const {
        if (random.next_boolean()) {
            put(level, pos, basalt_);
        }
    }

    [[nodiscard]] bool hang_off(FeatureLevel& level, FeatureRandom& random, BlockPos pos) const {
        if (random.next_int(10) != 0) {
            put(level, pos, basalt_);
            return true;
        }
        return false;
    }

    registry::BlockStateId basalt_;
};

// ── delta_feature ───────────────────────────────────────────────────────────

class DeltaFeature final : public Feature {
public:
    DeltaFeature(registry::BlockStateId contents, registry::BlockStateId rim, IntProviderRef size,
                 IntProviderRef rim_size, std::vector<u16> cannot_replace)
        : contents_(contents), rim_(rim), size_(std::move(size)), rim_size_(std::move(rim_size)),
          cannot_replace_(std::move(cannot_replace)) {}

    [[nodiscard]] std::string_view type_name() const override { return "delta_feature"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto& blocks  = *context.blocks;
        bool        placed  = false;
        const bool  rimmed  = random.next_double() < 0.9;
        const i32   rim_x   = rimmed ? rim_size_->sample(random) : 0;
        const i32   rim_z   = rimmed ? rim_size_->sample(random) : 0;
        const bool  has_rim = rimmed && rim_x != 0 && rim_z != 0;
        const i32   size_x  = size_->sample(random);
        const i32   size_z  = size_->sample(random);
        const i32   limit   = std::max(size_x, size_z);
        ManhattanWalk walk{at, size_x, 0, size_z};
        BlockPos      pos;
        while (walk.next(pos)) {
            if (std::abs(pos.x - at.x) + std::abs(pos.y - at.y) + std::abs(pos.z - at.z) > limit) {
                break;
            }
            if (!clear(blocks, level, pos)) {
                continue;
            }
            if (has_rim) {
                placed = true;
                put(level, pos, rim_);
            }
            const BlockPos inner = pos.offset(rim_x, 0, rim_z);
            if (clear(blocks, level, inner)) {
                placed = true;
                put(level, inner, contents_);
            }
        }
        return placed;
    }

private:
    /// Not the contents, not a protected block, air above and nothing but
    /// solid on the other five sides.
    [[nodiscard]] bool clear(const registry::BlockRegistry& blocks, const FeatureLevel& level,
                             BlockPos pos) const {
        const Reader            read{blocks, level};
        const registry::BlockId here = read.block(pos);
        if (here == blocks.block_of(contents_) || holds(cannot_replace_, here)) {
            return false;
        }
        for (usize d = 0; d < kDirections.size(); ++d) {
            const auto& o   = kDirections[d];
            const bool  air = blocks.is_air(read.block(pos.offset(o[0], o[1], o[2])));
            const bool  up  = d == 1;
            if ((air && !up) || (!air && up)) {
                return false;
            }
        }
        return true;
    }

    registry::BlockStateId contents_;
    registry::BlockStateId rim_;
    IntProviderRef         size_;
    IntProviderRef         rim_size_;
    std::vector<u16>       cannot_replace_;
};

// ── netherrack_replace_blobs ────────────────────────────────────────────────

class ReplaceBlobsFeature final : public Feature {
public:
    ReplaceBlobsFeature(registry::BlockId target, registry::BlockStateId state, IntProviderRef radius)
        : target_(target), state_(state), radius_(std::move(radius)) {}

    [[nodiscard]] std::string_view type_name() const override {
        return "netherrack_replace_blobs";
    }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto&  blocks = *context.blocks;
        const Reader read{blocks, level};
        // The origin clamped into the level, then down to the first target.
        BlockPos pos = at;
        pos.y        = std::clamp(pos.y, level.min_y() + 1, level.max_y());
        bool found   = false;
        while (pos.y > level.min_y() + 1) {
            if (read.block(pos) == target_) {
                found = true;
                break;
            }
            pos = pos.below();
        }
        if (!found) {
            return false;
        }
        const i32     rx    = radius_->sample(random);
        const i32     ry    = radius_->sample(random);
        const i32     rz    = radius_->sample(random);
        const i32     limit = std::max(rx, std::max(ry, rz));
        bool          placed = false;
        ManhattanWalk walk{pos, rx, ry, rz};
        BlockPos      p;
        while (walk.next(p)) {
            if (std::abs(p.x - pos.x) + std::abs(p.y - pos.y) + std::abs(p.z - pos.z) > limit) {
                break;
            }
            if (read.block(p) == target_) {
                put(level, p, state_);
                placed = true;
            }
        }
        return placed;
    }

private:
    registry::BlockId      target_;
    registry::BlockStateId state_;
    IntProviderRef         radius_;
};

// ── Parsing ─────────────────────────────────────────────────────────────────

[[nodiscard]] std::expected<i32, FeatureError> int_field(Json config, std::string_view key) {
    i64 value = 0;
    if (config.at_key(key).get(value) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    return static_cast<i32>(value);
}

[[nodiscard]] std::expected<std::vector<u16>, FeatureError> block_set(
    const registry::BlockRegistry& blocks, std::initializer_list<std::string_view> names) {
    std::vector<u16> set;
    for (const std::string_view name : names) {
        auto block = named_block(blocks, name);
        if (!block) {
            return std::unexpected(block.error());
        }
        set.push_back(block->value());
    }
    std::ranges::sort(set);
    return set;
}

[[nodiscard]] std::expected<VineBlocks, FeatureError> vine_blocks(
    const registry::BlockRegistry& blocks, std::string_view head, std::string_view plant) {
    auto h = default_of(blocks, head);
    auto p = default_of(blocks, plant);
    if (!h) return std::unexpected(h.error());
    if (!p) return std::unexpected(p.error());
    return VineBlocks{*h, *p};
}

[[nodiscard]] std::expected<std::array<registry::BlockId, 3>, FeatureError> three_blocks(
    const registry::BlockRegistry& blocks, std::string_view a, std::string_view b,
    std::string_view c) {
    auto x = named_block(blocks, a);
    auto y = named_block(blocks, b);
    auto z = named_block(blocks, c);
    if (!x) return std::unexpected(x.error());
    if (!y) return std::unexpected(y.error());
    if (!z) return std::unexpected(z.error());
    return std::array<registry::BlockId, 3>{*x, *y, *z};
}

template <class T, class... Args>
[[nodiscard]] ClaimedFeature built(Args&&... args) {
    return std::expected<FeatureRef, FeatureError>{
        std::static_pointer_cast<const Feature>(std::make_shared<const T>(std::forward<Args>(args)...))};
}

}  // namespace

ClaimedFeature parse_nether_feature(std::string_view kind, Json config,
                                    const registry::BlockRegistry& blocks, const BlockTags& tags) {
    // `OV_NETHER_FEATURES=0` refuses the nine again, by name: the "before" arm
    // of the parity measurement, from the same binary (nether-2.md § 1).
    static const bool enabled = [] {
        const char* setting = std::getenv("OV_NETHER_FEATURES");
        return setting == nullptr || std::string_view{setting} != "0";
    }();
    if (!enabled && (kind == "glowstone_blob" || kind == "weeping_vines" ||
                     kind == "twisting_vines" || kind == "nether_forest_vegetation" ||
                     kind == "huge_fungus" || kind == "basalt_columns" ||
                     kind == "basalt_pillar" || kind == "delta_feature" ||
                     kind == "netherrack_replace_blobs")) {
        OV_LOG_ERROR("worldgen: {} is switched off (OV_NETHER_FEATURES=0)", kind);
        return std::unexpected(FeatureError::Unsupported);
    }
    if (kind == "glowstone_blob") {
        auto glowstone = default_of(blocks, "minecraft:glowstone");
        auto roof = three_blocks(blocks, "minecraft:netherrack", "minecraft:basalt",
                                 "minecraft:blackstone");
        if (!glowstone) return std::unexpected(glowstone.error());
        if (!roof) return std::unexpected(roof.error());
        return built<GlowstoneBlobFeature>(*glowstone, *roof);
    }
    if (kind == "weeping_vines") {
        auto wart       = default_of(blocks, "minecraft:nether_wart_block");
        auto netherrack = named_block(blocks, "minecraft:netherrack");
        auto vine = vine_blocks(blocks, "minecraft:weeping_vines", "minecraft:weeping_vines_plant");
        if (!wart) return std::unexpected(wart.error());
        if (!netherrack) return std::unexpected(netherrack.error());
        if (!vine) return std::unexpected(vine.error());
        return built<WeepingVinesFeature>(*wart, *netherrack, *vine);
    }
    if (kind == "twisting_vines") {
        auto width  = int_field(config, "spread_width");
        auto height = int_field(config, "spread_height");
        auto top    = int_field(config, "max_height");
        auto floor  = three_blocks(blocks, "minecraft:netherrack", "minecraft:warped_nylium",
                                   "minecraft:warped_wart_block");
        auto vine = vine_blocks(blocks, "minecraft:twisting_vines", "minecraft:twisting_vines_plant");
        if (!width || !height || !top) return std::unexpected(FeatureError::Malformed);
        if (!floor) return std::unexpected(floor.error());
        if (!vine) return std::unexpected(vine.error());
        return built<TwistingVinesFeature>(*width, *height, *top, *floor, *vine);
    }
    if (kind == "nether_forest_vegetation") {
        auto width  = int_field(config, "spread_width");
        auto height = int_field(config, "spread_height");
        if (!width || !height) return std::unexpected(FeatureError::Malformed);
        auto field = config.at_key("state_provider");
        if (field.error() != simdjson::SUCCESS) return std::unexpected(FeatureError::Malformed);
        auto provider = parse_state_provider(field.value(), blocks, tags);
        if (!provider) return std::unexpected(provider.error());
        NetherGround ground;
        auto nylium = tag_members(blocks, tags, "minecraft:nylium");
        auto dirt   = tag_members(blocks, tags, "minecraft:dirt");
        auto soul   = named_block(blocks, "minecraft:soul_soil");
        auto farm   = named_block(blocks, "minecraft:farmland");
        auto myc    = named_block(blocks, "minecraft:mycelium");
        auto crim   = named_block(blocks, "minecraft:crimson_fungus");
        auto warp   = named_block(blocks, "minecraft:warped_fungus");
        if (!nylium) return std::unexpected(nylium.error());
        if (!dirt) return std::unexpected(dirt.error());
        if (!soul || !farm || !myc || !crim || !warp) return std::unexpected(FeatureError::Missing);
        ground.nylium         = std::move(*nylium);
        ground.dirt           = std::move(*dirt);
        ground.soul_soil      = *soul;
        ground.farmland       = *farm;
        ground.mycelium       = *myc;
        ground.crimson_fungus = *crim;
        ground.warped_fungus  = *warp;
        // Every state the provider can give must be a plant this rule speaks
        // for; anything else would be placed by a rule that is not its own.
        for (const registry::BlockStateId state : (*provider)->possible_states()) {
            const std::string_view name = blocks.block_name(blocks.block_of(state));
            if (name != "minecraft:crimson_roots" && name != "minecraft:warped_roots" &&
                name != "minecraft:nether_sprouts" && name != "minecraft:crimson_fungus" &&
                name != "minecraft:warped_fungus") {
                OV_LOG_ERROR("worldgen: nether_forest_vegetation has no survival rule for {}",
                             name);
                return std::unexpected(FeatureError::Unsupported);
            }
        }
        return built<NetherForestVegetationFeature>(std::move(*provider), *width, *height,
                                                    std::move(ground));
    }
    if (kind == "huge_fungus") {
        HugeFungusFeature::Config fungus;
        auto base  = config.at_key("valid_base_block");
        auto stem  = config.at_key("stem_state");
        auto hat   = config.at_key("hat_state");
        auto decor = config.at_key("decor_state");
        auto repl  = config.at_key("replaceable_blocks");
        if (base.error() != simdjson::SUCCESS || stem.error() != simdjson::SUCCESS ||
            hat.error() != simdjson::SUCCESS || decor.error() != simdjson::SUCCESS ||
            repl.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto base_state  = parse_block_state(base.value(), blocks);
        auto stem_state  = parse_block_state(stem.value(), blocks);
        auto hat_state   = parse_block_state(hat.value(), blocks);
        auto decor_state = parse_block_state(decor.value(), blocks);
        auto predicate   = parse_block_predicate(repl.value(), blocks, tags);
        auto replaceable = tag_members(blocks, tags, "minecraft:replaceable");
        auto vine = vine_blocks(blocks, "minecraft:weeping_vines", "minecraft:weeping_vines_plant");
        if (!base_state) return std::unexpected(base_state.error());
        if (!stem_state) return std::unexpected(stem_state.error());
        if (!hat_state) return std::unexpected(hat_state.error());
        if (!decor_state) return std::unexpected(decor_state.error());
        if (!predicate) return std::unexpected(predicate.error());
        if (!replaceable) return std::unexpected(replaceable.error());
        if (!vine) return std::unexpected(vine.error());
        bool planted = false;
        (void)config.at_key("planted").get(planted);
        fungus.base               = blocks.block_of(*base_state);
        fungus.stem               = *stem_state;
        fungus.hat                = *hat_state;
        fungus.decor              = *decor_state;
        fungus.replaceable_blocks = std::move(*predicate);
        fungus.planted            = planted;
        fungus.replaceable        = std::move(*replaceable);
        fungus.hat_is_wart =
            blocks.block_name(blocks.block_of(*hat_state)) == "minecraft:nether_wart_block";
        fungus.vine = *vine;
        if (planted) {
            // A planted fungus breaks what stands under its stem and hat, with
            // drops — a gameplay write a FeatureLevel cannot make.
            OV_LOG_ERROR("worldgen: a planted huge_fungus destroys blocks with drops; not built");
            return std::unexpected(FeatureError::Unsupported);
        }
        return built<HugeFungusFeature>(std::move(fungus));
    }
    if (kind == "basalt_columns") {
        auto height = config.at_key("height");
        auto reach  = config.at_key("reach");
        if (height.error() != simdjson::SUCCESS || reach.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto h      = parse_int_provider(height.value());
        auto r      = parse_int_provider(reach.value());
        auto basalt = default_of(blocks, "minecraft:basalt");
        auto lava   = named_block(blocks, "minecraft:lava");
        auto set    = block_set(blocks, {"minecraft:lava", "minecraft:bedrock",
                                         "minecraft:magma_block", "minecraft:soul_sand",
                                         "minecraft:nether_bricks", "minecraft:nether_brick_fence",
                                         "minecraft:nether_brick_stairs", "minecraft:nether_wart",
                                         "minecraft:chest", "minecraft:spawner"});
        if (!h) return std::unexpected(h.error());
        if (!r) return std::unexpected(r.error());
        if (!basalt) return std::unexpected(basalt.error());
        if (!lava) return std::unexpected(lava.error());
        if (!set) return std::unexpected(set.error());
        ColumnBlockers blockers{std::move(*set), *lava, blocks.block_of(*basalt)};
        return built<BasaltColumnsFeature>(std::move(*h), std::move(*r), *basalt,
                                           std::move(blockers));
    }
    if (kind == "basalt_pillar") {
        auto basalt = default_of(blocks, "minecraft:basalt");
        if (!basalt) return std::unexpected(basalt.error());
        return built<BasaltPillarFeature>(*basalt);
    }
    if (kind == "delta_feature") {
        auto contents = config.at_key("contents");
        auto rim      = config.at_key("rim");
        auto size     = config.at_key("size");
        auto rim_size = config.at_key("rim_size");
        if (contents.error() != simdjson::SUCCESS || rim.error() != simdjson::SUCCESS ||
            size.error() != simdjson::SUCCESS || rim_size.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto c  = parse_block_state(contents.value(), blocks);
        auto r  = parse_block_state(rim.value(), blocks);
        auto s  = parse_int_provider(size.value());
        auto rs = parse_int_provider(rim_size.value());
        auto set = block_set(blocks, {"minecraft:bedrock", "minecraft:nether_bricks",
                                      "minecraft:nether_brick_fence",
                                      "minecraft:nether_brick_stairs", "minecraft:nether_wart",
                                      "minecraft:chest", "minecraft:spawner"});
        if (!c) return std::unexpected(c.error());
        if (!r) return std::unexpected(r.error());
        if (!s) return std::unexpected(s.error());
        if (!rs) return std::unexpected(rs.error());
        if (!set) return std::unexpected(set.error());
        return built<DeltaFeature>(*c, *r, std::move(*s), std::move(*rs), std::move(*set));
    }
    if (kind == "netherrack_replace_blobs") {
        auto target = config.at_key("target");
        auto state  = config.at_key("state");
        auto radius = config.at_key("radius");
        if (target.error() != simdjson::SUCCESS || state.error() != simdjson::SUCCESS ||
            radius.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto t = parse_block_state(target.value(), blocks);
        auto s = parse_block_state(state.value(), blocks);
        auto r = parse_int_provider(radius.value());
        if (!t) return std::unexpected(t.error());
        if (!s) return std::unexpected(s.error());
        if (!r) return std::unexpected(r.error());
        return built<ReplaceBlobsFeature>(blocks.block_of(*t), *s, std::move(*r));
    }
    return std::nullopt;
}

}  // namespace ov::worldgen
