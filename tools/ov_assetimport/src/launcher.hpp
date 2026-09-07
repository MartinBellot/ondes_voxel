// Finding the player's own Minecraft installation.
//
// Ondes VOXEL ships no game content. Everything it renders comes from a copy of
// Minecraft the player already owns, on their own machine, read at runtime and
// written into ./run/, which is never committed and never distributed.
//
// That makes locating the installation a first-class problem rather than a
// convenience: if this is wrong or vague, the only symptom the player sees is a
// game with no textures and no explanation.
#pragma once

#include "ov/base/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ov::assets {

/// A Minecraft version found on disk.
struct InstalledVersion {
    std::string           id;          // "1.20.1"
    std::filesystem::path client_jar;  // models, blockstates, fonts, textures
    /// Asset index naming the sounds and language files, or empty when absent.
    std::filesystem::path asset_index;
    /// Content-addressed object store the index points into.
    std::filesystem::path asset_objects;

    [[nodiscard]] bool has_assets() const noexcept {
        return !asset_index.empty() && !asset_objects.empty();
    }
};

/// A launcher found on this machine.
struct Launcher {
    std::string                   name;  // "PrismLauncher", "MultiMC", "Minecraft Launcher"
    std::filesystem::path         root;
    std::vector<InstalledVersion> versions;
};

/// Search the usual locations for every launcher we know about.
///
/// Returns everything found rather than the first match: a machine commonly has
/// several launchers, and the one holding 1.20.1 is not necessarily the first.
[[nodiscard]] std::vector<Launcher> discover_launchers();

/// Search a specific directory, for `--minecraft-dir`.
[[nodiscard]] std::optional<Launcher> inspect_directory(const std::filesystem::path& root);

/// The requested version across all launchers, or nullopt.
[[nodiscard]] std::optional<InstalledVersion> find_version(const std::vector<Launcher>& launchers,
                                                           std::string_view             version_id);

}  // namespace ov::assets
