// ov_modelbake — run the whole model pipeline over a real resource pack.
//
// The unit tests pin the rules on fixtures small enough to read. This runs the
// same code over all 1005 blockstate files and 2016 models of a 1.20.1 install
// and reports what came out, which is the only way to find the shapes nobody
// thought to write a fixture for.
//
// It also re-derives the one rule the format documents only as "it
// automatically generates based on the element's position": for every face that
// writes its uv by hand, it compares that uv with the one we would have
// generated. A high agreement rate is the evidence behind the per-face
// formulas recorded in docs/PROVENANCE.md, and a drop in it is the alarm if
// somebody changes them.
//
// Assets are never committed: this reads run/assets, which ov_assetimport
// produces and .gitignore excludes.

#include "ov/base/types.hpp"
#include "ov/render/asset_path.hpp"
#include "ov/render/asset_source.hpp"
#include "ov/render/baked_model.hpp"
#include "ov/render/block_state_model.hpp"
#include "ov/render/model.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::render;

namespace {

struct Totals {
    usize blockstates       = 0;
    usize blockstate_errors = 0;
    usize variants          = 0;
    usize models_resolved   = 0;
    usize model_errors      = 0;
    usize quads             = 0;
    usize missing_sprites   = 0;

    /// Faces that write an explicit uv, and how many of those match the uv the
    /// generator would have produced on its own. Kept per face, because a
    /// mapping that is wrong for one direction shows up as that direction
    /// alone falling off a cliff.
    std::array<usize, kDirectionCount> explicit_uv{};
    std::array<usize, kDirectionCount> explicit_uv_agrees{};
};

// Not called `near`: that is a legacy macro in the Windows headers, and the
// CI builds this on MSVC.
bool close_enough(f32 a, f32 b) {
    return std::fabs(a - b) < 1.0e-4F;
}

void check_generated_uv(const Model& model, Totals& totals) {
    for (const auto& element : model.elements) {
        for (u8 i = 0; i < kDirectionCount; ++i) {
            const auto& face = element.faces[i];
            if (!face || !face->uv) {
                continue;
            }
            ++totals.explicit_uv[i];
            const auto generated = default_face_uv(element, static_cast<Direction>(i));
            const auto written   = *face->uv;
            if (close_enough(generated[0], written[0]) && close_enough(generated[1], written[1]) &&
                close_enough(generated[2], written[2]) && close_enough(generated[3], written[3])) {
                ++totals.explicit_uv_agrees[i];
            }
        }
    }
}

void print_model(const Model& model, const ModelVariant& variant) {
    const auto baked = bake(model, variant);
    fmt::print("  model {}  x={} y={} uvlock={}  ambient_occlusion={}  quads={}\n",
               variant.model.full(), variant.x, variant.y, variant.uvlock, baked.ambient_occlusion,
               baked.quads.size());
    for (const auto& quad : baked.quads) {
        fmt::print("    {:<34} facing={:<5} cull={:<5} tint={:<2}{}\n", quad.sprite,
                   direction_name(quad.facing),
                   quad.cullface ? direction_name(*quad.cullface) : "-", quad.tint_index,
                   quad.shade ? "" : " unshaded");
        for (const auto& vertex : quad.vertices) {
            fmt::print("      ({:7.4f} {:7.4f} {:7.4f})  uv ({:6.2f} {:6.2f})\n", vertex.position.x,
                       vertex.position.y, vertex.position.z, vertex.u, vertex.v);
        }
    }
}

int usage() {
    fmt::print(stderr,
               "usage: ov_modelbake <assets-root> [blockstate]\n"
               "\n"
               "  <assets-root>  a resource pack root, the directory holding assets/.\n"
               "                 run/assets after ov_assetimport has run.\n"
               "  [blockstate]   one block to print in full, e.g. oak_stairs.\n"
               "                 Without it, every blockstate is baked and counted.\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<usize>(argc));
    if (args.size() < 2) {
        return usage();
    }

    const std::filesystem::path root(args[1]);
    if (!std::filesystem::is_directory(root / "assets")) {
        fmt::print(stderr, "error: {} has no assets/ directory\n", root.string());
        return 2;
    }

    const DirectoryAssetSource source(root);
    ModelLoader                loader(source);

    // ── One block, in detail ────────────────────────────────────────────────
    if (args.size() >= 3) {
        const auto name = ResourceLocation::parse(args[2]);
        if (!name) {
            fmt::print(stderr, "error: '{}' is not a resource location\n", args[2]);
            return 2;
        }
        const auto bytes = source.read(blockstate_asset_path(*name));
        if (!bytes) {
            fmt::print(stderr, "error: no blockstate file for {}\n", name->full());
            return 1;
        }
        const auto file = BlockStateFile::parse(*bytes);
        if (!file) {
            fmt::print(stderr, "error: {}: {}\n", name->full(), to_string(file.error()));
            return 1;
        }

        fmt::print("{} — {}\n", name->full(), file->is_multipart() ? "multipart" : "variants");
        const auto report = [&loader](const VariantGroup& group) {
            for (const auto& variant : group.alternatives) {
                const auto model = loader.load(variant.model);
                if (!model) {
                    fmt::print("  model {}: {}\n", variant.model.full(), to_string(model.error()));
                    continue;
                }
                print_model(**model, variant);
            }
        };
        for (const auto& entry : file->variants()) {
            report(entry.group);
        }
        for (const auto& entry : file->multipart()) {
            report(entry.group);
        }
        return 0;
    }

    // ── Everything, counted ─────────────────────────────────────────────────
    Totals                       totals;
    std::map<std::string, usize> failures;
    const std::filesystem::path  blockstates = root / "assets" / "minecraft" / "blockstates";

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(blockstates)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);

    for (const auto& path : files) {
        ++totals.blockstates;
        const auto name = ResourceLocation::parse(path.stem().string());
        if (!name) {
            ++totals.blockstate_errors;
            continue;
        }

        const auto bytes = source.read(blockstate_asset_path(*name));
        const auto file =
            bytes
                ? BlockStateFile::parse(*bytes)
                : std::expected<BlockStateFile, ModelError>(std::unexpected(ModelError::NotFound));
        if (!file) {
            ++totals.blockstate_errors;
            failures[fmt::format("blockstate: {}", to_string(file.error()))] += 1;
            continue;
        }

        const auto bake_group = [&](const VariantGroup& group) {
            for (const auto& variant : group.alternatives) {
                ++totals.variants;
                const auto model = loader.load(variant.model);
                if (!model) {
                    ++totals.model_errors;
                    failures[fmt::format("model: {}", to_string(model.error()))] += 1;
                    continue;
                }
                ++totals.models_resolved;
                check_generated_uv(**model, totals);

                const auto baked = bake(**model, variant);
                totals.quads += baked.quads.size();
                for (const auto& quad : baked.quads) {
                    if (quad.sprite == kMissingSprite) {
                        ++totals.missing_sprites;
                    }
                }
            }
        };

        for (const auto& entry : file->variants()) {
            bake_group(entry.group);
        }
        for (const auto& entry : file->multipart()) {
            bake_group(entry.group);
        }
    }

    fmt::print("blockstate files ..... {:6}  ({} failed)\n", totals.blockstates,
               totals.blockstate_errors);
    fmt::print("model references ..... {:6}\n", totals.variants);
    fmt::print("models resolved ...... {:6}  ({} failed)\n", totals.models_resolved,
               totals.model_errors);
    fmt::print("distinct models cached {:6}\n", loader.cached_models());
    fmt::print("quads baked .......... {:6}\n", totals.quads);
    fmt::print("missing sprites ...... {:6}\n", totals.missing_sprites);

    fmt::print("\nhand-written uv against the generated one, per face:\n");
    usize written_total = 0;
    usize agree_total   = 0;
    for (u8 i = 0; i < kDirectionCount; ++i) {
        written_total += totals.explicit_uv[i];
        agree_total += totals.explicit_uv_agrees[i];
        if (totals.explicit_uv[i] == 0) {
            continue;
        }
        const auto percent = 100.0 * static_cast<f64>(totals.explicit_uv_agrees[i]) /
                             static_cast<f64>(totals.explicit_uv[i]);
        fmt::print("  {:<6} {:6} written, {:6} agree  {:5.1f}%\n",
                   direction_name(static_cast<Direction>(i)), totals.explicit_uv[i],
                   totals.explicit_uv_agrees[i], percent);
    }
    if (written_total > 0) {
        fmt::print("  {:<6} {:6} written, {:6} agree  {:5.1f}%\n", "all", written_total,
                   agree_total,
                   100.0 * static_cast<f64>(agree_total) / static_cast<f64>(written_total));
    }

    if (!failures.empty()) {
        fmt::print("\nfailures:\n");
        for (const auto& [reason, count] : failures) {
            fmt::print("  {:5}  {}\n", count, reason);
        }
    }

    return totals.blockstate_errors == 0 && totals.model_errors == 0 ? 0 : 1;
}
