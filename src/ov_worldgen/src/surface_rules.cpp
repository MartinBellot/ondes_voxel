#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/surface_rules.hpp"

#include "ov/base/log.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ov::worldgen {

namespace {

using Json = simdjson::dom::element;

/// Vanilla's linear remap, spelled out rather than reached for: it appears in
/// `vertical_gradient` and in `stone_depth`'s secondary range, and both are
/// places where a swapped endpoint produces a world that still looks like a
/// world.
[[nodiscard]] constexpr f64 map_range(f64 value, f64 from_low, f64 from_high, f64 to_low,
                                      f64 to_high) noexcept {
    return to_low + (value - from_low) * (to_high - to_low) / (from_high - from_low);
}

// ── Rules ───────────────────────────────────────────────────────────────────

/// The first rule of the list that claims the position wins.
///
/// Not "the last", and the difference is the whole file: the overworld's top
/// level is bedrock, then the surface, then deepslate, and reversing it buries
/// the world in deepslate.
class Sequence final : public SurfaceRule {
public:
    explicit Sequence(std::vector<SurfaceRuleRef> rules) : rules_(std::move(rules)) {}

    [[nodiscard]] std::optional<registry::BlockStateId> apply(
        const SurfaceContext& at) const override {
        for (const auto& rule : rules_) {
            if (auto result = rule->apply(at)) {
                return result;
            }
        }
        return std::nullopt;
    }

private:
    std::vector<SurfaceRuleRef> rules_;
};

class Conditional final : public SurfaceRule {
public:
    Conditional(std::shared_ptr<const SurfaceCondition> condition, SurfaceRuleRef then_run)
        : condition_(std::move(condition)), then_run_(std::move(then_run)) {}

    [[nodiscard]] std::optional<registry::BlockStateId> apply(
        const SurfaceContext& at) const override {
        return condition_->test(at) ? then_run_->apply(at) : std::nullopt;
    }

private:
    std::shared_ptr<const SurfaceCondition> condition_;
    SurfaceRuleRef                          then_run_;
};

/// A fixed block. The leaf of every branch, and the only rule that ever
/// answers.
class BlockRule final : public SurfaceRule {
public:
    explicit BlockRule(registry::BlockStateId state) : state_(state) {}

    [[nodiscard]] std::optional<registry::BlockStateId> apply(const SurfaceContext&) const override {
        return state_;
    }

private:
    registry::BlockStateId state_;
};

/// The badlands: 192 bands of coloured terracotta, read by height.
class Bandlands final : public SurfaceRule {
public:
    explicit Bandlands(const SurfaceResources& resources) : resources_(&resources) {}

    [[nodiscard]] std::optional<registry::BlockStateId> apply(
        const SurfaceContext& at) const override {
        return resources_->clay_band(at.x, at.y, at.z);
    }

private:
    const SurfaceResources* resources_;
};

// ── Conditions ──────────────────────────────────────────────────────────────

class BiomeCondition final : public SurfaceCondition {
public:
    explicit BiomeCondition(std::vector<std::string> names) : names_(std::move(names)) {
        // Sorted so the test is a binary search. The longest list in the
        // overworld has 30 entries and the condition is asked about a hundred
        // thousand times per chunk.
        std::ranges::sort(names_);
    }

    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        const std::string_view here = at.queries->biome_at(at.x, at.y, at.z);
        return std::ranges::binary_search(names_, here);
    }

private:
    std::vector<std::string> names_;
};

/// One named noise, sampled in two dimensions.
///
/// y is not read: the sample is taken at y = 0 whatever the position's height
/// is, which is what makes a gravel patch a patch rather than a column.
class NoiseThreshold final : public SurfaceCondition {
public:
    NoiseThreshold(const NormalNoise& noise, f64 low, f64 high)
        : noise_(&noise), low_(low), high_(high) {}

    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        const f64 value =
            noise_->value(static_cast<f64>(at.x), 0.0, static_cast<f64>(at.z));
        return value >= low_ && value <= high_;
    }

private:
    const NormalNoise* noise_;
    f64                low_;
    f64                high_;
};

/// How a height is written in the datapack.
enum class AnchorKind : u8 { Absolute, AboveBottom, BelowTop };

