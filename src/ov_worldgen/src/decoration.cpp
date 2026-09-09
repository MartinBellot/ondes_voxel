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
    const FeatureRegistry& features) {
    const auto directory = data_root / "worldgen" / "biome";
    if (!std::filesystem::is_directory(directory)) {
        OV_LOG_ERROR("worldgen: {} is missing", directory.string());
        return std::unexpected(FeatureError::Missing);
    }

    Decorator decorator;
    Impl&     impl = *decorator.impl_;
    impl.blocks    = &blocks;
    impl.features  = &features;

    // Sorted, and that is load-bearing rather than tidy: two features that
    // never share a biome are unordered by the sorter's constraints, and the
    // tie falls to whichever biome was read first.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);

    simdjson::dom::parser                                 parser;
    std::vector<std::unique_ptr<simdjson::padded_string>> sources;
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
        auto&             lists = impl.biomes[biome];
        usize             step  = 0;
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
    // One graph per step. Each biome's list is a chain: the feature at position
    // i must come before the one at i + 1. A topological sort of the union of
    // those chains is an order every biome agrees with, and the index into it
    // is what seeds the feature.
    //
    // Kahn's algorithm rather than a depth-first walk, because the queue makes
    // the tie-break explicit: among the features that are ready, the one that
    // was seen first wins. "Seen first" is the sorted biome order above.
    for (usize step = 0; step < kDecorationStepCount; ++step) {
        std::vector<std::string>                      seen;
        std::unordered_map<std::string, usize>        rank;
        std::unordered_map<usize, std::set<usize>>    after;
        std::unordered_map<usize, usize>              incoming;

        const auto intern = [&](const std::string& name) {
            const auto found = rank.find(name);
            if (found != rank.end()) {
                return found->second;
            }
            const usize id = seen.size();
            seen.push_back(name);
            rank.emplace(name, id);
            incoming.emplace(id, 0);
            return id;
        };

        for (const auto& [biome, lists] : impl.biomes) {
            const auto& list = lists[step];
            for (usize position = 0; position + 1 < list.size(); ++position) {
                const usize from = intern(list[position]);
                const usize to   = intern(list[position + 1]);
                if (from != to && after[from].insert(to).second) {
                    ++incoming[to];
                }
            }
            if (list.size() == 1) {
                (void)intern(list.front());
            }
        }

        std::vector<usize> ready;
        for (usize id = 0; id < seen.size(); ++id) {
            if (incoming[id] == 0) {
                ready.push_back(id);
            }
        }
        std::vector<std::string>& order = impl.order[step];
        // A stable front-of-queue pick, kept sorted by first-seen id so that
        // the tie-break is the biome reading order and not a hash order.
        while (!ready.empty()) {
            std::ranges::sort(ready);
            const usize chosen = ready.front();
            ready.erase(ready.begin());
            order.push_back(seen[chosen]);
            for (const usize next : after[chosen]) {
                if (--incoming[next] == 0) {
                    ready.push_back(next);
                }
            }
        }
        if (order.size() != seen.size()) {
            // A cycle: two biomes disagree about the order of two features.
            // Vanilla logs and gives up on the step; here it is fatal, because
            // a partial order would seed every feature after the cycle wrongly
            // and nothing downstream could tell.
            OV_LOG_ERROR("worldgen: the feature order at step {} has a cycle ({} of {} placed)",
                         to_string(static_cast<DecorationStep>(step)), order.size(), seen.size());
            return std::unexpected(FeatureError::Malformed);
        }
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
