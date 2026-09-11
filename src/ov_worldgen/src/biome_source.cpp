#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/biome_source.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/end.hpp"  // ── end ──

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>

namespace ov::worldgen {

namespace {

/// A parameter is either a single number or a two-element range. Both appear
/// in the same file, often in the same entry.
[[nodiscard]] bool read_range(simdjson::simdjson_result<simdjson::dom::element> field, i64& low,
                              i64& high) {
    if (field.error() != simdjson::SUCCESS) {
        return false;
    }
    f64 single = 0.0;
    if (field.get(single) == simdjson::SUCCESS) {
        low  = BiomeSource::quantise(static_cast<f32>(single));
        high = low;
        return true;
    }
    simdjson::dom::array pair;
    if (field.get(pair) != simdjson::SUCCESS) {
        return false;
    }
    std::array<f64, 2> ends{};
    usize              index = 0;
    for (auto value : pair) {
        if (index >= ends.size() || value.get(ends[index]) != simdjson::SUCCESS) {
            return false;
        }
        ++index;
    }
    if (index != 2) {
        return false;
    }
    low  = BiomeSource::quantise(static_cast<f32>(ends[0]));
    high = BiomeSource::quantise(static_cast<f32>(ends[1]));
    return true;
}

}  // namespace

std::expected<BiomeSource, DensityError> BiomeSource::load(const std::filesystem::path& data_root,
                                                           std::string_view dimension) {
    if (dimension == "end") {  // ── end ── a rule, not a table
        return the_end();
    }
    const auto path = data_root / "reports" / "biome_parameters" / "minecraft" /
                      (std::string(dimension) + ".json");
    if (!std::filesystem::is_regular_file(path)) {
        OV_LOG_ERROR(
            "worldgen: {} is missing. The biome table is not in the datapack — it is exported "
            "by the official data generator into reports/biome_parameters.",
            path.string());
        return std::unexpected(DensityError::Missing);
    }

    auto text = simdjson::padded_string::load(path.string());
    if (text.error() != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }
    simdjson::dom::parser parser;
    auto                  document = parser.parse(text.value());
    if (document.error() != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }

    simdjson::dom::array raw;
    if (document.at_key("biomes").get(raw) != simdjson::SUCCESS) {
        return std::unexpected(DensityError::Malformed);
    }

    // The order of the seven axes is fixed and shared with the sampler. Getting
    // two of them the wrong way round produces a world made of plausible
    // biomes in impossible places.
    constexpr std::array<const char*, 7> kAxes{"temperature", "humidity",  "continentalness",
                                               "erosion",     "depth",     "weirdness",
                                               "offset"};

    BiomeSource source;
    for (auto element : raw) {
        Entry            entry;
        std::string_view name;
        if (element.at_key("biome").get(name) != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        entry.biome = std::string(name);

        auto parameters = element.at_key("parameters");
        if (parameters.error() != simdjson::SUCCESS) {
            return std::unexpected(DensityError::Malformed);
        }
        for (usize axis = 0; axis < kAxes.size(); ++axis) {
            if (!read_range(parameters.at_key(kAxes[axis]), entry.box[axis].low,
                            entry.box[axis].high)) {
                return std::unexpected(DensityError::Malformed);
            }
        }
        source.entries_.push_back(std::move(entry));
    }

    source.build_tree();
    source.scan_only_ = std::getenv("OV_BIOME_SCAN") != nullptr;
    if (source.scan_only_) {
        OV_LOG_WARN("worldgen: OV_BIOME_SCAN is set, the climate tree is bypassed");
    }

    OV_LOG_INFO("worldgen: {} biome boxes, {} distinct biomes, {} tree nodes",
                source.entries_.size(), source.biome_count(), source.nodes_.size());
    return source;
}

// ── end ──
BiomeSource BiomeSource::the_end() {
    BiomeSource source;
    for (const std::string_view name : kEndBiomes) {
        Entry entry;
        entry.biome = std::string(name);
        source.entries_.push_back(std::move(entry));
    }
    source.end_rule_ = true;
    OV_LOG_INFO("worldgen: the End's biome source, a fixed rule over {} biomes", kEndBiomes.size());
    return source;
}

std::string_view BiomeSource::entry_biome(usize index) const {
    return index < entries_.size() ? std::string_view{entries_[index].biome} : std::string_view{};
}

ClimatePoint BiomeSource::entry_centre(usize index) const {
    ClimatePoint point;
    if (index >= entries_.size()) {
        return point;
    }
    for (usize axis = 0; axis < 7; ++axis) {
        const auto& range      = entries_[index].box[axis];
        point.coordinates[axis] = range.low + (range.high - range.low) / 2;
    }
    // The seventh axis is measured against zero, never sampled, so a box with
    // a non-zero offset must still be probed at zero or the probe would be
    // asking about a point the world cannot reach.
    point.coordinates[6] = 0;
    return point;
}

std::vector<std::string_view> BiomeSource::biomes() const {
    std::set<std::string_view> names;
    for (const Entry& entry : entries_) {
        names.insert(entry.biome);
    }
    return {names.begin(), names.end()};
}

usize BiomeSource::biome_count() const noexcept {
    std::set<std::string_view> names;
    for (const Entry& entry : entries_) {
        names.insert(entry.biome);
    }
    return names.size();
}

ClimatePoint BiomeSource::sample(const NoiseRouter& router, i32 quart_x, i32 quart_y,
                                 i32 quart_z) const {
    // Biomes live on a 4x4x4 grid, and the climate is read at the *block* that
    // grid cell starts on. Sampling at the quart coordinate itself would
    // compress the whole world into a sixty-fourth of itself.
    const FunctionContext at{quart_x * 4, quart_y * 4, quart_z * 4};

    if (end_rule_) {  // ── end ── the rule's answer, as an index
        ClimatePoint point;
        point.coordinates[0] = static_cast<i64>(end_biome_at(router, quart_x, quart_z));
        return point;
    }

    const auto read = [&](const char* name) {
        const DensityFunction* function = router.entry(name);
        return function == nullptr ? 0.0F : static_cast<f32>(function->compute(at));
    };

    ClimatePoint point;
    point.coordinates[0] = quantise(read("temperature"));
    // The router calls it vegetation; the biome table calls the same axis
    // humidity.
    point.coordinates[1] = quantise(read("vegetation"));
    point.coordinates[2] = quantise(read("continents"));
    point.coordinates[3] = quantise(read("erosion"));
    point.coordinates[4] = quantise(read("depth"));
    // And weirdness is the router's ridges.
    point.coordinates[5] = quantise(read("ridges"));
    // The seventh axis is not sampled: it is measured against zero, so a box
    // with a non-zero offset is penalised by exactly that offset.
    point.coordinates[6] = 0;
    return point;
}

std::array<i64, 7> BiomeSource::gap_to(const ClimatePoint& climate,
                                       std::string_view    biome) const {
    i64                best = std::numeric_limits<i64>::max();
    std::array<i64, 7> gaps{};
    for (const Entry& entry : entries_) {
        if (entry.biome != biome) {
            continue;
        }
        std::array<i64, 7> here{};
        i64                total = 0;
        for (usize axis = 0; axis < entry.box.size(); ++axis) {
            here[axis] = entry.box[axis].distance(climate.coordinates[axis]);
            total += here[axis] * here[axis];
        }
        if (total < best) {
            best = total;
            gaps = here;
        }
    }
    return gaps;
}

i64 BiomeSource::distance_to(const ClimatePoint& climate, std::string_view biome) const {
    i64 best = std::numeric_limits<i64>::max();
    for (const Entry& entry : entries_) {
        if (entry.biome != biome) {
            continue;
        }
        i64 total = 0;
        for (usize axis = 0; axis < entry.box.size(); ++axis) {
            const i64 gap = entry.box[axis].distance(climate.coordinates[axis]);
            total += gap * gap;
        }
        best = std::min(best, total);
    }
    return best;
}

namespace {

/// Distance from a box to a point: zero on every axis the point is inside, the
/// sum of the squared gaps otherwise.
///
/// The whole search is integers. Twenty thousand is the widest a quantised
/// climate axis gets, so one squared gap is at most 1.6e9 and a sum of seven
/// of them at most 1.2e10 — well inside an i64, and never a double, so two
/// builds cannot round a boundary differently.
[[nodiscard]] i64 squared_distance(const std::array<i64, 7>& gaps) noexcept {
    i64 total = 0;
    for (const i64 gap : gaps) {
        total += gap * gap;
    }
    return total;
}

}  // namespace

i32 BiomeSource::scan(const ClimatePoint& climate) const {
    i64 best      = std::numeric_limits<i64>::max();
    i32 nearest   = -1;
    for (usize index = 0; index < entries_.size(); ++index) {
        std::array<i64, 7> gaps{};
        for (usize axis = 0; axis < 7; ++axis) {
            gaps[axis] = entries_[index].box[axis].distance(climate.coordinates[axis]);
        }
        const i64 total = squared_distance(gaps);
        // Strictly less, so the first of several equally distant boxes wins —
        // and the order here is the file's, which is exactly where this
        // disagrees with the tree.
        if (total < best) {
            best    = total;
            nearest = static_cast<i32>(index);
        }
    }
    return nearest;
}

i32 BiomeSource::search(const ClimatePoint& climate, i32 start) const {
    if (scan_only_) {
        return scan(climate);
    }
    if (root_ < 0) {
        return -1;
    }

    const auto distance_to_box = [&](const std::array<Range, 7>& box) {
        std::array<i64, 7> gaps{};
        for (usize axis = 0; axis < 7; ++axis) {
            gaps[axis] = box[axis].distance(climate.coordinates[axis]);
        }
        return squared_distance(gaps);
    };

    // The cached answer seeds the running best, as it does in the game. Being
    // the running best it wins any tie it takes part in, and it also prunes: a
    // subtree no nearer than the cached box is never opened, so a box inside
    // it that would have tied is never even reached.
    i32 best      = start;
    i64 best_dist = std::numeric_limits<i64>::max();
    if (best >= 0 && best < static_cast<i32>(entries_.size())) {
        best_dist = distance_to_box(entries_[static_cast<usize>(best)].box);
    } else {
        best = -1;
    }

    // An explicit stack rather than recursion. The tree is six levels deep at
    // 7593 boxes, but nothing in the data promises that, and worldgen has no
    // budget for a stack overflow on someone's datapack.
    struct Frame {
        u32 node{0};
        u32 next{0};
    };
    std::array<Frame, 40> stack{};
    usize                 depth = 0;
    stack[depth++]              = Frame{static_cast<u32>(root_), 0};

    while (depth != 0) {
        Frame&      frame = stack[depth - 1];
        const Node& node  = nodes_[frame.node];
        if (frame.next >= node.child_count) {
            --depth;
            continue;
        }
        const u32 child_index = node.first_child + frame.next;
        ++frame.next;

        const Node& child    = nodes_[child_index];
        const i64   distance = distance_to_box(child.box);
        // Strictly nearer, never merely as near. This single comparison is
        // what makes a tie depend on the shape of the tree rather than on the
        // climate, and it is the whole of what a plain scan got wrong.
        if (best_dist <= distance) {
            continue;
        }
        if (child.entry >= 0) {
            best_dist = distance;
            best      = child.entry;
            continue;
        }
        if (depth < stack.size()) {
            stack[depth++] = Frame{child_index, 0};
        } else {
            OV_LOG_ERROR("worldgen: the climate tree is deeper than {} levels", stack.size());
        }
    }
    return best;
}

i32 BiomeSource::entry_at(const ClimatePoint& climate, BiomeSearchCache& cache) const {
    if (end_rule_) {  // ── end ──
        const i64 index = std::clamp<i64>(climate.coordinates[0], 0,
                                          static_cast<i64>(entries_.size()) - 1);
        cache.last = static_cast<i32>(index);
        return static_cast<i32>(index);
    }
    const i32 found = search(climate, cache.last);
    cache.last      = found;
    return found;
}

std::string_view BiomeSource::biome_at(const ClimatePoint& climate) const {
    if (end_rule_) {  // ── end ──
        BiomeSearchCache unused;
        return entries_[static_cast<usize>(entry_at(climate, unused))].biome;
    }
    const i32 found = search(climate, -1);
    return found < 0 ? std::string_view{}
                     : std::string_view{entries_[static_cast<usize>(found)].biome};
}

std::string_view BiomeSource::biome_at(const ClimatePoint& climate,
                                       BiomeSearchCache&   cache) const {
    const i32 found = entry_at(climate, cache);
    return found < 0 ? std::string_view{}
                     : std::string_view{entries_[static_cast<usize>(found)].biome};
}

// ── Building the tree ───────────────────────────────────────────────────────
//
// Bottom-up, and every step of the shape matters to the answer rather than
// only to the speed, because the search's traversal order decides ties:
//
//   * leaves start in the order of the exported table;
//   * a group of six or fewer is ordered by the sum of the absolute midpoints
//     of its seven axes;
//   * a larger group is split by trying all seven axes in turn — order by that
//     axis, ties broken by the axes after it wrapping round, cut into buckets
//     of the largest power of six below the count, and keep whichever axis
//     gives the smallest total bounding box;
//   * the buckets are then reordered by absolute midpoint on the winning axis,
//     and each is built the same way.
//
// The bucket size is computed from `count - 0.01` rather than from `count`, so
// that an exact power of six splits into six full buckets instead of one
// bucket holding everything.

void BiomeSource::build_tree() {
    nodes_.clear();
    root_ = -1;
    if (entries_.empty()) {
        return;
    }

    // Built as a tree of index vectors first and flattened afterwards. The
    // extra pass costs one traversal at load time and buys a query that walks
    // a single array and allocates nothing.
    struct Builder {
        struct Item {
            std::array<Range, 7> box{};
            i32                  entry{-1};
            std::vector<usize>   children;
        };

        std::vector<Item> pool;

        [[nodiscard]] i64 midpoint(usize node, usize axis) const noexcept {
            // Truncating division, which is what an integer divide is in both
            // languages. Rounding it the other way would reorder boxes whose
            // midpoints straddle zero.
            return (pool[node].box[axis].low + pool[node].box[axis].high) / 2;
        }

        [[nodiscard]] i64 absolute_key(usize node) const noexcept {
            i64 total = 0;
            for (usize axis = 0; axis < 7; ++axis) {
                total += std::abs(midpoint(node, axis));
            }
            return total;
        }

        [[nodiscard]] std::array<Range, 7> bounding(const std::vector<usize>& members) const {
            std::array<Range, 7> box{};
            for (usize axis = 0; axis < 7; ++axis) {
                box[axis].low  = std::numeric_limits<i64>::max();
                box[axis].high = std::numeric_limits<i64>::min();
            }
            for (const usize member : members) {
                for (usize axis = 0; axis < 7; ++axis) {
                    box[axis].low  = std::min(box[axis].low, pool[member].box[axis].low);
                    box[axis].high = std::max(box[axis].high, pool[member].box[axis].high);
                }
            }
            return box;
        }

        /// Order by one axis, ties broken by the axes after it, wrapping round.
        /// Stable, because where all seven keys agree the source order is the
        /// answer.
        void sort_by_axis(std::vector<usize>& members, usize first_axis, bool absolute) const {
            std::stable_sort(members.begin(), members.end(), [&](usize a, usize b) {
                for (usize step = 0; step < 7; ++step) {
                    const usize axis = (first_axis + step) % 7;
                    const i64   ka   = midpoint(a, axis);
                    const i64   kb   = midpoint(b, axis);
                    const i64   va   = absolute ? std::abs(ka) : ka;
                    const i64   vb   = absolute ? std::abs(kb) : kb;
                    if (va != vb) {
                        return va < vb;
                    }
                }
                return false;
            });
        }

        /// Cut an ordered list into buckets of the largest power of six below
        /// its size.
        [[nodiscard]] static std::vector<std::vector<usize>> bucketize(
            const std::vector<usize>& members) {
            const f64   span = static_cast<f64>(members.size()) - 0.01;
            const usize per  = std::max<usize>(
                1, static_cast<usize>(std::pow(6.0, std::floor(std::log(span) / std::log(6.0)))));
            std::vector<std::vector<usize>> buckets;
            std::vector<usize>              current;
            for (const usize member : members) {
                current.push_back(member);
                if (current.size() >= per) {
                    buckets.push_back(std::move(current));
                    current.clear();
                }
            }
            if (!current.empty()) {
                buckets.push_back(std::move(current));
            }
            return buckets;
        }

        /// The cost of a bucket: the total width of its bounding box. The axis
        /// that makes the boxes tightest is the one that keeps the search from
        /// opening subtrees it does not need.
        [[nodiscard]] static i64 cost(const std::array<Range, 7>& box) noexcept {
            i64 total = 0;
            for (usize axis = 0; axis < 7; ++axis) {
                total += std::abs(box[axis].high - box[axis].low);
            }
            return total;
        }

        [[nodiscard]] usize add_branch(std::vector<usize> members) {
            Item branch;
            branch.box      = bounding(members);
            branch.children = std::move(members);
            pool.push_back(std::move(branch));
            return pool.size() - 1;
        }

        usize build(std::vector<usize> members) {
            if (members.size() == 1) {
                return members.front();
            }
            if (members.size() <= 6) {
                std::stable_sort(members.begin(), members.end(), [&](usize a, usize b) {
                    return absolute_key(a) < absolute_key(b);
                });
                return add_branch(std::move(members));
            }

            i64                             best_cost = std::numeric_limits<i64>::max();
            usize                           best_axis = 0;
            std::vector<std::vector<usize>> best_buckets;
            for (usize axis = 0; axis < 7; ++axis) {
                std::vector<usize> ordered = members;
                sort_by_axis(ordered, axis, false);
                auto buckets = bucketize(ordered);
                i64  total   = 0;
                for (const auto& bucket : buckets) {
                    total += cost(bounding(bucket));
                }
                // Strictly better, so the earliest axis wins a tie — the same
                // rule the search itself follows.
                if (total < best_cost) {
                    best_cost    = total;
                    best_axis    = axis;
                    best_buckets = std::move(buckets);
                }
            }

            // Each bucket becomes a node so that it can be ordered by its own
            // bounding box, and only then is it built out.
            std::vector<usize> groups;
            groups.reserve(best_buckets.size());
            for (auto& bucket : best_buckets) {
                groups.push_back(add_branch(std::move(bucket)));
            }
            sort_by_axis(groups, best_axis, true);

            std::vector<usize> children;
            children.reserve(groups.size());
            for (const usize group : groups) {
                children.push_back(build(pool[group].children));
            }
            return add_branch(std::move(children));
        }
    };

    Builder builder;
    builder.pool.reserve(entries_.size() * 2);
    std::vector<usize> leaves;
    leaves.reserve(entries_.size());
    for (usize index = 0; index < entries_.size(); ++index) {
        Builder::Item leaf;
        leaf.box   = entries_[index].box;
        leaf.entry = static_cast<i32>(index);
        builder.pool.push_back(std::move(leaf));
        leaves.push_back(builder.pool.size() - 1);
    }
    const usize root = builder.build(std::move(leaves));

    // Flatten, so that a node's children are one contiguous run and the search
    // visits them in the order they were built in. A worklist rather than
    // recursion, for the same reason the search uses one.
    nodes_.clear();
    const auto copy_of = [&](usize item) {
        return Node{builder.pool[item].box, builder.pool[item].entry, 0, 0};
    };
    nodes_.push_back(copy_of(root));
    std::vector<std::pair<usize, u32>> pending{{root, 0}};
    while (!pending.empty()) {
        const auto [item, slot] = pending.back();
        pending.pop_back();
        const usize count = builder.pool[item].children.size();
        if (count == 0) {
            continue;
        }
        const auto first          = static_cast<u32>(nodes_.size());
        nodes_[slot].first_child  = first;
        nodes_[slot].child_count  = static_cast<u32>(count);
        for (usize index = 0; index < count; ++index) {
            nodes_.push_back(copy_of(builder.pool[item].children[index]));
        }
        for (usize index = 0; index < count; ++index) {
            pending.emplace_back(builder.pool[item].children[index],
                                 first + static_cast<u32>(index));
        }
    }
    root_ = 0;
}

}  // namespace ov::worldgen

