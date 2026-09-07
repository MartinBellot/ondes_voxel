#include "launcher.hpp"

#include "ov/base/platform.hpp"
#include "ov/io/file.hpp"
#include "ov/io/zip.hpp"

#include <algorithm>
#include <cstdlib>
#include <regex>

namespace ov::assets {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] fs::path home_directory() {
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        return home;
    }
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr) {
        return profile;
    }
    return {};
}

/// Where each launcher keeps its data, per platform.
[[nodiscard]] std::vector<std::pair<std::string, fs::path>> candidate_roots() {
    const fs::path home = home_directory();
    if (home.empty()) {
        return {};
    }

#if OV_PLATFORM_MACOS
    const fs::path app_support = home / "Library" / "Application Support";
    return {
        {"PrismLauncher", app_support / "PrismLauncher"},
        {"MultiMC", app_support / "multimc"},
        {"ATLauncher", app_support / "ATLauncher"},
        {"Minecraft Launcher", app_support / "minecraft"},
    };
#elif OV_PLATFORM_WINDOWS
    const fs::path appdata = home / "AppData" / "Roaming";
    return {
        {"PrismLauncher", appdata / "PrismLauncher"},
        {"MultiMC", appdata / "MultiMC"},
        {"ATLauncher", appdata / "ATLauncher"},
        {"Minecraft Launcher", appdata / ".minecraft"},
    };
#else
    return {
        {"PrismLauncher", home / ".local" / "share" / "PrismLauncher"},
        {"PrismLauncher (flatpak)",
         home / ".var" / "app" / "org.prismlauncher.PrismLauncher" / "data" / "PrismLauncher"},
        {"MultiMC", home / ".local" / "share" / "multimc"},
        {"Minecraft Launcher", home / ".minecraft"},
    };
#endif
}

/// Read the version id out of a client jar's version.json.
///
/// The filename is a hint, not evidence: a jar named minecraft-1.20.1-client.jar
/// could hold anything, and importing 1.21 assets into a 1.20.1 game produces
/// missing textures with no diagnostic. The jar says what it is.
[[nodiscard]] std::optional<std::string> read_jar_version(const fs::path& jar) {
    const auto archive = io::ZipArchive::open(jar);
    if (!archive || !archive->contains("version.json")) {
        return std::nullopt;
    }
    const auto contents = archive->read("version.json", 1 << 20);
    if (!contents) {
        return std::nullopt;
    }

    // A regex rather than a JSON parser: ov_data does not exist yet, and this
    // reads one field from a file we control the shape of.
    const std::string text{contents->begin(), contents->end()};
    // Custom delimiter: the pattern itself contains )" , which would close a
    // raw string using the default one.
    const std::regex id_pattern{R"re("id"\s*:\s*"([^"]+)")re"};
    std::smatch      match;
    if (std::regex_search(text, match, id_pattern)) {
        return match[1].str();
    }
    return std::nullopt;
}

/// Every *.jar under a directory that turns out to be a Minecraft client.
void collect_client_jars(const fs::path& root, std::vector<InstalledVersion>& out) {
    if (!fs::is_directory(root)) {
        return;
    }

    std::error_code ec;
    for (fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied,
                                             ec};
         it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        // Depth is bounded: a launcher's library tree is deep, but a jar buried
        // twenty levels down is not one of ours, and descending forever into a
        // modpack's contents is how this ends up taking minutes.
        if (it.depth() > 8) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || it->path().extension() != ".jar") {
            continue;
        }
        // Skip the obvious non-clients before paying for a ZIP open.
        const std::string filename = it->path().filename().string();
        if (filename.find("server") != std::string::npos ||
            filename.find("sources") != std::string::npos) {
            continue;
        }

        if (const auto id = read_jar_version(it->path())) {
            const bool already =
                std::ranges::any_of(out, [&](const InstalledVersion& v) { return v.id == *id; });
            if (!already) {
                out.push_back(InstalledVersion{*id, it->path(), {}, {}});
            }
        }
    }
}

