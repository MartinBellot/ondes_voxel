// The Great Pyramids of one worldgen stack. An original Ondes VOXEL structure
// — see great_pyramid.hpp.
#define OV_LOG_CATEGORY "worldgen"

#include "great_pyramid_impl.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/structure_nbt.hpp"

#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <utility>

namespace ov::worldgen {

namespace {

[[nodiscard]] constexpr i64 key_of(i32 chunk_x, i32 chunk_z) noexcept {
    return (static_cast<i64>(chunk_x) << 32) | static_cast<i64>(static_cast<u32>(chunk_z));
}

[[nodiscard]] constexpr i32 floor_div(i32 value, i32 divisor) noexcept {
    const i32 q = value / divisor;
    return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? q - 1 : q;
}

/// The compound named `name` inside `parent`, created when missing.
nbt::Tag& child_compound(nbt::Tag& parent, std::string_view name) {
    if (nbt::Tag* found = parent.find(name); found != nullptr) {
        return *found;
    }
    parent.compound()->push_back(nbt::CompoundEntry{std::string{name}, nbt::Tag::make_compound()});
    return parent.compound()->back().value;
}

}  // namespace

struct GreatPyramidStage::Impl {
    std::optional<GreatPyramid>  pyramid;
    std::string                  error;
    bool                         reported{false};
    const StructurePlacer*       placer{nullptr};
    const StructureWorldSampler* sampler{nullptr};
    i64                          seed{0};
    RandomSpreadPlacement        grid{great_pyramid_placement()};
    /// Per candidate chunk: the layout, or null when it does not start one.
    std::unordered_map<i64, std::unique_ptr<GreatPyramidLayout>> starts;
    Stats                                                        stats;

    const GreatPyramidLayout* start_at(i32 chunk_x, i32 chunk_z) {
        if (!pyramid || !grid.is_candidate_chunk(seed, chunk_x, chunk_z)) {
            return nullptr;
        }
        const i64 key = key_of(chunk_x, chunk_z);
        if (const auto it = starts.find(key); it != starts.end()) {
            return it->second.get();
        }
        auto            layout   = std::make_unique<GreatPyramidLayout>();
        const PyramidDecision decision =
            pyramid->decide(seed, chunk_x, chunk_z, sampler, placer, layout.get());
        ++stats.decided;
        ++stats.decisions[decision];
        if (decision != PyramidDecision::Placed) {
            layout.reset();
        }
        return starts.emplace(key, std::move(layout)).first->second.get();
    }

    /// Every start whose blocks can reach chunk (chunk_x, chunk_z): the
    /// candidates of the grid cells within `kReach`.
    template <typename F>
    void around(i32 chunk_x, i32 chunk_z, F&& visit) {
        constexpr i32 kReach = GreatPyramid::kReach;
        const i32     gx0    = floor_div(chunk_x - kReach, grid.spacing);
        const i32     gx1    = floor_div(chunk_x + kReach, grid.spacing);
        const i32     gz0    = floor_div(chunk_z - kReach, grid.spacing);
        const i32     gz1    = floor_div(chunk_z + kReach, grid.spacing);
        for (i32 gz = gz0; gz <= gz1; ++gz) {
            for (i32 gx = gx0; gx <= gx1; ++gx) {
                const ChunkPos candidate = grid.candidate(seed, gx, gz);
                if (std::abs(candidate.x - chunk_x) > kReach ||
                    std::abs(candidate.z - chunk_z) > kReach) {
                    continue;
                }
                if (const GreatPyramidLayout* layout = start_at(candidate.x, candidate.z)) {
                    visit(*layout);
                }
            }
        }
    }
};

GreatPyramidStage::GreatPyramidStage(const registry::BlockRegistry& blocks,
                                     const StructurePlacer*         placer,
                                     const StructureWorldSampler* sampler, i64 level_seed)
    : impl_(std::make_unique<Impl>()) {
    impl_->placer  = placer;
    impl_->sampler = sampler;
    impl_->seed    = level_seed;
    if (auto created = GreatPyramid::create(blocks)) {
        impl_->pyramid.emplace(std::move(*created));
    } else {
        impl_->error = created.error();
    }
}

GreatPyramidStage::~GreatPyramidStage() = default;

bool GreatPyramidStage::ready() const noexcept { return impl_->pyramid.has_value(); }

void GreatPyramidStage::place(StructureLevel& level, i32 chunk_x, i32 chunk_z) {
    if (!impl_->pyramid) {
        if (!impl_->reported) {
            impl_->reported = true;
            OV_LOG_ERROR("{} not placed: {}", kGreatPyramidId, impl_->error);
        }
        return;
    }
    const BoundingBox column = BoundingBox::chunk_column(chunk_x, chunk_z, -2048, 2047);
    impl_->around(chunk_x, chunk_z, [&](const GreatPyramidLayout& layout) {
        if (!layout.box.intersects(column)) {
            return;
        }
        impl_->stats.blocks_written += impl_->pyramid->place(level, layout, column);
        ++impl_->stats.placements;
    });
}

const GreatPyramidLayout* GreatPyramidStage::start_at(i32 chunk_x, i32 chunk_z) {
    return impl_->start_at(chunk_x, chunk_z);
}

bool GreatPyramidStage::record(i32 chunk_x, i32 chunk_z, nbt::Tag& structures) {
    if (!impl_->pyramid || structures.compound() == nullptr) {
        return false;
    }
    bool added = false;
    if (const GreatPyramidLayout* here = impl_->start_at(chunk_x, chunk_z)) {
        child_compound(structures, "starts")
            .compound()
            ->push_back(nbt::CompoundEntry{std::string{kGreatPyramidId},
                                           GreatPyramid::start_to_nbt(*here)});
        added = true;
    }
    const BoundingBox   column = BoundingBox::chunk_column(chunk_x, chunk_z, -2048, 2047);
    nbt::Tag::LongArray positions;
    impl_->around(chunk_x, chunk_z, [&](const GreatPyramidLayout& layout) {
        if (layout.box.intersects(column)) {
            positions.push_back(packed_chunk_pos(layout.chunk_x, layout.chunk_z));
        }
    });
    if (!positions.empty()) {
        child_compound(structures, "References")
            .compound()
            ->push_back(nbt::CompoundEntry{std::string{kGreatPyramidId}, nbt::Tag{positions}});
        added = true;
    }
    return added;
}

void GreatPyramidStage::trim(i32 centre_x, i32 centre_z, i32 keep) {
    std::erase_if(impl_->starts, [&](const auto& entry) {
        const auto x = static_cast<i32>(entry.first >> 32);
        const auto z = static_cast<i32>(static_cast<u32>(entry.first));
        return std::abs(x - centre_x) > keep || std::abs(z - centre_z) > keep;
    });
}

void GreatPyramidStage::clear() { impl_->starts.clear(); }

const GreatPyramidStage::Stats& GreatPyramidStage::stats() const noexcept { return impl_->stats; }

const GreatPyramid* GreatPyramidStage::pyramid() const noexcept {
    return impl_->pyramid ? &*impl_->pyramid : nullptr;
}

}  // namespace ov::worldgen