struct Anchor {
    AnchorKind kind{AnchorKind::Absolute};
    i32        value{0};

    [[nodiscard]] i32 resolve(i32 min_y, i32 height) const noexcept {
        switch (kind) {
            case AnchorKind::Absolute:
                return resolve_anchor_absolute(value);
            case AnchorKind::AboveBottom:
                return resolve_anchor_above_bottom(value, min_y);
            case AnchorKind::BelowTop:
                return resolve_anchor_below_top(value, min_y, height);
        }
        return value;
    }
};

/// A band between two heights where the answer is random, and certain outside
/// it.
///
/// This is the bedrock floor: always bedrock at the very bottom, never bedrock
/// five blocks up, and in between a chance that falls off linearly. The draw is
/// positional — seeded from x, y and z — so the same world always has the same
/// ragged floor, and so a single block can be checked against the real game
/// without generating anything around it.
class VerticalGradient final : public SurfaceCondition {
public:
    VerticalGradient(PositionalRandomFactory factory, Anchor true_at_and_below,
                     Anchor false_at_and_above)
        : factory_(factory), below_(true_at_and_below), above_(false_at_and_above) {}

    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        const i32 low  = below_.resolve(at.min_y, at.height);
        const i32 high = above_.resolve(at.min_y, at.height);
        if (at.y <= low) {
            return true;
        }
        if (at.y >= high) {
            return false;
        }
        const f64 chance = map_range(static_cast<f64>(at.y), static_cast<f64>(low),
                                     static_cast<f64>(high), 1.0, 0.0);
        return static_cast<f64>(factory_.next_float_at(at.x, at.y, at.z)) < chance;
    }

private:
    PositionalRandomFactory factory_;
    Anchor                  below_;
    Anchor                  above_;
};

class YAbove final : public SurfaceCondition {
public:
    YAbove(Anchor anchor, i32 surface_depth_multiplier, bool add_stone_depth)
        : anchor_(anchor),
          multiplier_(surface_depth_multiplier),
          add_stone_depth_(add_stone_depth) {}

    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        const i32 here      = at.y + (add_stone_depth_ ? at.stone_depth_above : 0);
        const i32 threshold = anchor_.resolve(at.min_y, at.height) + at.surface_depth * multiplier_;
        return here >= threshold;
    }

private:
    Anchor anchor_;
    i32    multiplier_;
    bool   add_stone_depth_;
};

/// Where this position sits relative to the water standing over it.
///
/// True when there is no water at all, which is what makes the land rules the
/// default rather than a special case.
class WaterCondition final : public SurfaceCondition {
public:
    WaterCondition(i32 offset, i32 surface_depth_multiplier, bool add_stone_depth)
        : offset_(offset),
          multiplier_(surface_depth_multiplier),
          add_stone_depth_(add_stone_depth) {}

    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        if (at.water_height == kNoWater) {
            return true;
        }
        const i32 here = at.y + (add_stone_depth_ ? at.stone_depth_above : 0);
        return here >= at.water_height + offset_ + at.surface_depth * multiplier_;
    }

private:
    i32  offset_;
    i32  multiplier_;
    bool add_stone_depth_;
};

/// Cold enough to snow.
///
/// The threshold is 0.15 and it is the biome's temperature *at this position*,
/// not the biome's nominal one — see SurfaceSystem, which says exactly how far
/// that adjustment is reproduced.
class TemperatureCondition final : public SurfaceCondition {
public:
    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        return at.queries->temperature_at(at.x, at.y, at.z) < 0.15;
    }
};

/// A slope steep enough that soil would not sit on it.
///
/// Vanilla compares the surface height of the two columns one step away in z
/// and nothing else — not x, and not symmetrically. That asymmetry is not a
/// simplification made here: a symmetric test puts grass on cliff faces that
/// the real game leaves as bare stone, and the parity harness says so. The
/// neighbours are clamped inside the chunk, so a chunk edge is never steep
/// across it.
class SteepCondition final : public SurfaceCondition {
public:
    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        const i32 base_z  = at.z & ~15;
        const i32 local_z = at.z & 15;
        const i32 nearer  = base_z + std::max(local_z - 1, 0);
        const i32 further = base_z + std::min(local_z + 1, 15);
        const i32 low     = at.queries->surface_height(at.x, nearer);
        const i32 high    = at.queries->surface_height(at.x, further);
        return high >= low + 4;
    }
};

