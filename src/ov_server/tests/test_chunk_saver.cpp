// ── streaming ── The saver writes what it was given, where it belongs, and
// nothing is lost when it is destroyed with work still queued.
//
// What this settles is the plumbing — regions, completions, shutdown. The
// bytes themselves are `world::to_nbt`'s, unchanged, and tested with it.

#include "../src/chunk_saver.hpp"

#include "ov/nbt/region.hpp"
#include "ov/world/chunk_storage.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::server;

namespace {

constexpr world::AirStates kAir{registry::BlockStateId{0}, registry::BlockStateId{12817},
                                registry::BlockStateId{12818}};

[[nodiscard]] std::shared_ptr<const world::Chunk> chunk_at(i32 x, i32 z) {
    return std::make_shared<const world::Chunk>(ChunkPos{x, z}, world::WorldShape::overworld(),
                                                kAir, nullptr);
}

/// A fresh directory under the system's temporary one, removed on exit.
struct TemporaryDirectory {
    std::filesystem::path path;

    explicit TemporaryDirectory(const std::string& name) {
        path = std::filesystem::temp_directory_path() /
               (name + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&)            = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
};

[[nodiscard]] bool readable(const std::filesystem::path& region, ChunkPos pos) {
    const auto file = nbt::RegionFile::open(region);
    if (!file) {
        return false;
    }
    const auto document = file->read_chunk(pos);
    return document && world::chunk_data_version(*document) == world::kDataVersion1201;
}

}  // namespace

TEST_CASE("the saver writes every chunk into its own region, then says so",
          "[server][streaming][save]") {
    const TemporaryDirectory directory{"ov-chunk-saver"};

    ChunkSaver   saver;
    ChunkSaveJob job;
    job.region_dir = directory.path;
    job.chunks     = {chunk_at(0, 0), chunk_at(1, 0), chunk_at(40, -3)};  // regions 0,0 and 1,-1
    saver.submit(std::move(job));
    saver.flush();

    CHECK(saver.queued() == 0);
    CHECK(saver.chunks_written() == 3);
    CHECK(readable(directory.path / "r.0.0.mca", ChunkPos{0, 0}));
    CHECK(readable(directory.path / "r.0.0.mca", ChunkPos{1, 0}));
    CHECK(readable(directory.path / "r.1.-1.mca", ChunkPos{40, -3}));

    std::vector<ChunkPos> saved;
    CHECK(saver.drain_saved(saved) == 3);
    CHECK(saved.size() == 3);
    CHECK(saver.drain_saved(saved) == 0);  // reported once
}

TEST_CASE("a saver destroyed with work queued writes it first", "[server][streaming][save]") {
    const TemporaryDirectory directory{"ov-chunk-saver-shutdown"};
    {
        ChunkSaver saver;
        for (i32 i = 0; i < 4; ++i) {
            ChunkSaveJob job;
            job.region_dir = directory.path;
            job.chunks     = {chunk_at(i, 5)};
            saver.submit(std::move(job));
        }
        // No flush: the destructor is the shutdown path.
    }
    for (i32 i = 0; i < 4; ++i) {
        CHECK(readable(directory.path / "r.0.0.mca", ChunkPos{i, 5}));
    }
}
