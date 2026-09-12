#define OV_LOG_CATEGORY "gendet"

#include "ov/base/log.hpp"
#include "ov/server/generation_check.hpp"

#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::filesystem::path data{"data"};
    ov::i64               seed{1234567890};
    ov::i32               origin_x{0};
    ov::i32               origin_z{0};
    ov::i32               side{1};
    ov::usize             workers{4};
    // ── streaming ── the serial arm alone: its digest and its time
    bool                  serial_only{false};
    // ── structures ── --export mode
    std::filesystem::path export_dir;
    std::string           dimension{"overworld"};
    ov::i32               chunks[4]{0, 0, 0, 0};
};

void usage() {
    fmt::print(
        "ov_gendet — generate the same world serially and in parallel, and compare\n"
        "\n"
        "  --data=<dir>       data root (default: data)\n"
        "  --seed=<n>         world seed (default: 1234567890)\n"
        "  --origin=<x>,<z>   first generation block (default: 0,0)\n"
        "  --side=<n>         generation blocks per side (default: 1)\n"
        "  --workers=<n>      threads in the parallel arm (default: 4)\n"
        "  --serial-only      generate the serial arm only (digest and timing)\n"
        "\n"
        "A generation block is 4x4 chunks, so --side=2 compares 64 chunks.\n"
        "The digest covers every block, biome cell, stored heightmap and block\n"
        "entity of the serial arm: two binaries that generate the same world\n"
        "print the same digest.\n"
        "\n"
        "  --export=<dir>             instead: generate and write region files there\n"
        "  --dimension=<name>         overworld (default), nether or end\n"
        "  --chunks=<x0>,<z0>,<x1>,<z1>  the chunks to export, inclusive\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        }
        if (arg.starts_with("--data=")) {
            options.data = std::string{arg.substr(7)};
        } else if (arg.starts_with("--seed=")) {
            options.seed = std::strtoll(std::string{arg.substr(7)}.c_str(), nullptr, 10);
        } else if (arg.starts_with("--side=")) {
            options.side =
                static_cast<ov::i32>(std::strtol(std::string{arg.substr(7)}.c_str(), nullptr, 10));
        } else if (arg.starts_with("--workers=")) {
            options.workers = static_cast<ov::usize>(
                std::strtoul(std::string{arg.substr(10)}.c_str(), nullptr, 10));
        } else if (arg == "--serial-only") {
            options.serial_only = true;
        } else if (arg.starts_with("--origin=")) {
            const std::string value{arg.substr(9)};
            const auto        comma = value.find(',');
            if (comma == std::string::npos) {
                fmt::print("--origin wants <x>,<z>\n");
                return 2;
            }
            options.origin_x = static_cast<ov::i32>(std::strtol(value.c_str(), nullptr, 10));
            options.origin_z =
                static_cast<ov::i32>(std::strtol(value.c_str() + comma + 1, nullptr, 10));
        } else if (arg.starts_with("--export=")) {
            options.export_dir = std::string{arg.substr(9)};
        } else if (arg.starts_with("--dimension=")) {
            options.dimension = std::string{arg.substr(12)};
        } else if (arg.starts_with("--chunks=")) {
            const std::string value{arg.substr(9)};
            const char*       cursor = value.c_str();
            for (ov::i32& coordinate : options.chunks) {
                char* end  = nullptr;
                coordinate = static_cast<ov::i32>(std::strtol(cursor, &end, 10));
                cursor     = *end == ',' ? end + 1 : end;
            }
        } else {
            fmt::print("unknown argument '{}'\n", arg);
            usage();
            return 2;
        }
    }

    // ── structures ──
    if (!options.export_dir.empty()) {
        const auto written = ov::server::export_generated_chunks(
            options.data, options.seed, options.dimension, options.chunks[0], options.chunks[1],
            options.chunks[2], options.chunks[3], options.export_dir);
        if (!written.loaded) {
            fmt::print("the generator could not be loaded — nothing was written\n");
            return 1;
        }
        fmt::print("seed {}  {}  squares {}  chunks {}  {:.1f} s  -> {}\n", options.seed,
                   options.dimension, written.squares, written.chunks, written.seconds,
                   options.export_dir.string());
        return 0;
    }

    const auto report = ov::server::check_generation_determinism(
        options.data, options.seed, options.origin_x, options.origin_z, options.side,
        options.workers, !options.serial_only);

    if (!report.loaded) {
        fmt::print("the generator could not be loaded — nothing was compared\n");
        return 1;
    }

    if (options.serial_only) {
        fmt::print(
            "\n"
            "seed {}   blocks {}   chunks {}   origin {},{}\n"
            "serial   {:.3f} s   ({:.1f} ms per chunk)\n"
            "digest   {:016x}\n",
            options.seed, report.blocks, report.chunks, options.origin_x, options.origin_z,
            report.serial_seconds,
            report.chunks > 0 ? report.serial_seconds * 1000.0 / static_cast<double>(report.chunks)
                              : 0.0,
            report.digest);
        return 0;
    }

    fmt::print(
        "\n"
        "seed {}   blocks {}   chunks {}   workers {}\n"
        "serial   {:.3f} s\n"
        "parallel {:.3f} s   ({:.2f}x)\n"
        "digest   {:016x}\n"
        "\n"
        "chunks only one arm produced : {}\n"
        "block cells compared         : {}\n"
        "block cells differing        : {}\n"
        "biome cells compared         : {}\n"
        "biome cells differing        : {}\n"
        "\n"
        "{}\n",
        options.seed, report.blocks, report.chunks, options.workers, report.serial_seconds,
        report.parallel_seconds,
        report.parallel_seconds > 0.0 ? report.serial_seconds / report.parallel_seconds : 0.0,
        report.digest, report.missing, report.block_cells, report.block_cells_differing,
        report.biome_cells, report.biome_cells_differing,
        report.identical() ? "IDENTICAL — the parallel world is the serial world"
                           : "DIFFERENT — the parallel world is not the serial world");

    return report.identical() ? 0 : 1;
}