class NotCondition final : public SurfaceCondition {
public:
    explicit NotCondition(std::shared_ptr<const SurfaceCondition> inner)
        : inner_(std::move(inner)) {}

    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        return !inner_->test(at);
    }

private:
    std::shared_ptr<const SurfaceCondition> inner_;
};

/// A column the surface noise made too thin to hold soil.
class HoleCondition final : public SurfaceCondition {
public:
    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        return at.surface_depth <= 0;
    }
};

/// Above the height the terrain reached before the surface stage.
///
/// This is what keeps the whole surface branch out of the deep stone: every
/// rule that puts grass, sand, gravel or terracotta sits under it, so getting
/// it wrong either buries the world in dirt or leaves it bare.
class AbovePreliminarySurface final : public SurfaceCondition {
public:
    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        const i32 level = at.queries->preliminary_surface(at.x, at.z);
        return level != kNoSurface && at.y >= level;
    }
};

enum class StoneDepthSide : u8 { Floor, Ceiling };

/// How far into the stone this position is.
///
/// `floor` counts down from the top of a stone run and `ceiling` up from its
/// bottom, which is why the same rule file can describe both a grass surface
/// and the underside of a cave roof.
class StoneDepth final : public SurfaceCondition {
public:
    StoneDepth(StoneDepthSide side, i32 offset, bool add_surface_depth,
               i32 secondary_depth_range)
        : side_(side),
          offset_(offset),
          add_surface_depth_(add_surface_depth),
          secondary_range_(secondary_depth_range) {}

    [[nodiscard]] bool test(const SurfaceContext& at) const override {
        const i32 depth =
            side_ == StoneDepthSide::Floor ? at.stone_depth_above : at.stone_depth_below;
        const i32 surface = add_surface_depth_ ? at.surface_depth : 0;
        const i32 secondary =
            secondary_range_ == 0
                ? 0
                : static_cast<i32>(map_range(at.secondary_depth, -1.0, 1.0, 0.0,
                                             static_cast<f64>(secondary_range_)));
        return depth <= 1 + offset_ + surface + secondary;
    }

private:
    StoneDepthSide side_;
    i32            offset_;
    bool           add_surface_depth_;
    i32            secondary_range_;
};

// ── Parsing ─────────────────────────────────────────────────────────────────

/// "minecraft:sequence" -> "sequence".
[[nodiscard]] std::string strip_namespace(std::string_view name) {
    const auto colon = name.find(':');
    return std::string(colon == std::string_view::npos ? name : name.substr(colon + 1));
}

class Parser {
public:
    explicit Parser(SurfaceResources& resources) : resources_(&resources) {}

    [[nodiscard]] std::expected<SurfaceRuleRef, SurfaceError> rule(Json node);
    [[nodiscard]] std::expected<std::shared_ptr<const SurfaceCondition>, SurfaceError> condition(
        Json node);

private:
    [[nodiscard]] std::expected<Anchor, SurfaceError> anchor(Json node);
    [[nodiscard]] std::expected<registry::BlockStateId, SurfaceError> block_state(Json node);

    [[nodiscard]] static i64 integer_at(Json node, const char* key, i64 fallback) {
        i64  value = fallback;
        auto field = node.at_key(key);
        if (field.error() == simdjson::SUCCESS) {
            (void)field.get(value);
        }
        return value;
    }
    [[nodiscard]] static f64 number_at(Json node, const char* key, f64 fallback) {
        f64  value = fallback;
        auto field = node.at_key(key);
        if (field.error() == simdjson::SUCCESS) {
            (void)field.get(value);
        }
        return value;
    }
    [[nodiscard]] static bool flag_at(Json node, const char* key, bool fallback) {
        bool value = fallback;
        auto field = node.at_key(key);
        if (field.error() == simdjson::SUCCESS) {
            (void)field.get(value);
        }
        return value;
    }

    SurfaceResources* resources_;
};

