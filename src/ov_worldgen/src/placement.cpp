#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/placement.hpp"

#include "feature_json.hpp"
#include "overworld_feature.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/biome_zoom.hpp"  // ── worldgen-3 ──

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov::worldgen {

FeatureLevel::~FeatureLevel()           = default;
IntProvider::~IntProvider()             = default;
HeightProvider::~HeightProvider()       = default;
BlockPredicate::~BlockPredicate()       = default;
BiomeFeatures::~BiomeFeatures()         = default;
PlacementModifier::~PlacementModifier() = default;

std::string_view to_string(FeatureError error) noexcept {
    switch (error) {
        case FeatureError::Missing:
            return "missing";
        case FeatureError::Malformed:
            return "malformed";
        case FeatureError::Unsupported:
            return "unsupported";
    }
    return "unknown";
}

f64 FeatureRandom::next_gaussian() noexcept {
    if (have_next_gaussian_) {
        have_next_gaussian_ = false;
        return next_gaussian_;
    }
    f64 v1 = 0.0;
    f64 v2 = 0.0;
    f64 s  = 0.0;
    do {
        // Two draws, in this order. Split out because C++ does not sequence
        // the operands of an expression and Java does.
        const f64 first  = next_double();
        const f64 second = next_double();
        v1 = 2.0 * first - 1.0;
        v2 = 2.0 * second - 1.0;
        s  = v1 * v1 + v2 * v2;
    } while (s >= 1.0 || s == 0.0);
    const f64 multiplier = std::sqrt(-2.0 * std::log(s) / s);
    next_gaussian_       = v2 * multiplier;
    have_next_gaussian_  = true;
    return v1 * multiplier;
}

FeatureRandom::Kind configured_feature_random() noexcept {
    const char* choice = std::getenv("OV_FEATURE_RANDOM");
    return choice != nullptr && std::string_view(choice) == "legacy" ? FeatureRandom::Kind::Legacy
                                                                     : FeatureRandom::Kind::Xoroshiro;
}

std::string strip_namespace(std::string_view name) {
    const auto colon = name.find(':');
    return std::string(colon == std::string_view::npos ? name : name.substr(colon + 1));
}

std::string qualify(std::string_view name) {
    return name.find(':') == std::string_view::npos ? "minecraft:" + std::string(name)
                                                    : std::string(name);
}

i32 VerticalAnchor::resolve(i32 min_y, i32 world_height) const noexcept {
    switch (kind) {
        case Kind::Absolute:
            return value;
        case Kind::AboveBottom:
            return min_y + value;
        case Kind::BelowTop:
            // One less than the top, because the top itself is the first y
            // that is *outside* the world.
            return min_y + world_height - 1 - value;
    }
    return value;
}

namespace {

/// `nextInt(max - min + 1) + min`, the game's inclusive range draw.
///
/// Written out rather than inlined at each site because the +1 is the whole of
/// it: without it no ore ever reaches the top of its band, which is a bias too
/// small to see and large enough to fail a parity run.
[[nodiscard]] i32 between_inclusive(FeatureRandom& random, i32 low,
                                    i32 high) noexcept {
    return random.next_int(high - low + 1) + low;
}

// ── Int providers ───────────────────────────────────────────────────────────

class ConstantInt final : public IntProvider {
public:
    explicit ConstantInt(i32 value) : value_(value) {}
    [[nodiscard]] i32 sample(FeatureRandom&) const override { return value_; }

private:
    i32 value_;
};

class UniformInt final : public IntProvider {
public:
    UniformInt(i32 low, i32 high) : low_(low), high_(high) {}
    [[nodiscard]] i32 sample(FeatureRandom& random) const override {
        return between_inclusive(random, low_, high_);
    }

private:
    i32 low_;
    i32 high_;
};

class BiasedToBottomInt final : public IntProvider {
public:
    BiasedToBottomInt(i32 low, i32 high) : low_(low), high_(high) {}
    /// Two nested draws, which is what makes it biased: the second range is
    /// itself chosen by the first.
    [[nodiscard]] i32 sample(FeatureRandom& random) const override {
        return low_ + random.next_int(random.next_int(high_ - low_ + 1) + 1);
    }

private:
    i32 low_;
    i32 high_;
};

class ClampedInt final : public IntProvider {
public:
    ClampedInt(IntProviderRef source, i32 low, i32 high)
        : source_(std::move(source)), low_(low), high_(high) {}
    [[nodiscard]] i32 sample(FeatureRandom& random) const override {
        return std::clamp(source_->sample(random), low_, high_);
    }

private:
    IntProviderRef source_;
    i32            low_;
    i32            high_;
};

class ClampedNormalInt final : public IntProvider {
public:
    ClampedNormalInt(f32 mean, f32 deviation, i32 low, i32 high)
        : mean_(mean), deviation_(deviation), low_(low), high_(high) {}
    [[nodiscard]] i32 sample(FeatureRandom& random) const override {
        // std::llround is round-half-away-from-zero; Java's Math.round is
        // round-half-up. They differ only on exact halves of a gaussian, which
        // cannot happen, but the floor of value + 0.5 is what the game does
        // and costs nothing to spell out.
        const f64 value =
            static_cast<f64>(mean_) + random.next_gaussian() * static_cast<f64>(deviation_);
        const auto rounded = static_cast<i32>(std::floor(value + 0.5));
        return std::clamp(rounded, low_, high_);
    }

private:
    f32 mean_;
    f32 deviation_;
    i32 low_;
    i32 high_;
};

class WeightedListInt final : public IntProvider {
public:
    WeightedListInt(std::vector<std::pair<i32, i32>> entries, i32 total)
        : entries_(std::move(entries)), total_(total) {}
    [[nodiscard]] i32 sample(FeatureRandom& random) const override {
        i32 roll = random.next_int(total_);
        for (const auto& [value, weight] : entries_) {
            roll -= weight;
            if (roll < 0) {
                return value;
            }
        }
        return entries_.empty() ? 0 : entries_.back().first;
    }

private:
    /// value, weight.
    std::vector<std::pair<i32, i32>> entries_;
    i32                              total_;
};

// ── Height providers ────────────────────────────────────────────────────────

class ConstantHeight final : public HeightProvider {
public:
    explicit ConstantHeight(VerticalAnchor anchor) : anchor_(anchor) {}
    [[nodiscard]] i32 sample(FeatureRandom&, i32 min_y,
                             i32 world_height) const override {
        return anchor_.resolve(min_y, world_height);
    }

private:
    VerticalAnchor anchor_;
};

class UniformHeight final : public HeightProvider {
public:
    UniformHeight(VerticalAnchor low, VerticalAnchor high) : low_(low), high_(high) {}
    [[nodiscard]] i32 sample(FeatureRandom& random, i32 min_y,
                             i32 world_height) const override {
        const i32 low  = low_.resolve(min_y, world_height);
        const i32 high = high_.resolve(min_y, world_height);
        if (low > high) {
            // The game warns and returns the bottom rather than drawing from an
            // empty range. Reproduced because a datapack can express it and a
            // negative bound would otherwise reach next_int with a bound of
            // zero.
            OV_LOG_WARN("worldgen: empty height range [{}, {}]", low, high);
            return low;
        }
        return between_inclusive(random, low, high);
    }

private:
    VerticalAnchor low_;
    VerticalAnchor high_;
};

/// Uniform in the middle, tapering at both ends.
///
/// Two draws whose sum is the height, which is what makes the distribution a
/// trapezoid rather than a triangle: `plateau` widens the flat top, and at
/// plateau zero the two draws are equal and it is a triangle.
class TrapezoidHeight final : public HeightProvider {
public:
    TrapezoidHeight(VerticalAnchor low, VerticalAnchor high, i32 plateau)
        : low_(low), high_(high), plateau_(plateau) {}
    [[nodiscard]] i32 sample(FeatureRandom& random, i32 min_y,
                             i32 world_height) const override {
        const i32 low  = low_.resolve(min_y, world_height);
        const i32 high = high_.resolve(min_y, world_height);
        if (low > high) {
            OV_LOG_WARN("worldgen: empty trapezoid range [{}, {}]", low, high);
            return low;
        }
        const i32 span = high - low;
        if (plateau_ >= span) {
            return between_inclusive(random, low, high);
        }
        const i32 slope = (span - plateau_) / 2;
        const i32 flat  = span - slope;
        return low + random.next_int(flat + 1) + random.next_int(slope + 1);
    }

private:
    VerticalAnchor low_;
    VerticalAnchor high_;
    i32            plateau_;
};

/// Two nested inclusive draws, so low heights come out far more often.
class BiasedToBottomHeight final : public HeightProvider {
public:
    BiasedToBottomHeight(VerticalAnchor low, VerticalAnchor high, i32 inner)
        : low_(low), high_(high), inner_(inner) {}
    [[nodiscard]] i32 sample(FeatureRandom& random, i32 min_y,
                             i32 world_height) const override {
        const i32 low  = low_.resolve(min_y, world_height);
        const i32 high = high_.resolve(min_y, world_height);
        if (high - inner_ < low + 1) {
            OV_LOG_WARN("worldgen: biased_to_bottom has no room in [{}, {}]", low, high);
            return low;
        }
        const i32 first = between_inclusive(random, low + inner_, high);
        return between_inclusive(random, low, first - 1);
    }

private:
    VerticalAnchor low_;
    VerticalAnchor high_;
    i32            inner_;
};

/// Three nested draws. The one the amethyst geodes and the deepest ores use.
class VeryBiasedToBottomHeight final : public HeightProvider {
public:
    VeryBiasedToBottomHeight(VerticalAnchor low, VerticalAnchor high, i32 inner)
        : low_(low), high_(high), inner_(inner) {}
    [[nodiscard]] i32 sample(FeatureRandom& random, i32 min_y,
                             i32 world_height) const override {
        const i32 low  = low_.resolve(min_y, world_height);
        const i32 high = high_.resolve(min_y, world_height);
        if (high - inner_ < low + 1) {
            OV_LOG_WARN("worldgen: very_biased_to_bottom has no room in [{}, {}]", low, high);
            return low;
        }
        const i32 first  = between_inclusive(random, low + inner_, high);
        const i32 second = between_inclusive(random, low, first - 1);
        // The third draw reaches one higher than the second, which is the
        // asymmetry that separates this from applying the bias twice.
        return between_inclusive(random, low, second - 1 + inner_);
    }

private:
    VerticalAnchor low_;
    VerticalAnchor high_;
    i32            inner_;
};

}  // namespace

// ── Block tags ──────────────────────────────────────────────────────────────

struct BlockTags::Impl {
    std::unordered_map<std::string, std::unordered_set<u16>> tags;
    /// The same members in file order (features-2: the corals draw an index).
    std::unordered_map<std::string, std::vector<u16>> ordered;
};

std::expected<BlockTags, FeatureError> BlockTags::load(const std::filesystem::path& data_root,
                                                       const registry::BlockRegistry& blocks) {
    const auto directory = data_root / "tags" / "blocks";
    if (!std::filesystem::is_directory(directory)) {
        OV_LOG_ERROR("worldgen: {} is missing; block tags come out of the generated data",
                     directory.string());
        return std::unexpected(FeatureError::Missing);
    }

    // Read every file first, then resolve, because a tag may name a tag whose
    // file has not been read yet and the order on disk is not a dependency
    // order.
    std::unordered_map<std::string, std::vector<std::string>> raw;
    simdjson::dom::parser                                     parser;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        auto text = simdjson::padded_string::load(entry.path().string());
        if (text.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto document = parser.parse(text.value());
        if (document.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        simdjson::dom::array values;
        if (document.at_key("values").get(values) != simdjson::SUCCESS) {
            continue;
        }
        auto& members = raw["minecraft:" + entry.path().stem().string()];
        for (auto value : values) {
            std::string_view name;
            if (value.get(name) == simdjson::SUCCESS) {
                members.emplace_back(name);
                continue;
            }
            // The long form, `{"id": ..., "required": false}`. Optional entries
            // are still members when the block exists, which it always does
            // here since this is vanilla's own data.
            simdjson::dom::object object;
            if (value.get(object) == simdjson::SUCCESS &&
                object.at_key("id").get(name) == simdjson::SUCCESS) {
                members.emplace_back(name);
            }
        }
    }

    auto impl = std::make_shared<Impl>();
    // Resolution is by hand rather than recursive-with-memo because a cycle in
    // a datapack must not overflow the stack; `seen` makes a cycle a no-op.
    for (const auto& [name, members] : raw) {
        std::unordered_set<u16>  resolved;
        std::unordered_set<std::string> seen{name};
        std::vector<std::string> pending{name};
        while (!pending.empty()) {
            const std::string current = std::move(pending.back());
            pending.pop_back();
            const auto found = raw.find(current);
            if (found == raw.end()) {
                OV_LOG_WARN("worldgen: tag {} names {}, which has no file", name, current);
                continue;
            }
            for (const std::string& member : found->second) {
                if (member.starts_with('#')) {
                    const std::string referenced = qualify(std::string_view(member).substr(1));
                    if (seen.insert(referenced).second) {
                        pending.push_back(referenced);
                    }
                    continue;
                }
                if (const auto block = blocks.find_block(qualify(member))) {
                    resolved.insert(block->value());
                } else {
                    OV_LOG_WARN("worldgen: tag {} names unknown block {}", name, member);
                }
            }
        }
        impl->tags.emplace(name, std::move(resolved));
    }

    // The ordered form: file order, a nested tag expanded where it is named,
    // a repeat dropped at its second appearance. Depth-first with an explicit
    // stack of (tag, next entry) so that a cycle is a no-op, not an overflow.
    for (const auto& [name, members] : raw) {
        (void)members;
        std::vector<u16>                              out;
        std::unordered_set<u16>                       placed;
        std::unordered_set<std::string>               open{name};
        std::vector<std::pair<std::string, usize>>    stack{{name, 0}};
        while (!stack.empty()) {
            auto& [current, next] = stack.back();
            const auto found      = raw.find(current);
            if (found == raw.end() || next >= found->second.size()) {
                stack.pop_back();
                continue;
            }
            const std::string member = found->second[next++];
            if (member.starts_with('#')) {
                std::string referenced = qualify(std::string_view(member).substr(1));
                if (open.insert(referenced).second) {
                    stack.emplace_back(std::move(referenced), 0);
                }
                continue;
            }
            if (const auto block = blocks.find_block(qualify(member))) {
                if (placed.insert(block->value()).second) {
                    out.push_back(block->value());
                }
            }
        }
        impl->ordered.emplace(name, std::move(out));
    }

    BlockTags result;
    result.impl_ = std::move(impl);
    OV_LOG_INFO("worldgen: {} block tags", result.impl_->tags.size());
    return result;
}

bool BlockTags::contains(std::string_view tag, registry::BlockId block) const {
    if (impl_ == nullptr) {
        return false;
    }
    const auto found = impl_->tags.find(qualify(tag));
    return found != impl_->tags.end() && found->second.contains(block.value());
}

bool BlockTags::known(std::string_view tag) const {
    return impl_ != nullptr && impl_->tags.contains(qualify(tag));
}

std::vector<registry::BlockId> BlockTags::ordered(std::string_view tag) const {
    std::vector<registry::BlockId> out;
    if (impl_ == nullptr) {
        return out;
    }
    const auto found = impl_->ordered.find(qualify(tag));
    if (found == impl_->ordered.end()) {
        return out;
    }
    out.reserve(found->second.size());
    for (const u16 block : found->second) {
        out.push_back(registry::BlockId{block});
    }
    return out;
}

usize BlockTags::tag_count() const noexcept {
    return impl_ == nullptr ? 0 : impl_->tags.size();
}

// ── Block predicates ────────────────────────────────────────────────────────

namespace {

class MatchingBlocks final : public BlockPredicate {
public:
    MatchingBlocks(std::vector<u16> blocks, BlockPos offset,
                   const registry::BlockRegistry& registry)
        : blocks_(std::move(blocks)), offset_(offset), registry_(&registry) {}
    [[nodiscard]] bool test(const FeatureLevel& level, BlockPos at) const override {
        const auto state =
            level.block_at(at.x + offset_.x, at.y + offset_.y, at.z + offset_.z);
        const auto block = registry_->block_of(state);
        return std::ranges::find(blocks_, block.value()) != blocks_.end();
    }

private:
    std::vector<u16>               blocks_;
    BlockPos                 offset_;
    const registry::BlockRegistry* registry_;
};

class MatchingBlockTag final : public BlockPredicate {
public:
    MatchingBlockTag(std::string tag, BlockPos offset,
                     const registry::BlockRegistry& registry, const BlockTags& tags)
        : tag_(std::move(tag)), offset_(offset), registry_(&registry), tags_(&tags) {}
    [[nodiscard]] bool test(const FeatureLevel& level, BlockPos at) const override {
        const auto state =
            level.block_at(at.x + offset_.x, at.y + offset_.y, at.z + offset_.z);
        return tags_->contains(tag_, registry_->block_of(state));
    }

private:
    std::string                    tag_;
    BlockPos                 offset_;
    const registry::BlockRegistry* registry_;
    const BlockTags*               tags_;
};

class SturdyFace final : public BlockPredicate {
public:
    SturdyFace(BlockPos offset, registry::BlockRegistry::Face face,
               const registry::BlockRegistry& registry)
        : offset_(offset), face_(face), registry_(&registry) {}
    [[nodiscard]] bool test(const FeatureLevel& level, BlockPos at) const override {
        return registry_->face_is_sturdy(
            level.block_at(at.x + offset_.x, at.y + offset_.y, at.z + offset_.z), face_);
    }

private:
    BlockPos                 offset_;
    registry::BlockRegistry::Face  face_;
    const registry::BlockRegistry* registry_;
};

class InsideWorld final : public BlockPredicate {
public:
    explicit InsideWorld(BlockPos offset) : offset_(offset) {}
    [[nodiscard]] bool test(const FeatureLevel& level, BlockPos at) const override {
        return !level.outside_build_height(at.y + offset_.y);
    }

private:
    BlockPos offset_;
};

enum class Combine : u8 { AllOf, AnyOf, Not, AlwaysTrue };

class Combined final : public BlockPredicate {
public:
    Combined(Combine how, std::vector<BlockPredicateRef> parts)
        : how_(how), parts_(std::move(parts)) {}
    [[nodiscard]] bool test(const FeatureLevel& level, BlockPos at) const override {
        switch (how_) {
            case Combine::AllOf:
                return std::ranges::all_of(
                    parts_, [&](const auto& part) { return part->test(level, at); });
            case Combine::AnyOf:
                return std::ranges::any_of(
                    parts_, [&](const auto& part) { return part->test(level, at); });
            case Combine::Not:
                return parts_.empty() || !parts_.front()->test(level, at);
            case Combine::AlwaysTrue:
                return true;
        }
        return true;
    }

private:
    Combine                        how_;
    std::vector<BlockPredicateRef> parts_;
};

[[nodiscard]] BlockPos read_offset(Json node) {
    simdjson::dom::array values;
    if (node.at_key("offset").get(values) != simdjson::SUCCESS) {
        return {};
    }
    std::array<i32, 3> parts{};
    usize              index = 0;
    for (auto value : values) {
        i64 number = 0;
        if (index < parts.size() && value.get(number) == simdjson::SUCCESS) {
            parts[index] = static_cast<i32>(number);
        }
        ++index;
    }
    return {parts[0], parts[1], parts[2]};
}

/// A field that is either one name or a list of names.
[[nodiscard]] std::vector<std::string> read_names(simdjson::simdjson_result<Json> field) {
    std::vector<std::string> names;
    if (field.error() != simdjson::SUCCESS) {
        return names;
    }
    std::string_view single;
    if (field.get(single) == simdjson::SUCCESS) {
        names.emplace_back(single);
        return names;
    }
    simdjson::dom::array list;
    if (field.get(list) == simdjson::SUCCESS) {
        for (auto value : list) {
            std::string_view name;
            if (value.get(name) == simdjson::SUCCESS) {
                names.emplace_back(name);
            }
        }
    }
    return names;
}

}  // namespace

std::expected<BlockPredicateRef, FeatureError> parse_block_predicate(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string kind   = strip_namespace(type);
    const auto        offset = read_offset(node);
    const auto        wrap   = [](auto pointer) {
        return std::static_pointer_cast<const BlockPredicate>(std::move(pointer));
    };

    // `matching_fluids` is answered in overworld_feature.cpp (features-2): a
    // fluid is not a block — `flowing_water` and `empty` have no block, and a
    // waterlogged block holds water.
    if (kind == "matching_blocks") {
        std::vector<u16> ids;
        for (const std::string& name : read_names(node.at_key(
                 kind == "matching_blocks" ? "blocks" : "fluids"))) {
            if (const auto block = blocks.find_block(qualify(name))) {
                ids.push_back(block->value());
            } else {
                OV_LOG_ERROR("worldgen: block predicate names unknown block {}", name);
                return std::unexpected(FeatureError::Malformed);
            }
        }
        return wrap(std::make_shared<const MatchingBlocks>(std::move(ids), offset, blocks));
    }
    if (kind == "matching_block_tag") {
        std::string_view tag;
        if (node.at_key("tag").get(tag) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        if (!tags.known(tag)) {
            OV_LOG_ERROR("worldgen: block predicate names unknown tag {}", tag);
            return std::unexpected(FeatureError::Malformed);
        }
        return wrap(std::make_shared<const MatchingBlockTag>(std::string(tag), offset, blocks,
                                                             tags));
    }
    if (kind == "has_sturdy_face") {
        std::string_view direction;
        if (node.at_key("direction").get(direction) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        using Face                          = registry::BlockRegistry::Face;
        const std::array<std::pair<std::string_view, Face>, 6> faces{
            {{"down", Face::Down},
             {"up", Face::Up},
             {"north", Face::North},
             {"south", Face::South},
             {"west", Face::West},
             {"east", Face::East}}};
        for (const auto& [name, face] : faces) {
            if (name == direction) {
                return wrap(std::make_shared<const SturdyFace>(offset, face, blocks));
            }
        }
        return std::unexpected(FeatureError::Malformed);
    }
    if (kind == "inside_world_bounds") {
        return wrap(std::make_shared<const InsideWorld>(offset));
    }
    if (kind == "true") {
        return wrap(std::make_shared<const Combined>(Combine::AlwaysTrue,
                                                     std::vector<BlockPredicateRef>{}));
    }
    if (kind == "all_of" || kind == "any_of" || kind == "not") {
        std::vector<BlockPredicateRef> parts;
        const auto                     collect = [&](Json child) -> std::expected<void, FeatureError> {
            auto parsed = parse_block_predicate(child, blocks, tags);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            parts.push_back(*parsed);
            return {};
        };
        if (kind == "not") {
            auto field = node.at_key("predicate");
            if (field.error() != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            if (auto ok = collect(field.value()); !ok) {
                return std::unexpected(ok.error());
            }
        } else {
            simdjson::dom::array list;
            if (node.at_key("predicates").get(list) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            for (auto child : list) {
                if (auto ok = collect(child); !ok) {
                    return std::unexpected(ok.error());
                }
            }
        }
        const Combine how = kind == "all_of"   ? Combine::AllOf
                            : kind == "any_of" ? Combine::AnyOf
                                               : Combine::Not;
        return wrap(std::make_shared<const Combined>(how, std::move(parts)));
    }

    // ── the tree and vegetation work hooks in here, and only here ──────────
    if (kind == "would_survive") {
        return parse_survival_predicate(node, blocks, tags);
    }
    if (auto shaped = parse_shape_predicate(kind, node, blocks, tags)) {  // features-2
        return std::move(*shaped);
    }
    // ── end of that hook ───────────────────────────────────────────────────

    // `solid`, `replaceable` and `unobstructed` are the remaining vanilla
    // predicates. They ask whether a block could stand at a position, which is
    // a *gameplay* question — ov_gameplay is a layer above this one and a
    // feature must not reach up into it. `would_survive` above is answered
    // from a named table of the rules the vegetation features actually need,
    // in vegetation_feature.cpp; the other three have no such table and are
    // named and refused. That is the honest outcome: a predicate quietly
    // answered "yes" would scatter saplings across bare stone.
    OV_LOG_ERROR("worldgen: block predicate '{}' is not implemented", kind);
    return std::unexpected(FeatureError::Unsupported);
}

// ── Placement modifiers ─────────────────────────────────────────────────────

namespace {

/// A modifier that emits `count` copies of its input.
class CountPlacement final : public PlacementModifier {
public:
    explicit CountPlacement(IntProviderRef count) : count_(std::move(count)) {}
    void positions(const FeatureContext&, const FeatureLevel&,
                   FeatureRandom& random, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        const i32 many = count_->sample(random);
        for (i32 index = 0; index < many; ++index) {
            out.push_back(at);
        }
    }
    [[nodiscard]] std::string_view name() const override { return "count"; }

private:
    IntProviderRef count_;
};

class RarityFilter final : public PlacementModifier {
public:
    explicit RarityFilter(i32 chance) : chance_(chance) {}
    void positions(const FeatureContext&, const FeatureLevel&,
                   FeatureRandom& random, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        // A float compare and not a next_int(chance) == 0: the two consume the
        // same one draw and disagree about which chunks pass.
        if (random.next_float() < 1.0F / static_cast<f32>(chance_)) {
            out.push_back(at);
        }
    }
    [[nodiscard]] std::string_view name() const override { return "rarity_filter"; }

private:
    i32 chance_;
};

class InSquare final : public PlacementModifier {
public:
    void positions(const FeatureContext&, const FeatureLevel&,
                   FeatureRandom& random, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        // x then z, in that order, and both from the same generator. Swapping
        // them mirrors every scatter in the world about the diagonal.
        const i32 dx = random.next_int(16);
        const i32 dz = random.next_int(16);
        out.push_back({at.x + dx, at.y, at.z + dz});
    }
    [[nodiscard]] std::string_view name() const override { return "in_square"; }
};

class HeightRange final : public PlacementModifier {
public:
    explicit HeightRange(HeightProviderRef height) : height_(std::move(height)) {}
    void positions(const FeatureContext&, const FeatureLevel& level,
                   FeatureRandom& random, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        out.push_back({at.x, height_->sample(random, level.min_y(), level.world_height()), at.z});
    }
    [[nodiscard]] std::string_view name() const override { return "height_range"; }

private:
    HeightProviderRef height_;
};

}  // namespace

// ── worldgen-3 ── See biome_zoom.hpp.
std::string_view FeatureContext::biome_at(const FeatureLevel& level, i32 x, i32 y, i32 z) const {
    if (!fuzzy_biomes) {
        return level.biome_at(x, y, z);
    }
    const BiomeCell cell = fuzzy_biome_cell(biome_zoom_seed, x, y, z);
    return level.biome_at(cell.x * 4, cell.y * 4, cell.z * 4);
}

namespace {

class BiomeFilter final : public PlacementModifier {
public:
    void positions(const FeatureContext& context, const FeatureLevel& level,
                   FeatureRandom&, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        if (context.biomes == nullptr) {
            return;
        }
        if (context.biomes->lists(context.biome_at(level, at.x, at.y, at.z), context.feature_name)) {
            out.push_back(at);
        }
    }
    [[nodiscard]] std::string_view name() const override { return "biome"; }
};

class HeightmapPlacement final : public PlacementModifier {
public:
    explicit HeightmapPlacement(world::HeightmapType type) : type_(type) {}
    void positions(const FeatureContext&, const FeatureLevel& level,
                   FeatureRandom&, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        const i32 y = level.height(type_, at.x, at.z);
        // Strictly above the floor of the world, so a column that is entirely
        // empty produces nothing rather than a feature at the void.
        if (y > level.min_y()) {
            out.push_back({at.x, y, at.z});
        }
    }
    [[nodiscard]] std::string_view name() const override { return "heightmap"; }

private:
    world::HeightmapType type_;
};

class PredicateFilter final : public PlacementModifier {
public:
    explicit PredicateFilter(BlockPredicateRef predicate) : predicate_(std::move(predicate)) {}
    void positions(const FeatureContext&, const FeatureLevel& level,
                   FeatureRandom&, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        if (predicate_->test(level, at)) {
            out.push_back(at);
        }
    }
    [[nodiscard]] std::string_view name() const override { return "block_predicate_filter"; }

private:
    BlockPredicateRef predicate_;
};

class SurfaceWaterDepthFilter final : public PlacementModifier {
public:
    explicit SurfaceWaterDepthFilter(i32 max_depth) : max_depth_(max_depth) {}
    void positions(const FeatureContext&, const FeatureLevel& level,
                   FeatureRandom&, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        // The two generation-time heightmaps, not the stored ones: the depth of
        // the water is the gap between the top of it and the floor under it.
        const i32 floor   = level.height(world::HeightmapType::OceanFloorWG, at.x, at.z);
        const i32 surface = level.height(world::HeightmapType::WorldSurfaceWG, at.x, at.z);
        if (surface - floor <= max_depth_) {
            out.push_back(at);
        }
    }
    [[nodiscard]] std::string_view name() const override { return "surface_water_depth_filter"; }

private:
    i32 max_depth_;
};

class SurfaceRelativeThreshold final : public PlacementModifier {
public:
    SurfaceRelativeThreshold(world::HeightmapType type, i32 low, i32 high)
        : type_(type), low_(low), high_(high) {}
    void positions(const FeatureContext&, const FeatureLevel& level,
                   FeatureRandom&, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        // In i64, because the defaults are the extremes of an i32 and adding
        // the surface to one of them overflows.
        const auto surface = static_cast<i64>(level.height(type_, at.x, at.z));
        const i64  low     = surface + low_;
        const i64  high    = surface + high_;
        if (low <= at.y && at.y <= high) {
            out.push_back(at);
        }
    }
    [[nodiscard]] std::string_view name() const override {
        return "surface_relative_threshold_filter";
    }

private:
    world::HeightmapType type_;
    i64                  low_;
    i64                  high_;
};

class RandomOffset final : public PlacementModifier {
public:
    RandomOffset(IntProviderRef xz, IntProviderRef y) : xz_(std::move(xz)), y_(std::move(y)) {}
    void positions(const FeatureContext&, const FeatureLevel&,
                   FeatureRandom& random, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        // x, y, z, and the x and z draws come from the *same* provider asked
        // twice rather than one draw used for both.
        const i32 dx = xz_->sample(random);
        const i32 dy = y_->sample(random);
        const i32 dz = xz_->sample(random);
        out.push_back({at.x + dx, at.y + dy, at.z + dz});
    }
    [[nodiscard]] std::string_view name() const override { return "random_offset"; }

private:
    IntProviderRef xz_;
    IntProviderRef y_;
};

/// One position per floor found under the column, from the top down.
///
/// The nether's vegetation stands on the roofs of its caves, so a single
/// heightmap would put it all on the top surface and leave the caves bare.
class CountOnEveryLayer final : public PlacementModifier {
public:
    explicit CountOnEveryLayer(IntProviderRef count) : count_(std::move(count)) {}

    void positions(const FeatureContext& context, const FeatureLevel& level,
                   FeatureRandom& random, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        i32  layer = 0;
        bool found = true;
        while (found) {
            found           = false;
            const i32 many  = count_->sample(random);
            for (i32 index = 0; index < many; ++index) {
                const i32 x    = random.next_int(16) + at.x;
                const i32 z    = random.next_int(16) + at.z;
                const i32 top  = level.height(world::HeightmapType::MotionBlocking, x, z);
                const i32 here = floor_of_layer(context, level, x, top, z, layer);
                if (here != std::numeric_limits<i32>::max()) {
                    out.push_back({x, here, z});
                    found = true;
                }
            }
            ++layer;
        }
    }
    [[nodiscard]] std::string_view name() const override { return "count_on_every_layer"; }

private:
    /// The y just above the `layer`-th solid-under-empty boundary going down.
    [[nodiscard]] static i32 floor_of_layer(const FeatureContext& context,
                                            const FeatureLevel& level, i32 x, i32 top, i32 z,
                                            i32 layer) {
        const auto empty = [&](registry::BlockStateId state) {
            const auto block = context.blocks->block_of(state);
            const auto name  = context.blocks->block_name(block);
            return context.blocks->is_air(block) || name == "minecraft:water" ||
                   name == "minecraft:lava";
        };
        i32  seen  = 0;
        auto above = level.block_at(x, top, z);
        for (i32 y = top; y >= level.min_y() + 1; --y) {
            const auto below = level.block_at(x, y - 1, z);
            const auto name  = context.blocks->block_name(context.blocks->block_of(below));
            if (!empty(below) && empty(above) && name != "minecraft:bedrock") {
                if (seen == layer) {
                    return y;
                }
                ++seen;
            }
            above = below;
        }
        return std::numeric_limits<i32>::max();
    }

    IntProviderRef count_;
};

/// Walk up or down until a target block is found, giving up after so many
/// steps or as soon as the way is blocked.
class EnvironmentScan final : public PlacementModifier {
public:
    EnvironmentScan(bool upwards, i32 max_steps, BlockPredicateRef target,
                    BlockPredicateRef allowed)
        : upwards_(upwards),
          max_steps_(max_steps),
          target_(std::move(target)),
          allowed_(std::move(allowed)) {}

    void positions(const FeatureContext&, const FeatureLevel& level,
                   FeatureRandom&, BlockPos at,
                   std::vector<BlockPos>& out) const override {
        if (!allowed_->test(level, at)) {
            return;
        }
        const i32      step = upwards_ ? 1 : -1;
        BlockPos here = at;
        for (i32 taken = 0; taken < max_steps_; ++taken) {
            if (target_->test(level, here)) {
                out.push_back(here);
                return;
            }
            here.y += step;
            if (level.outside_build_height(here.y)) {
                return;
            }
            if (!allowed_->test(level, here)) {
                break;
            }
        }
        // One last look where the walk stopped: the block that blocked the way
        // is often the one being looked for.
        if (target_->test(level, here)) {
            out.push_back(here);
        }
    }
    [[nodiscard]] std::string_view name() const override { return "environment_scan"; }

private:
    bool              upwards_;
    i32               max_steps_;
    BlockPredicateRef target_;
    BlockPredicateRef allowed_;
};

}  // namespace

// ── Parsing ─────────────────────────────────────────────────────────────────

std::expected<VerticalAnchor, FeatureError> parse_anchor(Json node) {
    const std::array<std::pair<const char*, VerticalAnchor::Kind>, 3> kinds{
        {{"absolute", VerticalAnchor::Kind::Absolute},
         {"above_bottom", VerticalAnchor::Kind::AboveBottom},
         {"below_top", VerticalAnchor::Kind::BelowTop}}};
    for (const auto& [key, kind] : kinds) {
        i64 value = 0;
        if (node.at_key(key).get(value) == simdjson::SUCCESS) {
            return VerticalAnchor{kind, static_cast<i32>(value)};
        }
    }
    OV_LOG_ERROR("worldgen: a vertical anchor names none of absolute, above_bottom, below_top");
    return std::unexpected(FeatureError::Malformed);
}

std::expected<IntProviderRef, FeatureError> parse_int_provider(Json node) {
    const auto wrap = [](auto pointer) {
        return std::static_pointer_cast<const IntProvider>(std::move(pointer));
    };

    // A bare number is a constant, which is how most of the data spells one.
    i64 plain = 0;
    if (node.get(plain) == simdjson::SUCCESS) {
        return wrap(std::make_shared<const ConstantInt>(static_cast<i32>(plain)));
    }

    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string kind = strip_namespace(type);

    // The bounds live under "value" for some providers and at the top level for
    // others; both spellings appear in the vanilla data.
    auto       scope  = node.at_key("value");
    const Json holder = scope.error() == simdjson::SUCCESS ? scope.value_unsafe() : node;
    const auto integer = [&](const char* key, i64 fallback) {
        i64 value = fallback;
        (void)holder.at_key(key).get(value);
        return static_cast<i32>(value);
    };
    const auto number = [&](const char* key, f64 fallback) {
        f64 value = fallback;
        (void)holder.at_key(key).get(value);
        return static_cast<f32>(value);
    };

    if (kind == "constant") {
        return wrap(std::make_shared<const ConstantInt>(integer("value", 0)));
    }
    if (kind == "uniform") {
        return wrap(std::make_shared<const UniformInt>(integer("min_inclusive", 0),
                                                       integer("max_inclusive", 0)));
    }
    if (kind == "biased_to_bottom") {
        return wrap(std::make_shared<const BiasedToBottomInt>(integer("min_inclusive", 0),
                                                              integer("max_inclusive", 0)));
    }
    if (kind == "clamped_normal") {
        return wrap(std::make_shared<const ClampedNormalInt>(
            number("mean", 0.0), number("deviation", 1.0), integer("min_inclusive", 0),
            integer("max_inclusive", 0)));
    }
    if (kind == "clamped") {
        // `source` sits next to the bounds, inside "value" — not at the top.
        // Looking for it at the top failed silently and cost `forest_flowers`
        // and `flower_forest_flowers` (features-2).
        auto source = holder.at_key("source");
        if (source.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto inner = parse_int_provider(source.value());
        if (!inner) {
            return inner;
        }
        return wrap(std::make_shared<const ClampedInt>(*inner, integer("min_inclusive", 0),
                                                       integer("max_inclusive", 0)));
    }
    if (kind == "weighted_list") {
        simdjson::dom::array list;
        if (node.at_key("distribution").get(list) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        std::vector<std::pair<i32, i32>> entries;
        i32                              total = 0;
        for (auto member : list) {
            i64 weight = 0;
            if (member.at_key("weight").get(weight) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            auto data = member.at_key("data");
            if (data.error() != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            // The datapack allows a whole provider here; every vanilla use is a
            // plain number, and anything else is refused rather than guessed.
            i64 value = 0;
            if (data.get(value) != simdjson::SUCCESS) {
                OV_LOG_ERROR("worldgen: a weighted_list entry is not a plain number");
                return std::unexpected(FeatureError::Unsupported);
            }
            entries.emplace_back(static_cast<i32>(value), static_cast<i32>(weight));
            total += static_cast<i32>(weight);
        }
        if (total <= 0) {
            return std::unexpected(FeatureError::Malformed);
        }
        return wrap(std::make_shared<const WeightedListInt>(std::move(entries), total));
    }

    OV_LOG_ERROR("worldgen: int provider '{}' is not implemented", kind);
    return std::unexpected(FeatureError::Unsupported);
}

std::expected<HeightProviderRef, FeatureError> parse_height_provider(Json node) {
    const auto wrap = [](auto pointer) {
        return std::static_pointer_cast<const HeightProvider>(std::move(pointer));
    };

    // A bare anchor is a constant height, which is how the shorthand
    // `{"above_bottom": 8}` reads.
    if (node.at_key("type").error() != simdjson::SUCCESS) {
        auto anchor = parse_anchor(node);
        if (!anchor) {
            return std::unexpected(anchor.error());
        }
        return wrap(std::make_shared<const ConstantHeight>(*anchor));
    }

    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string kind = strip_namespace(type);

    const auto bound = [&](const char* key) -> std::expected<VerticalAnchor, FeatureError> {
        auto field = node.at_key(key);
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        return parse_anchor(field.value());
    };
    const auto integer = [&](const char* key, i64 fallback) {
        i64 value = fallback;
        (void)node.at_key(key).get(value);
        return static_cast<i32>(value);
    };

    if (kind == "constant") {
        auto field = node.at_key("value");
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto anchor = parse_anchor(field.value());
        if (!anchor) {
            return std::unexpected(anchor.error());
        }
        return wrap(std::make_shared<const ConstantHeight>(*anchor));
    }
    if (kind == "uniform" || kind == "trapezoid" || kind == "biased_to_bottom" ||
        kind == "very_biased_to_bottom") {
        auto low  = bound("min_inclusive");
        auto high = bound("max_inclusive");
        if (!low) return std::unexpected(low.error());
        if (!high) return std::unexpected(high.error());
        if (kind == "uniform") {
            return wrap(std::make_shared<const UniformHeight>(*low, *high));
        }
        if (kind == "trapezoid") {
            return wrap(std::make_shared<const TrapezoidHeight>(*low, *high, integer("plateau", 0)));
        }
        if (kind == "biased_to_bottom") {
            return wrap(
                std::make_shared<const BiasedToBottomHeight>(*low, *high, integer("inner", 1)));
        }
        return wrap(
            std::make_shared<const VeryBiasedToBottomHeight>(*low, *high, integer("inner", 1)));
    }

    OV_LOG_ERROR("worldgen: height provider '{}' is not implemented", kind);
    return std::unexpected(FeatureError::Unsupported);
}

namespace {

[[nodiscard]] std::expected<world::HeightmapType, FeatureError> read_heightmap(Json node,
                                                                               const char* key) {
    std::string_view name;
    if (node.at_key(key).get(name) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    if (const auto type = world::heightmap_type_from(name)) {
        return *type;
    }
    OV_LOG_ERROR("worldgen: unknown heightmap '{}'", name);
    return std::unexpected(FeatureError::Malformed);
}

}  // namespace

std::expected<PlacementModifierRef, FeatureError> parse_placement_modifier(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string kind = strip_namespace(type);
    const auto        wrap = [](auto pointer) {
        return std::static_pointer_cast<const PlacementModifier>(std::move(pointer));
    };
    const auto integer = [&](const char* key, i64 fallback) {
        i64 value = fallback;
        (void)node.at_key(key).get(value);
        return static_cast<i32>(value);
    };
    const auto provider = [&](const char* key) -> std::expected<IntProviderRef, FeatureError> {
        auto field = node.at_key(key);
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        return parse_int_provider(field.value());
    };
    const auto predicate = [&](const char* key) -> std::expected<BlockPredicateRef, FeatureError> {
        auto field = node.at_key(key);
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        return parse_block_predicate(field.value(), blocks, tags);
    };

    if (kind == "count") {
        auto count = provider("count");
        if (!count) return std::unexpected(count.error());
        return wrap(std::make_shared<const CountPlacement>(*count));
    }
    if (kind == "rarity_filter") {
        const i32 chance = integer("chance", 1);
        if (chance <= 0) {
            return std::unexpected(FeatureError::Malformed);
        }
        return wrap(std::make_shared<const RarityFilter>(chance));
    }
    if (kind == "in_square") {
        return wrap(std::make_shared<const InSquare>());
    }
    if (kind == "height_range") {
        auto field = node.at_key("height");
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto height = parse_height_provider(field.value());
        if (!height) return std::unexpected(height.error());
        return wrap(std::make_shared<const HeightRange>(*height));
    }
    if (kind == "biome") {
        return wrap(std::make_shared<const BiomeFilter>());
    }
    if (kind == "heightmap") {
        auto type_of = read_heightmap(node, "heightmap");
        if (!type_of) return std::unexpected(type_of.error());
        return wrap(std::make_shared<const HeightmapPlacement>(*type_of));
    }
    if (kind == "block_predicate_filter") {
        auto test = predicate("predicate");
        if (!test) return std::unexpected(test.error());
        return wrap(std::make_shared<const PredicateFilter>(*test));
    }
    if (kind == "surface_water_depth_filter") {
        return wrap(std::make_shared<const SurfaceWaterDepthFilter>(integer("max_water_depth", 0)));
    }
    if (kind == "surface_relative_threshold_filter") {
        auto type_of = read_heightmap(node, "heightmap");
        if (!type_of) return std::unexpected(type_of.error());
        // The defaults are the extremes of an i32, so an absent bound really
        // does mean "no bound on that side" rather than zero.
        return wrap(std::make_shared<const SurfaceRelativeThreshold>(
            *type_of, integer("min_inclusive", std::numeric_limits<i32>::min()),
            integer("max_inclusive", std::numeric_limits<i32>::max())));
    }
    if (kind == "random_offset") {
        auto xz = provider("xz_spread");
        auto y  = provider("y_spread");
        if (!xz) return std::unexpected(xz.error());
        if (!y) return std::unexpected(y.error());
        return wrap(std::make_shared<const RandomOffset>(*xz, *y));
    }
    if (kind == "count_on_every_layer") {
        auto count = provider("count");
        if (!count) return std::unexpected(count.error());
        return wrap(std::make_shared<const CountOnEveryLayer>(*count));
    }
    if (kind == "environment_scan") {
        std::string_view direction;
        if (node.at_key("direction_of_search").get(direction) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        if (direction != "up" && direction != "down") {
            OV_LOG_ERROR("worldgen: environment_scan searches '{}', which is neither up nor down",
                         direction);
            return std::unexpected(FeatureError::Malformed);
        }
        auto target = predicate("target_condition");
        if (!target) return std::unexpected(target.error());
        auto allowed = node.at_key("allowed_search_condition").error() == simdjson::SUCCESS
                           ? predicate("allowed_search_condition")
                           : std::expected<BlockPredicateRef, FeatureError>{
                                 std::static_pointer_cast<const BlockPredicate>(
                                     std::make_shared<const Combined>(
                                         Combine::AlwaysTrue, std::vector<BlockPredicateRef>{}))};
        if (!allowed) return std::unexpected(allowed.error());
        return wrap(std::make_shared<const EnvironmentScan>(direction == "up",
                                                            integer("max_steps", 1), *target,
                                                            *allowed));
    }

    // `noise_threshold_count` and `noise_based_count` both read
    // Biome.BIOME_INFO_NOISE — a PerlinSimplexNoise seeded from the fixed seed
    // 2345, which is a different noise family from anything in noise.cpp and
    // has no oracle here: no feature this module can place uses it, so an
    // implementation of it could not be checked against the game and would be
    // a plausible number nobody had measured. `carving_mask` needs the carvers,
    // which are not built yet. All three are refused by name so that the
    // placed features using them are visibly absent rather than quietly wrong.
    if (auto noise = parse_noise_placement(kind, node)) {  // features-2: BIOME_INFO_NOISE
        return std::move(*noise);
    }
    OV_LOG_ERROR("worldgen: placement modifier '{}' is not implemented", kind);
    return std::unexpected(FeatureError::Unsupported);
}

// ── Running a pipeline ──────────────────────────────────────────────────────

namespace {

void expand_stage(const std::vector<PlacementModifierRef>& pipeline, usize stage,
                  std::vector<std::vector<BlockPos>>& scratch,
                  const FeatureContext& context, const FeatureLevel& level,
                  FeatureRandom& random, BlockPos at,
                  const std::function<void(BlockPos)>& sink) {
    if (stage >= pipeline.size()) {
        sink(at);
        return;
    }
    // One scratch buffer per stage. Safe because the walk is depth first: at
    // any moment there is exactly one live iteration per stage.
    std::vector<BlockPos>& produced = scratch[stage];
    produced.clear();
    pipeline[stage]->positions(context, level, random, at, produced);
    // Indexed rather than iterated: a deeper stage cannot touch this buffer,
    // but a reallocation of it during the walk would be silent and fatal, and
    // an index survives one.
    for (usize index = 0; index < produced.size(); ++index) {
        const BlockPos next = produced[index];
        expand_stage(pipeline, stage + 1, scratch, context, level, random, next, sink);
    }
}

}  // namespace

void expand(const std::vector<PlacementModifierRef>& pipeline, const FeatureContext& context,
            const FeatureLevel& level, FeatureRandom& random, BlockPos origin,
            const std::function<void(BlockPos)>& sink) {
    std::vector<std::vector<BlockPos>> scratch(pipeline.size());
    expand_stage(pipeline, 0, scratch, context, level, random, origin, sink);
}

}  // namespace ov::worldgen
