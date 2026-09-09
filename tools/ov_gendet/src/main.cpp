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
        "\n"
        "A generation block is 4x4 chunks, so --side=2 compares 64 chunks.\n");
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
        } else {
            fmt::print("unknown argument '{}'\n", arg);
            usage();
            return 2;
        }
    }

    const auto report = ov::server::check_generation_determinism(
        options.data, options.seed, options.origin_x, options.origin_z, options.side,
        options.workers);

    if (!report.loaded) {
        fmt::print("the generator could not be loaded — nothing was compared\n");
        return 1;
    }

    fmt::print(
        "\n"
        "seed {}   blocks {}   chunks {}   workers {}\n"
        "serial   {:.3f} s\n"
        "parallel {:.3f} s   ({:.2f}x)\n"
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
        report.missing, report.block_cells, report.block_cells_differing, report.biome_cells,
        report.biome_cells_differing,
        report.identical() ? "IDENTICAL — the parallel world is the serial world"
                           : "DIFFERENT — the parallel world is not the serial world");

    return report.identical() ? 0 : 1;
}