std::expected<Anchor, SurfaceError> Parser::anchor(Json node) {
    // Exactly one of the three keys is present. A node with none of them is a
    // datapack error, not a height of zero.
    struct Named {
        const char* key;
        AnchorKind  kind;
    };
    static constexpr std::array<Named, 3> kKinds{Named{"absolute", AnchorKind::Absolute},
                                                 Named{"above_bottom", AnchorKind::AboveBottom},
                                                 Named{"below_top", AnchorKind::BelowTop}};
    for (const auto& [key, kind] : kKinds) {
        auto field = node.at_key(key);
        if (field.error() != simdjson::SUCCESS) {
            continue;
        }
        i64 value = 0;
        if (field.get(value) != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        return Anchor{kind, static_cast<i32>(value)};
    }
    OV_LOG_ERROR("worldgen: a vertical anchor names none of absolute/above_bottom/below_top");
    return std::unexpected(SurfaceError::Malformed);
}

std::expected<registry::BlockStateId, SurfaceError> Parser::block_state(Json node) {
    std::string_view name;
    if (node.at_key("Name").get(name) != simdjson::SUCCESS) {
        return std::unexpected(SurfaceError::Malformed);
    }
    std::vector<std::pair<std::string_view, std::string_view>> properties;
    auto                                                       raw = node.at_key("Properties");
    if (raw.error() == simdjson::SUCCESS) {
        simdjson::dom::object object;
        if (raw.get(object) != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        for (auto [key, value] : object) {
            std::string_view text;
            if (value.get(text) != simdjson::SUCCESS) {
                return std::unexpected(SurfaceError::Malformed);
            }
            properties.emplace_back(key, text);
        }
    }
    auto state = resources_->block_state(name, properties);
    if (!state) {
        OV_LOG_ERROR("worldgen: surface rule names block '{}', which the registry does not have",
                     name);
        return std::unexpected(SurfaceError::UnknownBlock);
    }
    return *state;
}

std::expected<std::shared_ptr<const SurfaceCondition>, SurfaceError> Parser::condition(Json node) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(SurfaceError::Malformed);
    }
    const std::string kind = strip_namespace(type);

    const auto wrap = [](auto pointer) {
        return std::static_pointer_cast<const SurfaceCondition>(std::move(pointer));
    };

    if (kind == "biome") {
        simdjson::dom::array raw;
        if (node.at_key("biome_is").get(raw) != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        std::vector<std::string> names;
        for (auto entry : raw) {
            std::string_view name;
            if (entry.get(name) != simdjson::SUCCESS) {
                return std::unexpected(SurfaceError::Malformed);
            }
            names.emplace_back(name);
        }
        return wrap(std::make_shared<const BiomeCondition>(std::move(names)));
    }
    if (kind == "noise_threshold") {
        std::string_view name;
        if (node.at_key("noise").get(name) != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        const NormalNoise* noise = resources_->noise(name);
        if (noise == nullptr) {
            OV_LOG_ERROR("worldgen: surface rule names noise '{}', which has no file", name);
            return std::unexpected(SurfaceError::Missing);
        }
        return wrap(std::make_shared<const NoiseThreshold>(*noise, number_at(node, "min_threshold",
                                                                            0.0),
                                                           number_at(node, "max_threshold", 0.0)));
    }
    if (kind == "vertical_gradient") {
        std::string_view name;
        if (node.at_key("random_name").get(name) != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        auto below = node.at_key("true_at_and_below");
        auto above = node.at_key("false_at_and_above");
        if (below.error() != simdjson::SUCCESS || above.error() != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        auto low  = anchor(below.value());
        auto high = anchor(above.value());
        if (!low) return std::unexpected(low.error());
        if (!high) return std::unexpected(high.error());
        return wrap(std::make_shared<const VerticalGradient>(resources_->random_factory(name), *low,
                                                             *high));
    }
    if (kind == "y_above") {
        auto field = node.at_key("anchor");
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        auto where = anchor(field.value());
        if (!where) return std::unexpected(where.error());
        return wrap(std::make_shared<const YAbove>(
            *where, static_cast<i32>(integer_at(node, "surface_depth_multiplier", 0)),
            flag_at(node, "add_stone_depth", false)));
    }
    if (kind == "water") {
        return wrap(std::make_shared<const WaterCondition>(
            static_cast<i32>(integer_at(node, "offset", 0)),
            static_cast<i32>(integer_at(node, "surface_depth_multiplier", 0)),
            flag_at(node, "add_stone_depth", false)));
    }
    if (kind == "temperature") {
        return wrap(std::make_shared<const TemperatureCondition>());
    }
    if (kind == "steep") {
        return wrap(std::make_shared<const SteepCondition>());
    }
    if (kind == "not") {
        auto field = node.at_key("invert");
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        auto inner = condition(field.value());
        if (!inner) return inner;
        return wrap(std::make_shared<const NotCondition>(*inner));
    }
    if (kind == "hole") {
        return wrap(std::make_shared<const HoleCondition>());
    }
    if (kind == "above_preliminary_surface") {
        return wrap(std::make_shared<const AbovePreliminarySurface>());
    }
    if (kind == "stone_depth") {
        std::string_view side;
        if (node.at_key("surface_type").get(side) != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        if (side != "floor" && side != "ceiling") {
            OV_LOG_ERROR("worldgen: stone_depth surface_type '{}' is neither floor nor ceiling",
                         side);
            return std::unexpected(SurfaceError::Unsupported);
        }
        return wrap(std::make_shared<const StoneDepth>(
            side == "floor" ? StoneDepthSide::Floor : StoneDepthSide::Ceiling,
            static_cast<i32>(integer_at(node, "offset", 0)),
            flag_at(node, "add_surface_depth", false),
            static_cast<i32>(integer_at(node, "secondary_depth_range", 0))));
    }

    OV_LOG_ERROR("worldgen: surface condition type '{}' is not implemented", kind);
    return std::unexpected(SurfaceError::Unsupported);
}

std::expected<SurfaceRuleRef, SurfaceError> Parser::rule(Json node) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(SurfaceError::Malformed);
    }
    const std::string kind = strip_namespace(type);

    const auto wrap = [](auto pointer) {
        return std::static_pointer_cast<const SurfaceRule>(std::move(pointer));
    };

    if (kind == "sequence") {
        simdjson::dom::array raw;
        if (node.at_key("sequence").get(raw) != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        std::vector<SurfaceRuleRef> rules;
        for (auto entry : raw) {
            auto parsed = rule(entry);
            if (!parsed) return parsed;
            rules.push_back(*parsed);
        }
        return wrap(std::make_shared<const Sequence>(std::move(rules)));
    }
    if (kind == "condition") {
        auto guard = node.at_key("if_true");
        auto body  = node.at_key("then_run");
        if (guard.error() != simdjson::SUCCESS || body.error() != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        auto tested = condition(guard.value());
        if (!tested) return std::unexpected(tested.error());
        auto then_run = rule(body.value());
        if (!then_run) return then_run;
        return wrap(std::make_shared<const Conditional>(*tested, *then_run));
    }
    if (kind == "block") {
        auto field = node.at_key("result_state");
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        auto state = block_state(field.value());
        if (!state) return std::unexpected(state.error());
        return wrap(std::make_shared<const BlockRule>(*state));
    }
    if (kind == "bandlands") {
        return wrap(std::make_shared<const Bandlands>(*resources_));
    }

    OV_LOG_ERROR("worldgen: surface rule type '{}' is not implemented", kind);
    return std::unexpected(SurfaceError::Unsupported);
}

}  // namespace

SurfaceQueries::~SurfaceQueries()     = default;
SurfaceRule::~SurfaceRule()           = default;
SurfaceCondition::~SurfaceCondition() = default;
SurfaceResources::~SurfaceResources() = default;

std::string_view to_string(SurfaceError error) noexcept {
    switch (error) {
        case SurfaceError::Missing:
            return "a file the surface rule names is missing";
        case SurfaceError::Malformed:
            return "a surface rule is not the shape one has";
        case SurfaceError::Unsupported:
            return "a surface rule or condition type is not implemented";
        case SurfaceError::UnknownBlock:
            return "a surface rule names a block the registry does not have";
    }
    return "unknown";
}

i32 resolve_anchor_absolute(i32 value) noexcept {
    return value;
}
i32 resolve_anchor_above_bottom(i32 value, i32 min_y) noexcept {
    return min_y + value;
}
i32 resolve_anchor_below_top(i32 value, i32 min_y, i32 height) noexcept {
    return min_y + height - 1 - value;
}

template<typename Random>
std::array<registry::BlockStateId, kClayBandCount> clay_bands_from(Random&                random,
                                                                   const ClayBandColours& colours) {
    std::array<registry::BlockStateId, kClayBandCount> bands{};
    bands.fill(colours.terracotta);

    // Orange, sparsely: the loop advances by its own draw *on top of* the
    // for-loop's increment, so the step is at least two. Writing it as a plain
    // stride would produce twice as many orange bands and consume half as many
    // draws, which would then shift every colour after it.
    for (usize index = 0; index < kClayBandCount; ++index) {
        index += static_cast<usize>(random.next_int(5)) + 1;
        if (index < kClayBandCount) {
            bands[index] = colours.orange;
        }
    }

    const auto make_bands = [&](i32 minimum, registry::BlockStateId colour) {
        const i32 runs = 6 + random.next_int(10);  // nextIntBetweenInclusive(6, 15)
        for (i32 run = 0; run < runs; ++run) {
            const i32 length = minimum + random.next_int(3);
            const i32 start  = random.next_int(static_cast<i32>(kClayBandCount));
            for (i32 offset = 0;
                 offset < length && start + offset < static_cast<i32>(kClayBandCount); ++offset) {
                bands[static_cast<usize>(start + offset)] = colour;
            }
        }
    };
    make_bands(1, colours.yellow);
    make_bands(2, colours.brown);
    make_bands(1, colours.red);

    // White, with grey shoulders. The two shoulder draws happen even when the
    // band is at an edge and the write is skipped — the state is consumed
    // either way, and that is the kind of detail that decides whether the
    // table matches.
    const i32 whites = 9 + random.next_int(7);  // nextIntBetweenInclusive(9, 15)
    i32       placed = 0;
    for (i32 index = 0; placed < whites && index < static_cast<i32>(kClayBandCount);
         index += random.next_int(16) + 4) {
        bands[static_cast<usize>(index)] = colours.white;
        if (index - 1 > 0 && random.next_boolean()) {
            bands[static_cast<usize>(index - 1)] = colours.light_gray;
        }
        if (index + 1 < static_cast<i32>(kClayBandCount) && random.next_boolean()) {
            bands[static_cast<usize>(index + 1)] = colours.light_gray;
        }
        ++placed;
    }
    return bands;
}

std::array<registry::BlockStateId, kClayBandCount> generate_clay_bands(
    math::XoroshiroRandomSource& random, const ClayBandColours& colours) {
    return clay_bands_from(random, colours);
}

std::array<registry::BlockStateId, kClayBandCount> generate_clay_bands(
    math::LegacyRandomSource& random, const ClayBandColours& colours) {
    return clay_bands_from(random, colours);
}

std::expected<SurfaceRuleRef, SurfaceError> load_surface_rule(
    const std::filesystem::path& settings_file, SurfaceResources& resources) {
    if (!std::filesystem::is_regular_file(settings_file)) {
        OV_LOG_ERROR("worldgen: {} is missing", settings_file.string());
        return std::unexpected(SurfaceError::Missing);
    }
    auto text = simdjson::padded_string::load(settings_file.string());
    if (text.error() != simdjson::SUCCESS) {
        return std::unexpected(SurfaceError::Malformed);
    }
    simdjson::dom::parser parser;
    auto                  document = parser.parse(text.value());
    if (document.error() != simdjson::SUCCESS) {
        OV_LOG_ERROR("worldgen: {} is not valid JSON", settings_file.string());
        return std::unexpected(SurfaceError::Malformed);
    }
    auto node = document.at_key("surface_rule");
    if (node.error() != simdjson::SUCCESS) {
        OV_LOG_ERROR("worldgen: {} has no surface_rule", settings_file.string());
        return std::unexpected(SurfaceError::Malformed);
    }

    Parser interpreter{resources};
    return interpreter.rule(node.value());
}

}  // namespace ov::worldgen
