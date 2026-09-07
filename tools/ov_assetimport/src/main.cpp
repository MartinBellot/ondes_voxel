// ov-assetimport — assemble run/assets/ from what the player already owns.
//
// Ondes VOXEL ships no game content. Mojang's usage guidelines forbid
// redistributing it, and the Faithful license attaches conditions to theirs, so
// the repository holds none at all. Instead this tool reads a Minecraft
// installation already on the machine and writes the result into ./run/, which
// is gitignored and never distributed.
//
// What comes from where, and why both are needed:
//
//   ressourcepacks/*   Textures. A resource pack is textures and almost nothing
//                      else — Faithful 32x has 3292 PNGs and no models at all.
//   client jar         Block models, blockstates, fonts, language files, and
//                      any texture no pack overrides. 20953 entries, of which
//                      1005 blockstates and 3691 models. Without these there is
//                      no geometry, only pixels with nowhere to go.
//   asset index        Sounds and translations, content-addressed: the index
//                      maps a name like minecraft/sounds/step/stone1.ogg to a
//                      hash under assets/objects/.
//
// Packs stack the way they do in the real game: later packs override earlier
// ones, and the jar is the bottom of the stack.

#define OV_LOG_CATEGORY "assetimport"

#include "launcher.hpp"

#include "ov/base/log.hpp"
#include "ov/io/file.hpp"
#include "ov/io/zip.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ov;
namespace fs = std::filesystem;

constexpr std::string_view kTargetVersion = "1.20.1";

/// Subtrees taken from the client jar.
///
/// Deliberately a list rather than "everything under assets/": the jar also
/// holds the game's own code and the vanilla datapack, neither of which belongs
/// in a resource directory.
constexpr std::string_view kJarPrefixes[] = {
    "assets/minecraft/blockstates/",
    "assets/minecraft/models/",
    "assets/minecraft/textures/",
    "assets/minecraft/font/",
    "assets/minecraft/particles/",
    "assets/minecraft/atlases/",
    "assets/minecraft/shaders/",
    "assets/minecraft/texts/",
    // en_us.json only; every other language lives in the asset index.
    "assets/minecraft/lang/",
};

struct Options {
    fs::path    minecraft_dir;
    fs::path    output = "run/assets";
    fs::path    packs  = "ressourcepacks";
    std::string version{kTargetVersion};
    bool        with_sounds = false;
    bool        list_only   = false;
    bool        dry_run     = false;
    bool        show_help   = false;
};

struct Origin {
    std::string source;  // "Faithful 32x", "client jar", "asset index"
    usize       bytes{0};
};

/// Where every written file came from, so docs/ASSETS.md can be regenerated and
/// the licence of each byte on disk stays answerable.
using Provenance = std::map<std::string, Origin>;

void print_usage() {
    fmt::print(
        "ov-assetimport — assemble run/assets/ from your own Minecraft install\n"
        "\n"
        "  --minecraft-dir=<path>   use this installation instead of searching\n"
        "  --output=<path>          where to write (default: run/assets)\n"
        "  --packs=<path>           resource packs to stack (default: ressourcepacks)\n"
        "  --version=<id>           game version (default: {})\n"
        "  --sounds                 also import sounds (~hundreds of MB)\n"
        "  --list                   show what was found and stop\n"
        "  --dry-run                report what would be written, write nothing\n"
        "  --help, -h               this message\n"
        "\n"
        "Nothing is downloaded and nothing is redistributed. The output directory\n"
        "is gitignored.\n"
        "\n"
        "Not an official Minecraft product. Not approved by or associated with Mojang.\n",
        kTargetVersion);
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
        } else if (arg == "--sounds") {
            options.with_sounds = true;
        } else if (arg == "--list") {
            options.list_only = true;
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg.starts_with("--minecraft-dir=")) {
            options.minecraft_dir = arg.substr(16);
        } else if (arg.starts_with("--output=")) {
            options.output = arg.substr(9);
        } else if (arg.starts_with("--packs=")) {
            options.packs = arg.substr(8);
        } else if (arg.starts_with("--version=")) {
            options.version = arg.substr(10);
        } else {
            fmt::print(stderr, "unknown argument '{}' (try --help)\n", arg);
        }
    }
    return options;
}

void report_installations(const std::vector<assets::Launcher>& launchers) {
    if (launchers.empty()) {
        fmt::print("  no Minecraft installation found\n");
        return;
    }
    for (const auto& launcher : launchers) {
        fmt::print("  {} — {}\n", launcher.name, launcher.root.string());
        for (const auto& version : launcher.versions) {
            fmt::print("      {:10s} {}\n", version.id,
                       version.has_assets() ? "jar + assets" : "jar only");
        }
    }
}

