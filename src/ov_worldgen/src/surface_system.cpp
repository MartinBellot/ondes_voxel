#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/surface_system.hpp"

#include "ov/base/log.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace ov::worldgen {

namespace {

/// The density `initial_density_without_jaggedness` has to exceed for a cell to
/// count as the terrain's coarse top. Not a round number and not a guess: it is
/// 25/64, and it is the threshold the preliminary surface is defined by.
constexpr f64 kPreliminarySurfaceThreshold = 0.390625;

/// The scale and the offset the surface noise is turned into a depth by.
///
/// Range: the noise sits in about [-1, 1], so the depth lands in 0..5 — three
/// or four blocks of dirt under most grass, and the occasional zero that the
/// `hole` condition turns into bare stone.
constexpr f64 kSurfaceDepthScale  = 2.75;
constexpr f64 kSurfaceDepthOffset = 3.0;
constexpr f64 kSurfaceDepthJitter = 0.25;

[[nodiscard]] std::string strip_namespace(std::string_view name) {
    const auto colon = name.find(':');
    return std::string(colon == std::string_view::npos ? name : name.substr(colon + 1));
}

}  // namespace

struct SurfaceSystem::Impl final : public SurfaceResources {
    std::filesystem::path root;

    const registry::BlockRegistry* blocks{nullptr};

    /// Forked from the world seed exactly as NoiseRouter::load forks its own.
    /// Two factories from the same seed are the same factory, which is the
    /// point: vanilla has one, and a noise named here and named there has to
    /// come out identical.
    ///
    /// ── nether ── Legacy when the settings say `legacy_random_source`, as the
    /// router's is: the Nether's surface noises, its bedrock gradients and its
    /// per-column depth all come from a `java.util.Random` factory.
    PositionalRandomFactory factory{math::XoroshiroPositionalFactory{0, 0}};

    std::unordered_map<std::string, std::shared_ptr<const NormalNoise>> noises;

    SurfaceRuleRef rule;

    const NormalNoise* surface_noise{nullptr};
    const NormalNoise* surface_secondary_noise{nullptr};
    const NormalNoise* clay_bands_offset_noise{nullptr};

    std::array<registry::BlockStateId, kClayBandCount> clay_bands{};

    registry::BlockStateId default_block{};

    i32 min_y{-64};
    i32 height{384};
    i32 cell_height{8};
    i32 sea_level{63};

    /// Whether the per-column depth carries the extra quarter-block of jitter
    /// drawn from a positional generator. Switchable so the question can be
    /// settled by measurement against the reference world rather than argued
    /// about — the same trick density.cpp uses for the cave noise.
    bool depth_jitter{true};

    /// Whether `stone_depth(ceiling)` measures the run of stone below the
    /// position, or is simply always satisfied. A switch rather than a
    /// decision, because the two give visibly different beaches and the
    /// reference world is the only thing that can say which is the game's.
    bool ceiling_depth_from_run{true};

    [[nodiscard]] std::expected<std::shared_ptr<const NormalNoise>, SurfaceError> load_noise(
        std::string_view name);

    [[nodiscard]] i32 surface_depth_at(i32 x, i32 z) const {
        f64 value = surface_noise == nullptr
                        ? 0.0
                        : surface_noise->value(static_cast<f64>(x), 0.0, static_cast<f64>(z));
        value = value * kSurfaceDepthScale + kSurfaceDepthOffset;
        if (depth_jitter) {
            value += factory.next_double_at(x, 0, z) * kSurfaceDepthJitter;
        }
        // Truncation towards zero, as the cast to int is. The distinction only
        // shows for a negative depth, and a negative depth is exactly what the
        // `hole` condition looks for.
        return static_cast<i32>(value);
    }

    [[nodiscard]] f64 secondary_depth_at(i32 x, i32 z) const {
        return surface_secondary_noise == nullptr
                   ? 0.0
                   : surface_secondary_noise->value(static_cast<f64>(x), 0.0,
                                                    static_cast<f64>(z));
    }

    // ── SurfaceResources ────────────────────────────────────────────────────

    [[nodiscard]] const NormalNoise* noise(std::string_view name) override {
        auto built = load_noise(name);
        return built ? built->get() : nullptr;
    }

    [[nodiscard]] PositionalRandomFactory random_factory(std::string_view name) override {
        // A factory under a name is a fork of the generator that name hashes
        // to, not that generator itself. One level of forking either way gives
        // a different bedrock floor, and the floor is checkable a block at a
        // time — which is how this was settled.
        return factory.fork_named(name);
    }

