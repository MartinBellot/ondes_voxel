// ops.json: who is an operator, and at which level.
//
// Vanilla's file, in vanilla's shape, next to the server — so an ops.json
// written by a vanilla server is read here and one written here is read by
// vanilla:
//
//     [
//       {
//         "uuid": "f3f20367-e988-37d8-ac20-1a1929fd1e59",
//         "name": "ovprobe",
//         "level": 4,
//         "bypassesPlayerLimit": false
//       }
//     ]
//
// Offline mode means the uuid is the name's offline uuid, which is also what
// vanilla writes for an offline server.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ov::server::cmd {

struct OpEntry {
    net::Uuid   uuid{};
    std::string name;
    i32         level{4};
    bool        bypasses_player_limit{false};
};

class OpList {
public:
    OpList() = default;
    explicit OpList(std::filesystem::path path) : path_{std::move(path)} {}

    /// Read the file. A missing file is an empty list; a malformed one is an
    /// empty list **and** a false return, so the caller can say so rather
    /// than silently de-op everyone.
    bool load();
    bool save() const;

    [[nodiscard]] std::optional<i32> level_of(const net::Uuid& uuid) const;

    /// False when already present, which is vanilla's "Nothing changed".
    bool add(OpEntry entry);
    bool remove(const net::Uuid& uuid);

    [[nodiscard]] const std::vector<OpEntry>& entries() const noexcept { return entries_; }

    /// The file's text, exactly as it would be written.
    [[nodiscard]] std::string to_json() const;
    bool                      from_json(std::string_view text);

private:
    std::filesystem::path path_;
    std::vector<OpEntry>  entries_;
    /// ── dedicated server administration ── the most entries the list ever
    /// held: vanilla's HashMap never shrinks, and its size decides the order
    /// the file is written in (admin/java_compat.hpp).
    usize high_water_{0};
};

}  // namespace ov::server::cmd
