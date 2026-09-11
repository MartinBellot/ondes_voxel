#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/decoration.hpp"

#include "feature_json.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ov::worldgen {

std::string_view to_string(DecorationStep step) noexcept {
    switch (step) {
        case DecorationStep::RawGeneration:
            return "raw_generation";
        case DecorationStep::Lakes:
            return "lakes";
        case DecorationStep::LocalModifications:
            return "local_modifications";
        case DecorationStep::UndergroundStructures:
            return "underground_structures";
        case DecorationStep::SurfaceStructures:
            return "surface_structures";
        case DecorationStep::Strongholds:
            return "strongholds";
        case DecorationStep::UndergroundOres:
            return "underground_ores";
        case DecorationStep::UndergroundDecoration:
            return "underground_decoration";
        case DecorationStep::FluidSprings:
            return "fluid_springs";
        case DecorationStep::VegetalDecoration:
            return "vegetal_decoration";
        case DecorationStep::TopLayerModification:
            return "top_layer_modification";
    }
    return "unknown";
}

i64 decoration_seed(i64 level_seed, i32 chunk_min_x, i32 chunk_min_z,
                    FeatureRandom::Kind kind) noexcept {
    // Seeded from the world seed, two draws, both forced odd. Forcing the low
    // bit is not decoration: an even multiplier throws away a bit of the
    // coordinate at each doubling, which would make chunks 2^k apart share a
    // decoration seed.
    // OV_DECORATION_SEED reaches the variants of this formula that were tried
    // against the reference world. It exists for the same reason
    // OV_NO_INTERPOLATION does: the formula is a claim about the game, and a
    // claim in this repository is a thing somebody ran.
    const char*            spelling = std::getenv("OV_DECORATION_SEED");
    const std::string_view variant  = spelling == nullptr ? "xor" : spelling;

    // OV_DECORATION_RANDOM separates the family the two multipliers come from
    // from the one the features draw with. In the game they are one object, so
    // the two always agree; separable here only so that the pair could be
    // measured rather than assumed.
    const char*  deco_choice = std::getenv("OV_DECORATION_RANDOM");
    const auto   deco_kind =
        deco_choice == nullptr
              ? kind
              : (std::string_view(deco_choice) == "legacy" ? FeatureRandom::Kind::Legacy
                                                           : FeatureRandom::Kind::Xoroshiro);
    FeatureRandom source{deco_kind, level_seed};
    i64           a = source.next_long();
    i64           b = source.next_long();
    if (variant != "even") {
        a |= 1;
        b |= 1;
    }
    const i64 x = variant == "chunk" ? chunk_min_x >> 4 : chunk_min_x;
    const i64 z = variant == "chunk" ? chunk_min_z >> 4 : chunk_min_z;
    // The multiplies wrap, deliberately: this is a mixing step and not an
    // arithmetic one, and a chunk at the world border depends on it wrapping.
    const auto mixed = static_cast<i64>(static_cast<u64>(x) * static_cast<u64>(a) +
                                        static_cast<u64>(z) * static_cast<u64>(b));
    if (variant == "plus") {
        return static_cast<i64>(static_cast<u64>(mixed) + static_cast<u64>(level_seed));
    }
    if (variant == "bare") {
        return mixed;
    }
    return mixed ^ level_seed;
}

i64 feature_seed(i64 decoration, i32 index, i32 step) noexcept {
    // 10000 apart per step, so a step with fewer than ten thousand features
    // cannot reach into the next one's seeds. Every step in vanilla has at most
    // a few dozen.
    return decoration + index + 10000LL * step;
}

namespace {

/// Which biomes list which placed features, in the form the `biome` modifier
/// asks about.
class BiomeFeatureIndex final : public BiomeFeatures {
public:
    void add(std::string biome, std::string feature) {
        listed_.emplace(std::move(biome) + "\n" + std::move(feature));
    }

    [[nodiscard]] bool lists(std::string_view biome, std::string_view feature) const override {
        std::string key;
        key.reserve(biome.size() + feature.size() + 1);
        key.append(biome).append("\n").append(feature);
        return listed_.contains(key);
    }

private:
    /// Biome and feature joined by a newline, which no resource location can
    /// contain. One set beats a map of sets here: the question is asked once
    /// per candidate position and the answer is a single hash.
    std::unordered_set<std::string> listed_;
};

}  // namespace

struct Decorator::Impl {
    const registry::BlockRegistry* blocks{nullptr};
    const FeatureRegistry*         features{nullptr};