    [[nodiscard]] std::optional<registry::BlockStateId> block_state(
        std::string_view                                               name,
        std::span<const std::pair<std::string_view, std::string_view>> properties) override {
        const auto block = blocks->find_block(name);
        if (!block) {
            return std::nullopt;
        }
        if (properties.empty()) {
            return blocks->default_state(*block);
        }
        return blocks->state_for(*block, properties);
    }

    [[nodiscard]] registry::BlockStateId clay_band(i32 x, i32 y, i32 z) const override {
        // The whole stack is shifted up or down by a noise, so two badlands
        // hills a hundred blocks apart do not have their stripes at the same
        // heights.
        const f64 raw = clay_bands_offset_noise == nullptr
                            ? 0.0
                            : clay_bands_offset_noise->value(static_cast<f64>(x), 0.0,
                                                             static_cast<f64>(z));
        const auto offset = static_cast<i32>(std::llround(raw * 4.0));
        const auto count  = static_cast<i32>(kClayBandCount);
        // The extra `+ count` before the modulo is not decoration: y is
        // negative for two thirds of the world and C++ leaves the sign of a
        // negative modulo to the dividend.
        const i32 index = ((y + offset) % count + count) % count;
        return clay_bands[static_cast<usize>(index)];
    }
};

std::expected<std::shared_ptr<const NormalNoise>, SurfaceError> SurfaceSystem::Impl::load_noise(
    std::string_view name) {
    const std::string key(name);
    if (const auto found = noises.find(key); found != noises.end()) {
        return found->second;
    }

    const auto path = root / "worldgen" / "noise" / (strip_namespace(name) + ".json");
    if (!std::filesystem::is_regular_file(path)) {
        OV_LOG_ERROR("worldgen: {} is missing", path.string());
        return std::unexpected(SurfaceError::Missing);
    }
    auto text = simdjson::padded_string::load(path.string());
    if (text.error() != simdjson::SUCCESS) {
        return std::unexpected(SurfaceError::Malformed);
    }
    simdjson::dom::parser parser;
    auto                  document = parser.parse(text.value());
    if (document.error() != simdjson::SUCCESS) {
        return std::unexpected(SurfaceError::Malformed);
    }
    i64 first_octave = 0;
    if (document.at_key("firstOctave").get(first_octave) != simdjson::SUCCESS) {
        return std::unexpected(SurfaceError::Malformed);
    }
    simdjson::dom::array raw;
    if (document.at_key("amplitudes").get(raw) != simdjson::SUCCESS) {
        return std::unexpected(SurfaceError::Malformed);
    }
    std::vector<f64> amplitudes;
    for (auto value : raw) {
        f64 amplitude = 0.0;
        if (value.get(amplitude) != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        amplitudes.push_back(amplitude);
    }

    auto built = std::make_shared<const NormalNoise>(
        factory.normal_noise(name, static_cast<i32>(first_octave), amplitudes));
    noises.emplace(key, built);
    return built;
}

namespace {

/// What the rules are told about a chunk of ours.
///
/// The heights are taken from the column contents rather than from a stored
/// heightmap, because at this point in generation WORLD_SURFACE_WG *is* the
/// highest non-air block and nothing else has touched the chunk. Reading a
/// stored map would make this depend on which maps the noise stage happened to
/// maintain.
class ChunkQueries final : public SurfaceQueries {
public:
    ChunkQueries(const world::Chunk& chunk, const registry::BlockRegistry& blocks,
                 const NoiseRouter& router, const std::array<i32, 256>& heights)
        : chunk_(&chunk),
          blocks_(&blocks),
          initial_density_(router.entry("initial_density_without_jaggedness")),
          heights_(&heights),
          origin_x_(chunk.position().x * 16),
          origin_z_(chunk.position().z * 16),
          min_y_(router.min_y()),
          height_(router.height()),
          cell_height_(router.cell_height()) {
        if (initial_density_ == nullptr) {
            OV_LOG_WARN(
                "worldgen: the router has no initial_density_without_jaggedness; the surface "
                "rules will see no preliminary surface and place nothing");
        }
    }

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        const auto local_x = static_cast<usize>(x - origin_x_);
        const auto local_z = static_cast<usize>(z - origin_z_);
        return blocks_->biome_name(chunk_->get_biome(local_x, y, local_z));
    }

    [[nodiscard]] f64 temperature_at(i32 x, i32 y, i32 z) const override {
        const auto local_x = static_cast<usize>(x - origin_x_);
        const auto local_z = static_cast<usize>(z - origin_z_);
        return blocks_->biome(chunk_->get_biome(local_x, y, local_z)).temperature;
    }

    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override {
        const usize local_x = static_cast<usize>(x - origin_x_) & 15U;
        const usize local_z = static_cast<usize>(z - origin_z_) & 15U;
        return (*heights_)[local_z * 16 + local_x];
    }

    [[nodiscard]] i32 preliminary_surface(i32 x, i32 z) const override {
        if (initial_density_ == nullptr) {
            return kNoSurface;
        }
        // Per quarter-block column, and evaluated at the column's origin — the
        // same quantisation the density graph's flat_cache uses, and for the
        // same reason: the game asks this question once per quart and reuses
        // the answer across the four blocks.
        const i32 quart_x = (x >> 2) << 2;
        const i32 quart_z = (z >> 2) << 2;
        const u64 key     = (static_cast<u64>(static_cast<u32>(quart_x)) << 32) |
                        static_cast<u64>(static_cast<u32>(quart_z));
        if (const auto found = cache_.find(key); found != cache_.end()) {
            return found->second;
        }
        i32 level = kNoSurface;
        for (i32 y = min_y_ + height_; y >= min_y_; y -= cell_height_) {
            if (initial_density_->compute(FunctionContext{quart_x, y, quart_z}) >
                kPreliminarySurfaceThreshold) {
                level = y;
                break;
            }
        }
        cache_.emplace(key, level);
        return level;
    }

private:
    const world::Chunk*            chunk_;
    const registry::BlockRegistry* blocks_;
    const DensityFunction*         initial_density_;
    const std::array<i32, 256>*    heights_;
    i32                            origin_x_;
    i32                            origin_z_;
    i32                            min_y_;
    i32                            height_;
    i32                            cell_height_;

    mutable std::unordered_map<u64, i32> cache_;
};

}  // namespace

