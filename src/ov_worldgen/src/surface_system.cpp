#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/surface_system.hpp"

#include "surface_extension.hpp"  // ── worldgen-3 ──

#include "ov/worldgen/biome_source.hpp"  // ── worldgen-3 ── the zoom's neighbours
#include "ov/worldgen/biome_zoom.hpp"

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

    /// ── worldgen-3 ── The pillar and iceberg passes; inert when a noise or
    /// the Xoroshiro factory is missing.
    SurfaceExtension extension;

    /// ── worldgen-3 ── The biome zoom's seed, and whether the rules ask
    /// through it (`OV_BIOME_ZOOM=0`, the instrument, says no).
    i64  zoom_seed{0};
    bool fuzzy_biomes{true};

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

/// ── worldgen-3 ── The rules' biome questions, asked the way the game asks
/// them: through `BiomeManager`'s zoom (biome_zoom.hpp). The inner queries must
/// answer for a cell one step outside the chunk.
class ZoomedQueries final : public SurfaceQueries {
public:
    ZoomedQueries(const SurfaceQueries& inner, i64 seed, i32 min_y, i32 height)
        : inner_(&inner), seed_(seed), min_quart_(min_y >> 2), max_quart_(((min_y + height) >> 2) - 1) {}

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        const BiomeCell cell = zoomed(x, y, z);
        return inner_->biome_at(cell.x * 4, cell.y * 4, cell.z * 4);
    }
    [[nodiscard]] f64 temperature_at(i32 x, i32 y, i32 z) const override {
        const BiomeCell cell = zoomed(x, y, z);
        return inner_->temperature_at(cell.x * 4, cell.y * 4, cell.z * 4);
    }
    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override {
        return inner_->surface_height(x, z);
    }
    [[nodiscard]] i32 preliminary_surface(i32 x, i32 z) const override {
        return inner_->preliminary_surface(x, z);
    }