    /// Per step, the shared ordering of every placed feature that appears at
    /// it. A feature's position in this list is the index that seeds it.
    std::array<std::vector<std::string>, kDecorationStepCount> order;
    /// The same, as a lookup.
    std::array<std::unordered_map<std::string, i32>, kDecorationStepCount> index;

    /// Per biome, per step, the features it lists — in the biome's own order,
    /// which is what the sorter's constraints come from.
    std::map<std::string, std::array<std::vector<std::string>, kDecorationStepCount>> biomes;

    BiomeFeatureIndex listed;

    /// Named by a biome and not built by the registry. Kept so the gap is a
    /// list rather than a silence.
    std::set<std::string> missing;
};

Decorator::Decorator() : impl_(std::make_unique<Impl>()) {}
Decorator::Decorator(Decorator&&) noexcept            = default;
Decorator& Decorator::operator=(Decorator&&) noexcept = default;
Decorator::~Decorator()                                = default;

std::expected<Decorator, FeatureError> Decorator::load(
    const std::filesystem::path& data_root, const registry::BlockRegistry& blocks,
    const FeatureRegistry& features, const BiomeSource& source) {
    const auto directory = data_root / "worldgen" / "biome";
    if (!std::filesystem::is_directory(directory)) {
        OV_LOG_ERROR("worldgen: {} is missing", directory.string());
        return std::unexpected(FeatureError::Missing);
    }

    Decorator decorator;
    Impl&     impl = *decorator.impl_;
    impl.blocks    = &blocks;
    impl.features  = &features;

    // The biomes this dimension can produce, in the order the biome source
    // yields them — first occurrence in the climate table, duplicates dropped.
    // That order is the sorter's tie-break, so it is not presentation: reading
    // the directory alphabetically instead puts `ore_copper` one index early
    // and moves every copper vein in the world.
    std::vector<std::string> biome_order;
    std::set<std::string>    already;
    for (usize entry = 0; entry < source.entry_count(); ++entry) {
        std::string name{source.entry_biome(entry)};
        if (already.insert(name).second) {
            biome_order.push_back(std::move(name));
        }
    }

    std::vector<std::filesystem::path> files;
    files.reserve(biome_order.size());
    for (const std::string& biome : biome_order) {
        const auto bare = biome.find(':') == std::string::npos
                              ? biome
                              : biome.substr(biome.find(':') + 1);
        auto       path = directory / (bare + ".json");
        if (!std::filesystem::is_regular_file(path)) {
            OV_LOG_ERROR("worldgen: the biome source names {}, which has no file at {}", biome,
                         path.string());
            return std::unexpected(FeatureError::Missing);
        }
        files.push_back(std::move(path));
    }

    simdjson::dom::parser                                 parser;
    std::vector<std::unique_ptr<simdjson::padded_string>> sources;
    // In biome_order, not sorted: `read_order` below records it.
    std::vector<std::string> read_order;
    for (const auto& path : files) {
        auto text = simdjson::padded_string::load(path.string());
        if (text.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Missing);
        }
        sources.push_back(std::make_unique<simdjson::padded_string>(std::move(text.value())));
        auto document = parser.parse(*sources.back());
        if (document.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        simdjson::dom::array steps;
        if (document.at_key("features").get(steps) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }

        const std::string biome = "minecraft:" + path.stem().string();
        read_order.push_back(biome);
        auto& lists = impl.biomes[biome];
        usize step  = 0;
        for (auto list : steps) {
            simdjson::dom::array members;
            if (list.get(members) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            if (step >= kDecorationStepCount) {
                OV_LOG_ERROR("worldgen: {} has more than {} decoration steps", biome,
                             kDecorationStepCount);
                return std::unexpected(FeatureError::Malformed);
            }
            for (auto member : members) {
                std::string_view name;
                if (member.get(name) != simdjson::SUCCESS) {
                    // A biome may inline a placed feature. Vanilla's own data
                    // never does, and reading half of one would be worse than
                    // saying so.
                    OV_LOG_ERROR("worldgen: {} inlines a placed feature at step {}", biome, step);
                    return std::unexpected(FeatureError::Unsupported);
                }
                const std::string qualified = qualify(name);
                lists[step].push_back(qualified);
                impl.listed.add(biome, qualified);
                if (features.placed(qualified) == nullptr) {
                    impl.missing.insert(qualified);
                }
            }
            ++step;
        }
    }

    // ── The shared ordering ─────────────────────────────────────────────────
    //
    // A node is one placed feature at one step. Two numbers order the nodes:
    // the step, then the position at which the *feature* was first met while
    // walking the biomes in the source's order — a single counter shared by
    // every step, not one per step.
    //
    // Each biome contributes a chain over its whole feature list flattened
    // across steps, so the last feature of a step points at the first of the
    // next. The chain says "here, this comes before that"; the union of every
    // biome's chains is the constraint the ordering has to satisfy.
    //
    // The sort is a depth-first walk whose **reverse post-order** is the
    // answer, not Kahn's algorithm — and the two disagree exactly where the
    // constraints do not decide. A depth-first walk pushes the successor it
    // enters first *deepest*, so that successor is appended to the post-order
    // first and therefore ends up *last* once it is reversed. Kahn's queue does
    // the opposite. `ore_copper` and `ore_copper_large` never share a biome, so
    // nothing orders them; the game seeds `ore_copper` with 24 and Kahn gives
    // it 23. That one index was worth 0.3 % of the copper in the world.
    struct SortNode {
        usize       step{0};
        usize       seen{0};
        std::string name;
    };
    using SortKey = std::pair<usize, usize>;  // (step, first-seen number)

    std::unordered_map<std::string, usize> first_seen;
    std::map<SortKey, usize>               node_of;
    std::vector<SortNode>                  nodes;
    std::vector<std::set<SortKey>>         after;

    const auto node_id = [&](usize step, const std::string& name) {
        auto seen = first_seen.find(name);
        if (seen == first_seen.end()) {
            const usize next = first_seen.size();
            seen             = first_seen.emplace(name, next).first;
        }
        const SortKey key{step, seen->second};
        const auto    found = node_of.find(key);
        if (found != node_of.end()) {
            return found->second;
        }
        const usize id = nodes.size();
        nodes.push_back({step, seen->second, name});
        after.emplace_back();
        node_of.emplace(key, id);
        return id;
    };

    for (const std::string& biome : read_order) {
        const auto&        lists = impl.biomes.at(biome);
        std::vector<usize> chain;
        for (usize step = 0; step < kDecorationStepCount; ++step) {
            for (const std::string& name : lists[step]) {
                chain.push_back(node_id(step, name));
            }
        }
        for (usize position = 0; position + 1 < chain.size(); ++position) {
            const SortNode& to = nodes[chain[position + 1]];
            after[chain[position]].insert(SortKey{to.step, to.seen});
        }
    }

    enum class Mark : u8 { None, OnStack, Done };
    struct Frame {
        usize                          node{0};
        std::set<SortKey>::const_iterator next;
    };

    std::vector<Mark>  mark(nodes.size(), Mark::None);
    std::vector<usize> post_order;
    post_order.reserve(nodes.size());
    // Roots in (step, first-seen) order: node_of is keyed by exactly that.
    for (const auto& [key, root] : node_of) {
        if (mark[root] != Mark::None) {
            continue;
        }
        std::vector<Frame> stack;
        mark[root] = Mark::OnStack;
        stack.push_back({root, after[root].begin()});
        while (!stack.empty()) {
            const usize here = stack.back().node;
            if (stack.back().next == after[here].end()) {
                mark[here] = Mark::Done;
                post_order.push_back(here);
                stack.pop_back();
                continue;
            }
            const usize child = node_of.at(*stack.back().next);
            ++stack.back().next;
            if (mark[child] == Mark::OnStack) {
                // Two biomes disagree about the order of two features. Vanilla
                // logs and falls back; here it is fatal, because a partial
                // order would seed everything after the cycle wrongly and
                // nothing downstream could tell.
                OV_LOG_ERROR("worldgen: the feature order has a cycle at {} (step {})",
                             nodes[child].name,
                             to_string(static_cast<DecorationStep>(nodes[child].step)));
                return std::unexpected(FeatureError::Malformed);
            }
            if (mark[child] != Mark::None) {
                continue;
            }
            mark[child] = Mark::OnStack;
            stack.push_back({child, after[child].begin()});
        }
    }

    for (auto position = post_order.rbegin(); position != post_order.rend(); ++position) {
        const SortNode& node = nodes[*position];
        impl.order[node.step].push_back(node.name);
    }
    for (usize step = 0; step < kDecorationStepCount; ++step) {
        const auto& order = impl.order[step];
        for (usize position = 0; position < order.size(); ++position) {
            impl.index[step].emplace(order[position], static_cast<i32>(position));
        }
    }

    usize ordered = 0;
    for (const auto& list : impl.order) {
        ordered += list.size();
    }
    OV_LOG_INFO("worldgen: {} biomes, {} placed features ordered across {} steps, {} of them named "
                "but not built",
                impl.biomes.size(), ordered, kDecorationStepCount, impl.missing.size());
    return decorator;
}

std::vector<std::string_view> Decorator::order_at(DecorationStep step) const {
    std::vector<std::string_view> names;
    for (const std::string& name : impl_->order[static_cast<usize>(step)]) {
        names.emplace_back(name);
    }
    return names;
}

i32 Decorator::index_of(DecorationStep step, std::string_view feature) const {
    const auto& table = impl_->index[static_cast<usize>(step)];
    const auto  found = table.find(qualify(feature));
    return found == table.end() ? -1 : found->second;
}

const BiomeFeatures& Decorator::biome_features() const noexcept {
    return impl_->listed;
}

usize Decorator::missing_count() const noexcept {
    return impl_->missing.size();
}

std::vector<std::string> Decorator::missing() const {
    return {impl_->missing.begin(), impl_->missing.end()};
}

usize Decorator::biome_count() const noexcept {
    return impl_->biomes.size();
}

usize Decorator::decorate(FeatureLevel& level, i32 chunk_x, i32 chunk_z, i64 level_seed) const {
    const i32  origin_x = chunk_x * 16;
    const i32  origin_z = chunk_z * 16;
    const auto kind     = configured_feature_random();
    const i64  seed     = decoration_seed(level_seed, origin_x, origin_z, kind);

    // Every biome in the chunk and in the eight around it. A feature listed by
    // a neighbour is *attempted* here and then thrown away by the `biome`
    // modifier if it lands outside that biome — which is how a forest's trees
    // stop exactly at the desert's edge instead of at the chunk's.
    std::set<std::string_view> nearby;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 base_x = origin_x + dx * 16;
            const i32 base_z = origin_z + dz * 16;
            for (i32 y = level.min_y(); y <= level.max_y(); y += 4) {
                for (i32 z = 0; z < 16; z += 4) {
                    for (i32 x = 0; x < 16; x += 4) {
                        nearby.insert(level.biome_at(base_x + x, y, base_z + z));
                    }
                }
            }
        }
    }

    usize placed = 0;
    for (usize step = 0; step < kDecorationStepCount; ++step) {
        // The features of every nearby biome at this step, as a set of indices
        // into the shared order, then run in ascending index. Ascending, and
        // not in any biome's own order: that is the whole point of the shared
        // ordering, and it is why a chunk on a border generates the same as one
        // in the middle.
        std::set<i32> wanted;
        for (const std::string_view biome : nearby) {
            const auto found = impl_->biomes.find(std::string(biome));
            if (found == impl_->biomes.end()) {
                continue;
            }
            for (const std::string& feature : found->second[step]) {
                const auto index = impl_->index[step].find(feature);
                if (index != impl_->index[step].end()) {
                    wanted.insert(index->second);
                }
            }
        }

        // The index that seeds a feature counts *every* feature run at this
        // step, including the ones this interpreter cannot build. Skipping one
        // without counting it would renumber all the rest and move them.
        //
        // Structures also consume indices at the steps they belong to; none
        // belongs to underground_ores, which is why the ore parity below is not
        // waiting on the structure work.
        for (const i32 index : wanted) {
            const std::string&   name = impl_->order[step][static_cast<usize>(index)];
            const PlacedFeature* feature = impl_->features->placed(name);
            if (feature == nullptr) {
                continue;
            }
            FeatureRandom random{kind, feature_seed(seed, index, static_cast<i32>(step))};

            FeatureContext context;
            context.blocks       = impl_->blocks;
            context.feature_name = name;
            context.biomes       = &impl_->listed;
            context.level_seed   = level_seed;  // ── end ──

            bool wrote = false;
            // The origin is the bottom corner of the chunk, floor included.
            // Every ore replaces its y anyway, but a feature that keeps it —
            // anything reached through `heightmap` — would start from the wrong
            // place.
            expand(feature->placement, context, level, random,
                   {origin_x, level.min_y(), origin_z},
                   [&](BlockPos at) {
                       wrote |= feature->feature->place(context, level, random, at);
                   });
            if (wrote) {
                ++placed;
            }
        }
    }
    return placed;
}

}  // namespace ov::worldgen