/// Copy an entry out of an archive into the output tree.
bool write_entry(const fs::path& output, std::string_view relative, const std::vector<u8>& contents,
                 bool dry_run) {
    if (dry_run) {
        return true;
    }
    const fs::path  destination = output / relative;
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        return false;
    }
    return io::write_file_atomic(destination, contents).has_value();
}

/// Import a resource pack that is a plain directory rather than a zip.
///
/// This is the common case, not an edge case: a pack downloaded and unzipped
/// into ressourcepacks/ is a directory, and Faithful 32x arrives that way.
usize import_directory(const fs::path& root, const fs::path& output, std::string_view label,
                       Provenance& provenance, bool dry_run) {
    usize written = 0;

    std::error_code ec;
    for (fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied,
                                             ec};
         it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }

        const fs::path relative_path = fs::relative(it->path(), root, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        // Generic form so the key matches a zip pack's entry names on every
        // platform; Windows would otherwise produce backslashes.
        const std::string relative = relative_path.generic_string();

        // The pack's own metadata and licence are not game assets, but the
        // licence must travel with anything derived from the pack, so it is
        // copied rather than skipped.
        if (relative == "pack.png" || relative.starts_with(".")) {
            continue;
        }

        const auto contents = io::read_file(it->path());
        if (!contents) {
            OV_LOG_WARN("{}: cannot read {}", label, relative);
            continue;
        }
        if (!write_entry(output, relative, *contents, dry_run)) {
            OV_LOG_WARN("{}: cannot write {}", label, relative);
            continue;
        }

        provenance[relative] = Origin{std::string{label}, contents->size()};
        ++written;
    }
    return written;
}

/// One entry of the asset index.
struct IndexedAsset {
    std::string name;  // "minecraft/lang/fr_fr.json"
    std::string hash;  // sha1, and also the path under objects/
};

