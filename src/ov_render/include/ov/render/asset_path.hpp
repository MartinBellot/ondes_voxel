// Where each kind of client asset lives inside a resource pack.
//
// The identifier type is ov::ResourceLocation, from ov_base. It already warns
// that `.` and `/` are legal path characters, so `minecraft:../../etc/passwd`
// is a *well-formed* location — turning one into a file path is exactly the
// place that has to be checked, and AssetSource is where that check lives.
#pragma once

#include "ov/base/resource_location.hpp"

#include <string>

namespace ov::render {

/// `minecraft:block/cube` -> `assets/minecraft/models/block/cube.json`.
[[nodiscard]] std::string model_asset_path(const ResourceLocation& location);

/// `minecraft:stone` -> `assets/minecraft/blockstates/stone.json`.
[[nodiscard]] std::string blockstate_asset_path(const ResourceLocation& location);

/// `minecraft:block/stone` -> `assets/minecraft/textures/block/stone.png`.
[[nodiscard]] std::string texture_asset_path(const ResourceLocation& location);

/// Is this a `builtin/...` model reference?
///
/// `builtin/generated` and `builtin/entity` have no file behind them: they tell
/// the game to build geometry in code. Blocks reach them only through item
/// models, but a parent chain walker still has to stop there rather than report
/// a missing file.
[[nodiscard]] bool is_builtin_model(const ResourceLocation& location) noexcept;

}  // namespace ov::render