private:
    [[nodiscard]] BiomeCell zoomed(i32 x, i32 y, i32 z) const noexcept {
        BiomeCell cell = fuzzy_biome_cell(seed_, x, y, z);
        cell.y         = std::clamp(cell.y, min_quart_, max_quart_);
        return cell;
    }

    const SurfaceQueries* inner_;
    i64                   seed_;
    i32                   min_quart_;
    i32                   max_quart_;
};

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
                 const NoiseRouter& router, const std::array<i32, 256>& heights,
                 const BiomeSource* source = nullptr)
        : chunk_(&chunk),
          blocks_(&blocks),
          router_(&router),
          source_(source),
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
        const i32 local_x = x - origin_x_;
        const i32 local_z = z - origin_z_;
        if (local_x >= 0 && local_x < 16 && local_z >= 0 && local_z < 16) {
            return blocks_->biome_name(chunk_->get_biome(static_cast<usize>(local_x), y,
                                                         static_cast<usize>(local_z)));
        }
        // ── worldgen-3 ── A cell of a neighbour, which the zoom reaches: the
        // biome source answers it exactly. The End's rule is per chunk and has
        // no cell of its own to ask; the chunk's edge answers there.
        if (source_ != nullptr && !source_->is_end_rule()) {
            const i32 quart_y = std::clamp(y, min_y_, min_y_ + height_ - 1) >> 2;
            return source_->biome_at(source_->sample(*router_, x >> 2, quart_y, z >> 2));
        }
        return blocks_->biome_name(chunk_->get_biome(static_cast<usize>(std::clamp(local_x, 0, 15)), y,
                                                     static_cast<usize>(std::clamp(local_z, 0, 15))));
    }

    [[nodiscard]] f64 temperature_at(i32 x, i32 y, i32 z) const override {
        const auto index = blocks_->find_biome(biome_at(x, y, z));
        return index ? blocks_->biome(*index).temperature : 0.8;
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
    const NoiseRouter*             router_;
    const BiomeSource*             source_;
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
    impl.zoom_seed = obfuscate_biome_seed(seed);  // ── worldgen-3 ──
    if (const char* setting = std::getenv("OV_BIOME_ZOOM"); setting != nullptr) {
        impl.fuzzy_biomes = std::string_view(setting) != "0";
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

    // ── worldgen-3 ── The pillar and iceberg passes' six noises. A dimension
    // whose datapack lacks them simply has no such passes.
    {
        SurfaceExtension& ext = impl.extension;
        const std::array<std::pair<std::string_view, const NormalNoise**>, 6> pass_noises{
            std::pair{std::string_view("minecraft:badlands_surface"), &ext.badlands_surface},
            std::pair{std::string_view("minecraft:badlands_pillar"), &ext.badlands_pillar},
            std::pair{std::string_view("minecraft:badlands_pillar_roof"), &ext.badlands_pillar_roof},
            std::pair{std::string_view("minecraft:iceberg_surface"), &ext.iceberg_surface},
            std::pair{std::string_view("minecraft:iceberg_pillar"), &ext.iceberg_pillar},
            std::pair{std::string_view("minecraft:iceberg_pillar_roof"), &ext.iceberg_pillar_roof}};
        for (const auto& [name, slot] : pass_noises) {
            auto built = impl.load_noise(name);
            *slot      = built ? built->get() : nullptr;
        }
        ext.blocks        = &blocks;
        ext.random        = impl.factory.xoroshiro();
        ext.default_block = impl.default_block;
        ext.min_y         = impl.min_y;
        ext.sea_level     = impl.sea_level;
        const auto packed = blocks.find_block("minecraft:packed_ice");
        const auto snow   = blocks.find_block("minecraft:snow_block");
        const auto water  = blocks.find_block("minecraft:water");
        if (packed && snow && water) {
            ext.packed_ice = blocks.default_state(*packed);
            ext.snow_block = blocks.default_state(*snow);
            ext.water      = *water;
        } else {
            ext.blocks = nullptr;
        }
        // An instrument, like OV_AQUIFER: the "before" of the measurement from
        // the same binary.
        if (const char* setting = std::getenv("OV_SURFACE_PASSES");
            setting != nullptr && std::string_view(setting) == "0") {
            ext.blocks = nullptr;
        }
        if (const char* setting = std::getenv("OV_SURFACE_PASS_SHIFT"); setting != nullptr) {
            ext.shift = std::atoi(setting);
        }
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
                                 i32 world_z, const SurfaceQueries& raw_queries,
                                 const registry::BlockRegistry& blocks) const {
    const Impl& impl = *impl_;
    if (impl.rule == nullptr || column.empty()) {
        return;
    }
    // ── worldgen-3 ── Every biome question below — the rules', the two passes'
    // — goes through the game's zoom unless the instrument says otherwise.
    const ZoomedQueries   zoomed{raw_queries, impl.zoom_seed, impl.min_y, impl.height};
    const SurfaceQueries& queries =
        impl.fuzzy_biomes ? static_cast<const SurfaceQueries&>(zoomed) : raw_queries;

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

    // ── worldgen-3 ── The biome is read once, one above the column's top, and
    // decides both passes: the pillar before the rules, the iceberg after.
    i32 top = queries.surface_height(world_x, world_z);
    const i32 above_top = top + 1;
    const auto is_biome = [](std::string_view name, std::string_view bare) {
        return name == bare || (name.starts_with("minecraft:") && name.substr(10) == bare);
    };
    std::string_view column_biome;
    if (impl.extension.ready()) {
        column_biome = queries.biome_at(world_x, above_top, world_z);
        if (is_biome(column_biome, "eroded_badlands")) {
            impl.extension.eroded_badlands(column, world_x, world_z, above_top);
            for (i32 y = impl.min_y + size - 1; y >= impl.min_y; --y) {
                if (!is_air(column[static_cast<usize>(y - impl.min_y)])) {
                    top = std::max(top, y);
                    break;
                }
            }
        }
    }

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

    // ── worldgen-3 ── The iceberg pass, after the rules. It stops at the
    // context's minimum surface level: the preliminary surface interpolated
    // between the four corners of the chunk grid cell, lowered by eight and
    // raised by the column's surface depth.
    if (is_biome(column_biome, "frozen_ocean") || is_biome(column_biome, "deep_frozen_ocean")) {
        const i32 cell_x  = (world_x >> 4) * 16;
        const i32 cell_z  = (world_z >> 4) * 16;
        const i32 c00     = queries.preliminary_surface(cell_x, cell_z);
        const i32 c10     = queries.preliminary_surface(cell_x + 16, cell_z);
        const i32 c01     = queries.preliminary_surface(cell_x, cell_z + 16);
        const i32 c11     = queries.preliminary_surface(cell_x + 16, cell_z + 16);
        i32       minimum = kNoSurface;
        if (c00 != kNoSurface && c10 != kNoSurface && c01 != kNoSurface && c11 != kNoSurface) {
            const auto fx   = static_cast<f64>(static_cast<f32>(world_x & 15) / 16.0F);
            const auto fz   = static_cast<f64>(static_cast<f32>(world_z & 15) / 16.0F);
            const auto lerp = [](f64 t, f64 a, f64 b) { return a + t * (b - a); };
            const f64  both = lerp(fz, lerp(fx, c00, c10), lerp(fx, c01, c11));
            minimum = static_cast<i32>(std::floor(both)) + at.surface_depth - 8;
        }
        impl.extension.frozen_ocean(column, world_x, world_z, above_top, minimum,
                                    [&] {
                                        // The column biome's own temperature: the
                                        // game asks that biome, not a new lookup.
                                        const auto index = blocks.find_biome(column_biome);
                                        return index ? static_cast<f32>(blocks.biome(*index).temperature)
                                                     : 0.5F;
                                    }());
    }
}

void SurfaceSystem::build(world::Chunk& chunk, const NoiseRouter& router,
                          const BiomeSource& biome_source, const registry::BlockRegistry& blocks) const {
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

    const ChunkQueries queries{chunk, blocks, router, heights, &biome_source};

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