SurfaceSystem::SurfaceSystem() : impl_(std::make_unique<Impl>()) {}
SurfaceSystem::SurfaceSystem(SurfaceSystem&&) noexcept            = default;
SurfaceSystem& SurfaceSystem::operator=(SurfaceSystem&&) noexcept = default;
SurfaceSystem::~SurfaceSystem()                                   = default;

std::expected<SurfaceSystem, SurfaceError> SurfaceSystem::load(
    const std::filesystem::path& data_root, std::string_view settings, i64 seed,
    const registry::BlockRegistry& blocks) {
    SurfaceSystem system;
    Impl&         impl = *system.impl_;
    impl.root          = data_root;
    impl.blocks        = &blocks;

    if (const char* setting = std::getenv("OV_SURFACE_DEPTH_JITTER"); setting != nullptr) {
        impl.depth_jitter = std::string_view(setting) != "0";
    }
    if (const char* setting = std::getenv("OV_CEILING_FROM_RUN"); setting != nullptr) {
        impl.ceiling_depth_from_run = std::string_view(setting) != "0";
    }

    const auto settings_file =
        data_root / "worldgen" / "noise_settings" / (std::string(settings) + ".json");
    if (!std::filesystem::is_regular_file(settings_file)) {
        OV_LOG_ERROR("worldgen: {} is missing", settings_file.string());
        return std::unexpected(SurfaceError::Missing);
    }
    {
        auto text = simdjson::padded_string::load(settings_file.string());
        if (text.error() != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        simdjson::dom::parser parser;
        auto                  document = parser.parse(text.value());
        if (document.error() != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        // ── nether ── Seeded only once the settings have said which generator
        // the dimension uses.
        bool legacy = false;
        (void)document.at_key("legacy_random_source").get(legacy);
        impl.factory = PositionalRandomFactory::for_world(seed, legacy);

        i64 value = 0;
        if (document.at_key("sea_level").get(value) == simdjson::SUCCESS) {
            impl.sea_level = static_cast<i32>(value);
        }
        auto shape = document.at_key("noise");
        if (shape.error() == simdjson::SUCCESS) {
            if (shape.at_key("min_y").get(value) == simdjson::SUCCESS) {
                impl.min_y = static_cast<i32>(value);
            }
            if (shape.at_key("height").get(value) == simdjson::SUCCESS) {
                impl.height = static_cast<i32>(value);
            }
            if (shape.at_key("size_vertical").get(value) == simdjson::SUCCESS) {
                impl.cell_height = static_cast<i32>(value) * 4;
            }
        }
        std::string_view default_name;
        auto             block = document.at_key("default_block");
        if (block.error() != simdjson::SUCCESS ||
            block.at_key("Name").get(default_name) != simdjson::SUCCESS) {
            return std::unexpected(SurfaceError::Malformed);
        }
        const auto found = blocks.find_block(default_name);
        if (!found) {
            OV_LOG_ERROR("worldgen: default_block '{}' is not in the registry", default_name);
            return std::unexpected(SurfaceError::UnknownBlock);
        }
        impl.default_block = blocks.default_state(*found);
    }

    // The three noises the stage itself samples, as opposed to the ones the
    // rules name. Loaded before the rule tree so that a missing file is
    // reported once rather than at the first badlands column generated.
    const std::array<std::pair<std::string_view, const NormalNoise**>, 3> own_noises{
        std::pair{std::string_view("minecraft:surface"), &impl.surface_noise},
        std::pair{std::string_view("minecraft:surface_secondary"), &impl.surface_secondary_noise},
        std::pair{std::string_view("minecraft:clay_bands_offset"), &impl.clay_bands_offset_noise}};
    for (const auto& [name, slot] : own_noises) {
        auto built = impl.load_noise(name);
        if (!built) {
            return std::unexpected(built.error());
        }
        *slot = built->get();
    }

    // The bands. Drawn from a generator seeded by the name alone, so the table
    // is the same in every chunk of a world and different in every world.
    {
        const auto colour = [&](std::string_view name) -> std::optional<registry::BlockStateId> {
            const auto block = blocks.find_block(name);
            if (!block) {
                OV_LOG_ERROR("worldgen: the registry has no '{}'", name);
                return std::nullopt;
            }
            return blocks.default_state(*block);
        };
        ClayBandColours colours;
        const std::array<std::pair<std::string_view, registry::BlockStateId*>, 7> wanted{
            std::pair{std::string_view("minecraft:terracotta"), &colours.terracotta},
            std::pair{std::string_view("minecraft:orange_terracotta"), &colours.orange},
            std::pair{std::string_view("minecraft:yellow_terracotta"), &colours.yellow},
            std::pair{std::string_view("minecraft:brown_terracotta"), &colours.brown},
            std::pair{std::string_view("minecraft:red_terracotta"), &colours.red},
            std::pair{std::string_view("minecraft:white_terracotta"), &colours.white},
            std::pair{std::string_view("minecraft:light_gray_terracotta"), &colours.light_gray}};
        for (const auto& [name, slot] : wanted) {
            auto state = colour(name);
            if (!state) {
                return std::unexpected(SurfaceError::UnknownBlock);
            }
            *slot = *state;
        }
        if (const auto* legacy_factory = impl.factory.legacy_factory()) {
            auto random     = legacy_factory->from_hash_of("minecraft:clay_bands");
            impl.clay_bands = generate_clay_bands(random, colours);
        } else {
            auto random     = impl.factory.xoroshiro()->from_hash_of("minecraft:clay_bands");
            impl.clay_bands = generate_clay_bands(random, colours);
        }
    }

    auto rule = load_surface_rule(settings_file, impl);
    if (!rule) {
        return std::unexpected(rule.error());
    }
    impl.rule = *rule;

    OV_LOG_INFO("worldgen: surface rules for {} loaded, {} noises, seed {}", settings,
                impl.noises.size(), seed);
    return system;
}

i32 SurfaceSystem::min_y() const noexcept {
    return impl_->min_y;
}
i32 SurfaceSystem::height() const noexcept {
    return impl_->height;
}
i32 SurfaceSystem::sea_level() const noexcept {
    return impl_->sea_level;
}
registry::BlockStateId SurfaceSystem::default_block() const noexcept {
    return impl_->default_block;
}
i32 SurfaceSystem::surface_depth(i32 x, i32 z) const {
    return impl_->surface_depth_at(x, z);
}
registry::BlockStateId SurfaceSystem::clay_band(i32 x, i32 y, i32 z) const {
    return impl_->clay_band(x, y, z);
}

void SurfaceSystem::build_column(std::span<registry::BlockStateId> column, i32 world_x,
                                 i32 world_z, const SurfaceQueries& queries,
                                 const registry::BlockRegistry& blocks) const {
    const Impl& impl = *impl_;
    if (impl.rule == nullptr || column.empty()) {
        return;
    }

    SurfaceContext at;
    at.queries         = &queries;
    at.x               = world_x;
    at.z               = world_z;
    at.min_y           = impl.min_y;
    at.height          = impl.height;
    at.surface_depth   = impl.surface_depth_at(world_x, world_z);
    at.secondary_depth = impl.secondary_depth_at(world_x, world_z);

    const auto size = static_cast<i32>(column.size());

    const auto is_air = [&](registry::BlockStateId state) {
        return state == registry::kAirState || blocks.is_air(blocks.block_of(state));
    };
    const auto is_fluid = [&](registry::BlockStateId state) {
        return blocks.holds_fluid(state);
    };

    // Depth counted from below, in one pass. Computing it on demand would mean
    // rescanning the column for every block of it, which turns a chunk from
    // thousands of operations into millions.
    // 1024 rather than the 384 the overworld needs: a datapack may raise the
    // world, and a column taller than this would silently get a depth of zero
    // in its top half, which reads as "always at the bottom of a stone run".
    static constexpr i32 kMaxColumn = 1024;
    std::array<i32, kMaxColumn> below{};
    const auto                  slots = static_cast<usize>(std::min(size, kMaxColumn));
    i32                         run   = 0;
    for (usize index = 0; index < slots; ++index) {
        const registry::BlockStateId state = column[index];
        run = (is_air(state) || is_fluid(state)) ? 0 : run + 1;
        below[index] = impl.ceiling_depth_from_run ? run : 1;
    }

    const i32 top = queries.surface_height(world_x, world_z);

    i32 stone_depth_above = 0;
    i32 water_height      = kNoWater;

    for (i32 y = std::min(top + 1, impl.min_y + size - 1); y >= impl.min_y; --y) {
        const auto                   index = static_cast<usize>(y - impl.min_y);
        const registry::BlockStateId state = column[index];

        if (is_air(state)) {
            stone_depth_above = 0;
            water_height      = kNoWater;
            continue;
        }
        if (is_fluid(state)) {
            if (water_height == kNoWater) {
                water_height = y + 1;
            }
            stone_depth_above = 0;
            continue;
        }
        // Only the stone the noise stage put there. A block a carver, an
        // aquifer or an earlier rule already decided is not the rules' to
        // reconsider — and the ordering of the top-level sequence depends on
        // it: deepslate is placed under the surface branch, and a second pass
        // over it would put grass on deepslate.
        if (state != impl.default_block) {
            ++stone_depth_above;
            continue;
        }

        ++stone_depth_above;
        at.y                 = y;
        at.stone_depth_above = stone_depth_above;
        at.stone_depth_below = index < slots ? below[index] : 0;
        at.water_height      = water_height;

        if (auto result = impl.rule->apply(at)) {
            column[index] = *result;
        }
    }
}

void SurfaceSystem::build(world::Chunk& chunk, const NoiseRouter& router, const BiomeSource&,
                          const registry::BlockRegistry& blocks) const {
    const Impl& impl = *impl_;
    if (impl.rule == nullptr) {
        return;
    }
    const auto shape    = chunk.shape();
    const auto size     = static_cast<usize>(shape.height);
    const i32  origin_x = chunk.position().x * 16;
    const i32  origin_z = chunk.position().z * 16;

    std::vector<registry::BlockStateId> column(size);
    std::vector<registry::BlockStateId> before(size);

    // Every column's top first: `steep` reads the two neighbours in z, and a
    // column that has already had its surface built would answer with a height
    // the game never saw.
    std::array<i32, 256> heights{};
    for (usize local_z = 0; local_z < 16; ++local_z) {
        for (usize local_x = 0; local_x < 16; ++local_x) {
            i32 top = shape.min_y;
            for (i32 y = shape.max_y(); y >= shape.min_y; --y) {
                const auto state = chunk.get_block(local_x, y, local_z);
                if (state != registry::kAirState && !blocks.is_air(blocks.block_of(state))) {
                    top = y;
                    break;
                }
            }
            heights[local_z * 16 + local_x] = top;
        }
    }

    const ChunkQueries queries{chunk, blocks, router, heights};

    for (usize local_z = 0; local_z < 16; ++local_z) {
        for (usize local_x = 0; local_x < 16; ++local_x) {
            for (usize index = 0; index < size; ++index) {
                column[index] =
                    chunk.get_block(local_x, shape.min_y + static_cast<i32>(index), local_z);
            }
            std::ranges::copy(column, before.begin());

            build_column(column, origin_x + static_cast<i32>(local_x),
                         origin_z + static_cast<i32>(local_z), queries, blocks);

            for (usize index = 0; index < size; ++index) {
                if (column[index] != before[index]) {
                    chunk.set_block(local_x, shape.min_y + static_cast<i32>(index), local_z,
                                    column[index]);
                }
            }
        }
    }

    chunk.recompute_heightmaps();
}

}  // namespace ov::worldgen
