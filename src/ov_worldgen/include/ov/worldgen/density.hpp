// The density function graph, as the datapack describes it.
//
// Since 1.18 the terrain is not a generator with parameters; it is an
// *expression*, written in JSON and evaluated per position. `noise_settings`
// names a router of about fifteen expressions — temperature, continents,
// erosion, final density — and each is a tree of adds, multiplies, noises and
// splines over about thirty-five named functions.
//
// The plan said this from the start, and it is the single most important
// architectural point about worldgen: implement the *interpreter*, never a
// generator that happens to look like the output. A hand-written approximation
// would be wrong at the first datapack and could never be seed-exact.
//
// What is exact here and what is not:
//
//   * Every arithmetic and noise node is evaluated exactly, in doubles, in the
//     order the game evaluates it.
//   * `flat_cache` genuinely changes the result — it quantises x and z to
//     multiples of four and evaluates at y = 0 — and is reproduced.
//   * `interpolated` also changes the result: it samples on the coarse cell
//     grid and interpolates between. Point-by-point it cannot be reproduced,
//     so this evaluator returns the direct value and says so. That is exact
//     for the climate functions, which is what the biome parity check needs,
//     and NOT exact for the final terrain density, which needs the cell grid.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/worldgen/noise.hpp"

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ov::worldgen {

enum class DensityError : u8 {
    /// A file named by the router or by another function is not there.
    Missing,
    /// A file is not JSON, or not the shape a density function has.
    Malformed,
    /// A node type this interpreter does not implement. Named loudly rather
    /// than treated as zero: a silently missing term is terrain that looks
    /// plausible and is wrong.
    Unsupported,
    /// A function that refers to itself, directly or through others.
    Cycle,
};

[[nodiscard]] std::string_view to_string(DensityError error) noexcept;

/// Where a function is being asked about.
struct FunctionContext {
    i32 x{0};
    i32 y{0};
    i32 z{0};
};

class DensityFunction {
public:
    DensityFunction()                                  = default;
    DensityFunction(const DensityFunction&)            = delete;
    DensityFunction& operator=(const DensityFunction&) = delete;
    virtual ~DensityFunction();

    [[nodiscard]] virtual f64 compute(const FunctionContext& at) const = 0;

    /// Bounds, used to prune whole subtrees without evaluating them. A bound
    /// that is too tight produces terrain with holes; one that is too loose
    /// only costs time.
    [[nodiscard]] virtual f64 min_value() const = 0;
    [[nodiscard]] virtual f64 max_value() const = 0;
};

using DensityRef = std::shared_ptr<const DensityFunction>;

/// Everything a seed decides, loaded once.
class NoiseRouter {
public:
    /// Read `worldgen/noise_settings/<settings>.json` and everything it names,
    /// and seed every noise from `seed`.
    [[nodiscard]] static std::expected<NoiseRouter, DensityError> load(
        const std::filesystem::path& data_root, std::string_view settings, i64 seed);

    NoiseRouter(NoiseRouter&&) noexcept;
    NoiseRouter& operator=(NoiseRouter&&) noexcept;
    ~NoiseRouter();

    /// One of the router's entries: "temperature", "vegetation", "continents",
    /// "erosion", "depth", "ridges", "final_density", and the rest.
    [[nodiscard]] const DensityFunction* entry(std::string_view name) const;

    /// A named function from the datapack — "minecraft:overworld/offset" and
    /// the like. The router's entries are built out of these, so when a router
    /// entry is wrong this is how the wrongness is bisected.
    [[nodiscard]] const DensityFunction* function(std::string_view name) const;

    /// Router entries that could not be built, and why. Empty when the whole
    /// graph loaded.
    [[nodiscard]] std::vector<std::pair<std::string, DensityError>> unavailable() const;

    [[nodiscard]] i32 sea_level() const noexcept;
    [[nodiscard]] i32 min_y() const noexcept;
    [[nodiscard]] i32 height() const noexcept;
    /// Cell size in blocks: horizontal and vertical. The terrain is sampled on
    /// this grid and interpolated between, which is why `interpolated` cannot
    /// be evaluated point by point.
    [[nodiscard]] i32 cell_width() const noexcept;
    [[nodiscard]] i32 cell_height() const noexcept;

private:
    struct Impl;

    NoiseRouter();

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::worldgen