/// Parse the asset index.
///
/// It is a flat JSON object mapping a logical name to {"hash": ..., "size": ...},
/// with no nesting beyond that. Scanned rather than parsed because ov_data does
/// not exist yet and this is one file of a shape Mojang controls; the loader
/// moves to the real JSON parser when there is one.
[[nodiscard]] std::vector<IndexedAsset> read_asset_index(const fs::path& index_path) {
    std::vector<IndexedAsset> assets;

    const auto contents = io::read_file(index_path, 64ull * 1024 * 1024);
    if (!contents) {
        return assets;
    }
    const std::string text{contents->begin(), contents->end()};

    // "name" : { "hash" : "hex40" , "size" : N }
    const std::regex pattern{R"re("([^"]+)"\s*:\s*\{\s*"hash"\s*:\s*"([0-9a-f]{40})")re"};

    for (auto it = std::sregex_iterator{text.begin(), text.end(), pattern};
         it != std::sregex_iterator{}; ++it) {
        assets.push_back(IndexedAsset{(*it)[1].str(), (*it)[2].str()});
    }
    return assets;
}

/// Copy assets named by the index out of the content-addressed object store.
///
/// Objects live at objects/<first two hex digits>/<full hash>, shared between
/// versions, so the same file backs several installs. Copying rather than
/// linking keeps run/ self-contained and independent of the launcher's store.
usize import_indexed(const std::vector<IndexedAsset>& assets, const fs::path& objects_root,
                     const fs::path& output, std::string_view prefix, std::string_view label,
                     Provenance& provenance, bool dry_run) {
    usize written = 0;

    for (const auto& asset : assets) {
        if (!prefix.empty() && !asset.name.starts_with(prefix)) {
            continue;
        }
        const fs::path object   = objects_root / asset.hash.substr(0, 2) / asset.hash;
        const auto     contents = io::read_file(object);
        if (!contents) {
            continue;
        }

        // The index names things as "minecraft/lang/fr_fr.json"; the resource
        // tree wants "assets/minecraft/lang/fr_fr.json".
        const std::string relative = "assets/" + asset.name;
        if (provenance.contains(relative)) {
            continue;  // a pack already provided it
        }
        if (!write_entry(output, relative, *contents, dry_run)) {
            continue;
        }

        provenance[relative] = Origin{std::string{label}, contents->size()};
        ++written;
    }
    return written;
}

/// Pull the listed subtrees out of a ZIP into the output tree.
usize import_archive(const io::ZipArchive& archive, const fs::path& output, std::string_view label,
                     std::span<const std::string_view> prefixes, Provenance& provenance,
                     bool dry_run, bool overwrite) {
    usize written = 0;

    for (const auto& entry : archive.entries()) {
        if (entry.is_directory()) {
            continue;
        }
        const bool wanted =
            prefixes.empty() || std::ranges::any_of(prefixes, [&](std::string_view prefix) {
                return entry.name.starts_with(prefix);
            });
        if (!wanted) {
            continue;
        }
        // Later packs win; the jar fills only what nothing else provided.
        if (!overwrite && provenance.contains(entry.name)) {
            continue;
        }

        const auto contents = archive.read(entry.name);
        if (!contents) {
            OV_LOG_WARN("{}: cannot read {} ({})", label, entry.name,
                        io::to_string(contents.error()));
            continue;
        }
        if (!write_entry(output, entry.name, *contents, dry_run)) {
            OV_LOG_WARN("{}: cannot write {}", label, entry.name);
            continue;
        }

        provenance[entry.name] = Origin{std::string{label}, contents->size()};
        ++written;
    }
    return written;
}

}  // namespace

int main(int argc, char** argv) {
    ov::set_log_level(ov::LogLevel::Info);

    const Options options = parse_args(argc, argv);
    if (options.show_help) {
        print_usage();
        return 0;
    }

    // ── Find the installation ───────────────────────────────────────────────
    fmt::print("\nMinecraft installations\n");
    std::vector<assets::Launcher> launchers;
    if (!options.minecraft_dir.empty()) {
        if (auto launcher = assets::inspect_directory(options.minecraft_dir)) {
            launchers.push_back(std::move(*launcher));
        } else {
            fmt::print(stderr, "  no Minecraft version found under {}\n",
                       options.minecraft_dir.string());
            return 1;
        }
    } else {
        launchers = assets::discover_launchers();
    }
    report_installations(launchers);

    const auto version = assets::find_version(launchers, options.version);
    if (!version) {
        fmt::print(stderr,
                   "\nMinecraft {} was not found.\n"
                   "\n"
                   "It is not optional: a resource pack carries textures only, so the block\n"
                   "models, blockstates, fonts and language files have to come from the game\n"
                   "itself.\n"
                   "\n"
                   "In PrismLauncher: Add Instance -> Vanilla -> {} -> OK, then launch it\n"
                   "once so the assets download. Or pass --minecraft-dir=<path>.\n",
                   options.version, options.version);
        return 1;
    }

    fmt::print("\nUsing Minecraft {}\n", version->id);
    fmt::print("  client jar ..... {}\n", version->client_jar.string());
    if (version->has_assets()) {
        fmt::print("  asset index .... {}\n", version->asset_index.filename().string());
    } else {
        fmt::print(
            "  asset index .... \033[0;33mnot found — sounds and translations will be "
            "missing\033[0m\n");
    }

    // ── Resource packs ──────────────────────────────────────────────────────
    std::vector<fs::path> packs;
    if (fs::is_directory(options.packs)) {
        for (const auto& entry : fs::directory_iterator{options.packs}) {
            if (entry.is_directory() || entry.path().extension() == ".zip") {
                packs.push_back(entry.path());
            }
        }
        std::ranges::sort(packs);
    }

    fmt::print("\nResource packs ({})\n", options.packs.string());
    if (packs.empty()) {
        fmt::print("  none — the game will fall back to the generated atlas\n");
    }
    for (const auto& pack : packs) {
        fmt::print("  {}\n", pack.filename().string());
    }

    if (options.list_only) {
        fmt::print("\n");
        return 0;
    }

    // ── Import ──────────────────────────────────────────────────────────────
    fmt::print("\nImporting into {}{}\n", options.output.string(),
               options.dry_run ? " (dry run)" : "");

    Provenance provenance;

    // Packs first, highest priority last, each overriding what came before —
    // the order the real game applies them in.
    for (const auto& pack : packs) {
        if (fs::is_directory(pack)) {
            const usize written = import_directory(pack, options.output, pack.filename().string(),
                                                   provenance, options.dry_run);
            fmt::print("  {:30s} {} files\n", pack.filename().string(), written);
            continue;
        }

        const auto archive = io::ZipArchive::open(pack);
        if (!archive) {
            fmt::print("  {:30s} \033[0;31m{}\033[0m\n", pack.filename().string(),
                       io::to_string(archive.error()));
            continue;
        }
        const usize written = import_archive(*archive, options.output, pack.filename().string(), {},
                                             provenance, options.dry_run, /*overwrite=*/true);
        fmt::print("  {:30s} {} files\n", pack.filename().string(), written);
    }

    // The jar fills in everything no pack provided.
    const auto jar = io::ZipArchive::open(version->client_jar);
    if (!jar) {
        fmt::print(stderr, "cannot open the client jar: {}\n", io::to_string(jar.error()));
        return 1;
    }
    const usize from_jar = import_archive(*jar, options.output, "client jar", kJarPrefixes,
                                          provenance, options.dry_run, /*overwrite=*/false);
    fmt::print("  {:30s} {} files\n", "client jar", from_jar);

    // Translations and sounds are content-addressed in the launcher's object
    // store rather than packed in the jar; only en_us.json ships inside it.
    if (version->has_assets()) {
        const auto indexed = read_asset_index(version->asset_index);

        const usize languages =
            import_indexed(indexed, version->asset_objects, options.output, "minecraft/lang/",
                           "asset index", provenance, options.dry_run);
        fmt::print("  {:30s} {} files\n", "asset index (lang)", languages);

        if (options.with_sounds) {
            const usize sounds =
                import_indexed(indexed, version->asset_objects, options.output, "minecraft/sounds/",
                               "asset index", provenance, options.dry_run);
            fmt::print("  {:30s} {} files\n", "asset index (sounds)", sounds);
        } else {
            const auto sound_count = std::ranges::count_if(indexed, [](const IndexedAsset& a) {
                return a.name.starts_with("minecraft/sounds/");
            });
            fmt::print("  {:30s} {} skipped (pass --sounds; not needed before M9)\n",
                       "asset index (sounds)", sound_count);
        }
    }

    // ── Summary ─────────────────────────────────────────────────────────────
    usize                        total_bytes = 0;
    std::map<std::string, usize> by_source;
    for (const auto& [name, origin] : provenance) {
        total_bytes += origin.bytes;
        ++by_source[origin.source];
    }

    fmt::print("\n{} files, {:.1f} MB\n", provenance.size(),
               static_cast<double>(total_bytes) / (1024.0 * 1024.0));
    for (const auto& [source, count] : by_source) {
        fmt::print("  {:30s} {}\n", source, count);
    }

    // ── Provenance manifest ─────────────────────────────────────────────────
    // Written next to the assets rather than into docs/, because it describes
    // this machine's import and not the repository. It is what makes "which
    // licence covers this file?" answerable for every byte in run/.
    if (!options.dry_run) {
        std::string manifest;
        manifest += "# Ondes VOXEL — asset provenance\n#\n";
        manifest += fmt::format("# Minecraft {} imported from {}\n", version->id,
                                version->client_jar.string());
        manifest += "#\n";
        manifest +=
            "# Nothing here is redistributable. Files marked 'client jar' or 'asset index'\n"
            "# are Mojang's, covered by the Minecraft EULA; files from a resource pack are\n"
            "# covered by that pack's own licence, which ships inside the pack directory.\n"
            "# This directory is gitignored and must never be committed or published.\n#\n";
        manifest += fmt::format("# {} files, {} bytes\n\n", provenance.size(), total_bytes);

        for (const auto& [name, origin] : provenance) {
            manifest += fmt::format("{}\t{}\t{}\n", origin.source, origin.bytes, name);
        }

        const std::vector<u8> bytes{manifest.begin(), manifest.end()};
        const fs::path        manifest_path = options.output / "PROVENANCE.tsv";
        if (io::write_file_atomic(manifest_path, bytes)) {
            fmt::print("\nProvenance written to {}\n", manifest_path.string());
        } else {
            OV_LOG_WARN("could not write {}", manifest_path.string());
        }
    }

    // The Faithful licence requires that its LICENSE.txt travel unmodified with
    // anything derived from the pack, and that credit be given. Say so here
    // rather than assume anyone reads the pack directory.
    for (const auto& pack : packs) {
        const fs::path licence = pack / "LICENSE.txt";
        if (fs::is_regular_file(licence)) {
            fmt::print("\n{} carries a licence: {}\n", pack.filename().string(), licence.string());
            fmt::print(
                "  It travels with anything derived from the pack, and the project\n"
                "  must never be monetized. See NOTICE.\n");
        }
    }

    if (options.dry_run) {
        fmt::print("\nDry run: nothing was written.\n");
    }
    fmt::print("\n");
    return 0;
}