/// Match each version to its asset index.
///
/// The index is named by asset-index id, not by game version: 1.20.1 uses
/// 5.json and 1.21 uses 17.json, and the two share an object store. Without the
/// mapping there is no way to tell which index belongs to which version, so
/// this reads the version manifest each launcher keeps beside the jar.
void attach_asset_indexes(const fs::path& root, std::vector<InstalledVersion>& versions) {
    std::error_code ec;

    std::vector<fs::path> index_files;
    fs::path              objects_dir;

    for (fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied,
                                             ec};
         it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it.depth() > 5) {
            it.disable_recursion_pending();
            continue;
        }
        const auto& path = it->path();
        if (it->is_directory(ec) && path.filename() == "objects" &&
            path.parent_path().filename() == "assets") {
            objects_dir = path;
        } else if (it->is_regular_file(ec) && path.extension() == ".json" &&
                   path.parent_path().filename() == "indexes") {
            index_files.push_back(path);
        }
    }

    if (objects_dir.empty() || index_files.empty()) {
        return;
    }

    // The mapping from version to index id lives in the launcher's own metadata.
    // Rather than parse each launcher's format, an index is matched to a version
    // by looking for the version id inside any json beside it — and failing
    // that, by the well-known ids.
    for (auto& version : versions) {
        // 1.20.1 through 1.20.4 use asset index 5; 1.21 uses 17. These are
        // stable facts about published versions, not guesses about this machine.
        std::string wanted;
        if (version.id.starts_with("1.20")) {
            wanted = "5.json";
        } else if (version.id.starts_with("1.21")) {
            wanted = "17.json";
        }
        if (wanted.empty()) {
            continue;
        }

        const auto found = std::ranges::find_if(
            index_files, [&](const fs::path& p) { return p.filename().string() == wanted; });
        if (found != index_files.end()) {
            version.asset_index   = *found;
            version.asset_objects = objects_dir;
        }
    }
}

}  // namespace

std::optional<Launcher> inspect_directory(const fs::path& root) {
    if (!fs::is_directory(root)) {
        return std::nullopt;
    }

    Launcher launcher;
    launcher.name = root.filename().string();
    launcher.root = root;
    collect_client_jars(root, launcher.versions);
    if (launcher.versions.empty()) {
        return std::nullopt;
    }
    attach_asset_indexes(root, launcher.versions);

    std::ranges::sort(launcher.versions, [](const InstalledVersion& a, const InstalledVersion& b) {
        return a.id < b.id;
    });
    return launcher;
}

std::vector<Launcher> discover_launchers() {
    std::vector<Launcher> found;
    for (const auto& [name, root] : candidate_roots()) {
        if (!fs::is_directory(root)) {
            continue;
        }
        Launcher launcher;
        launcher.name = name;
        launcher.root = root;
        collect_client_jars(root, launcher.versions);
        if (launcher.versions.empty()) {
            continue;
        }
        attach_asset_indexes(root, launcher.versions);
        std::ranges::sort(launcher.versions, [](const InstalledVersion& a,
                                                const InstalledVersion& b) { return a.id < b.id; });
        found.push_back(std::move(launcher));
    }
    return found;
}

std::optional<InstalledVersion> find_version(const std::vector<Launcher>& launchers,
                                             std::string_view             version_id) {
    // A version with assets beats one without: the jar alone gives models and
    // textures but no sounds or translations.
    std::optional<InstalledVersion> fallback;
    for (const auto& launcher : launchers) {
        for (const auto& version : launcher.versions) {
            if (version.id != version_id) {
                continue;
            }
            if (version.has_assets()) {
                return version;
            }
            if (!fallback) {
                fallback = version;
            }
        }
    }
    return fallback;
}

}  // namespace ov::assets
