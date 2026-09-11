// server.properties — the dedicated server's settings, in vanilla's file.
//
// The file is a `java.util.Properties` text: `key=value` lines, `#` comments,
// backslash escapes. On every start vanilla reads it, fills in every key it
// knows with its default, and writes it back — header, date, then the keys in
// the order its JDK's map iterates them (java_compat.hpp). Keys it does not
// know are kept. This does the same, so that a file moves between the two
// servers without losing a line: docs/provenance/serveur-dedie.md compares a
// file the real 1.20.1 server wrote with ours, key by key.
//
// What a value *means* is `DedicatedSettings`: vanilla's parsing rules (an
// unreadable number falls back to the default; a boolean is true only when it
// says "true"; difficulty and game mode by name or by number), and the file is
// rewritten with the value as it was understood.
#pragma once

#include "java_compat.hpp"

#include "ov/base/types.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ov::server::admin {

/// One key 1.20.1's dedicated server knows, and what it writes when the key is
/// missing. In the order the server reads them, which decides where a key
/// lands in its bucket when two keys share one.
struct PropertyDefault {
    std::string_view key;
    std::string_view value;
};

/// java.util.Properties.load, for text already decoded.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_properties(
    std::string_view text);

/// java.util.Properties' escaping of one key or value. `unicode` escapes what
/// is outside printable ASCII as `\uXXXX` (the OutputStream form of store).
[[nodiscard]] std::string escape_property(std::string_view text, bool is_key, bool unicode);

/// Bytes on disk to text: UTF-8 when the bytes are valid UTF-8, else
/// ISO-8859-1 — what a file edited in any editor can be.
[[nodiscard]] std::string decode_properties_bytes(std::string_view bytes);

class ServerProperties {
public:
    /// Every key of 1.20.1, with its default.
    [[nodiscard]] static std::span<const PropertyDefault> vanilla_defaults();

    /// Read the text of an existing file.
    [[nodiscard]] static ServerProperties from_text(std::string_view text);

    /// A missing value is filled with its default; what `DedicatedSettings`
    /// understood is written back by `set`.
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
    void                                     set(std::string_view key, std::string value);

    /// Fill in every known key that is missing, in the order vanilla asks.
    void fill_defaults();

    /// The file as vanilla writes it: `#Minecraft server properties`, the
    /// date, then every key in the JDK map's order.
    [[nodiscard]] std::string render(std::string_view date_line) const;

    /// Keys in the order they were first met: the file's, then the defaults'.
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const noexcept {
        return entries_;
    }

    /// Keys in the order vanilla would write them.
    [[nodiscard]] std::vector<std::string> write_order() const;

private:
    std::vector<std::pair<std::string, std::string>> entries_;
    /// How many keys the file itself held, first in `entries_`.
    usize from_file_{0};
};

/// What the values mean, read with vanilla's rules. Reading also normalises
/// the properties: an unreadable value is replaced by the default it falls
/// back to, as vanilla rewrites it.
struct DedicatedSettings {
    std::string motd{"A Minecraft Server"};
    i32         max_players{20};
    i32         server_port{25565};
    std::string server_ip;
    i32         view_distance{10};
    i32         simulation_distance{10};
    std::string level_name{"world"};
    std::string level_seed;
    std::string level_type{"minecraft:normal"};
    bool        generate_structures{true};
    u8          gamemode{0};
    bool        force_gamemode{false};
    u8          difficulty{1};
    bool        hardcore{false};
    bool        pvp{true};
    i32         spawn_protection{16};
    bool        allow_nether{true};
    bool        allow_flight{false};
    bool        white_list{false};
    bool        enforce_whitelist{false};
    bool        online_mode{true};
    bool        enforce_secure_profile{true};
    bool        prevent_proxy_connections{false};
    i32         network_compression_threshold{256};
    i32         player_idle_timeout{0};
    i64         max_tick_time{60000};
    bool        enable_rcon{false};
    i32         rcon_port{25575};
    std::string rcon_password;
    bool        broadcast_rcon_to_ops{true};
    bool        broadcast_console_to_ops{true};
    bool        enable_query{false};
    i32         query_port{25565};
    bool        enable_status{true};
    bool        hide_online_players{false};
    i32         op_permission_level{4};
    i32         function_permission_level{2};
    bool        spawn_monsters{true};
    bool        spawn_animals{true};
    bool        spawn_npcs{true};
    i32         max_world_size{29999984};
    i32         entity_broadcast_range_percentage{100};
    i32         rate_limit{0};
    bool        sync_chunk_writes{true};

    [[nodiscard]] static DedicatedSettings read(ServerProperties& properties);
};

/// server.properties as a start finds it and leaves it.
struct PropertiesFile {
    ServerProperties  properties;
    DedicatedSettings settings;
    /// False on a first start: the file was just written with the defaults.
    bool existed{false};
};

/// Read (or create) the file, understand it, and write it back the way
/// vanilla does on every start. The date line comes from `date_line`.
[[nodiscard]] PropertiesFile load_properties_file(const std::filesystem::path& path,
                                                  std::string_view             date_line);

/// Write the file again after a command changed a key.
bool save_properties_file(const std::filesystem::path& path, const ServerProperties& properties,
                          std::string_view date_line);

/// The name vanilla writes a game mode or a difficulty under.
[[nodiscard]] std::string_view game_mode_name(u8 mode) noexcept;
[[nodiscard]] std::string_view difficulty_name(u8 difficulty) noexcept;

}  // namespace ov::server::admin
