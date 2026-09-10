// The player file: world/playerdata/<uuid>.dat, as vanilla 1.20.1 writes it.
//
// What a player carries between two sessions — where they stand, what they
// hold, how hurt, hungry and experienced they are, the effects still running —
// written in the game's own format so that a world saved here opens in vanilla
// with its players in place, and a vanilla world opens here the same way.
//
// ── The one rule that shapes everything below ─────────────────────────────
//
// **A key this server does not model is kept, never dropped.** A vanilla file
// carries the recipe book, the advancements' brain, the warden tracker, the
// last death location, the shoulder parrots… none of which exist here. The
// record is therefore *merged onto the compound it was read from*: the keys
// this server owns are overwritten in place (their position kept, so an
// untouched file writes back tag for tag), and everything else goes back out
// exactly as it came in. A player who crosses from vanilla to here and back
// loses nothing. See docs/provenance/donnees-joueur.md for the measurement.
//
// ── Refused, never overwritten ─────────────────────────────────────────────
//
// A file that is not gzip, not NBT, from another DataVersion, filed under the
// wrong UUID or standing in a dimension this server does not have is refused
// with its path and its reason — and the player is not let in, because a
// player let in would be saved, and the save would replace the file. The same
// rule as the chunks and level.dat.
//
// ── Layer ──────────────────────────────────────────────────────────────────
//
// ov_server, not ov_world. The record is made of protocol item stacks
// (ov_protocol, 7) and of effect, food, health and attribute state
// (ov_gameplay, 9); ov_world (6) is below both and cannot name them. What is
// generic — gzip, atomic write, NBT — is already below, in ov_io and ov_nbt.
//
// Not a public header: server.cpp and the tests include it by path, like the
// other session files.
#pragma once

#include "effect_session.hpp"
#include "survival_session.hpp"

#include "ov/base/types.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/protocol/play.hpp"
#include "ov/protocol/types.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/level_dat.hpp"

#include <array>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ov::server {

/// The player's window has 46 slots; the file numbers them differently.
inline constexpr usize kPlayerSlots = 46;

/// The file's `Slot` for a window slot, or nullopt for the four crafting-grid
/// cells and the result, which vanilla never stores.
///
///     window 36..44 hotbar     -> 0..8
///     window  9..35 backpack   -> 9..35
///     window  5..8  head..feet -> 103..100
///     window 45     off hand   -> -106
[[nodiscard]] std::optional<i8> file_slot_of_window(usize window) noexcept;

/// The inverse. Nullopt for a number the player's inventory does not have.
[[nodiscard]] std::optional<usize> window_slot_of_file(i32 slot) noexcept;

/// How item ids become names and back. Both halves are needed: the wire and
/// the window speak protocol ids, the file speaks "minecraft:cobblestone".
struct ItemNames {
    const registry::Registries*         registries{nullptr};
    std::optional<registry::RegistryId> item_registry;
};

/// Abilities, as the `abilities` compound stores them.
struct Abilities {
    bool flying{false};
    bool may_fly{false};
    bool instabuild{false};
    bool invulnerable{false};
    bool may_build{true};
    f32  fly_speed{0.05F};
    f32  walk_speed{0.1F};

    /// What the game gives each mode. Flying survives only in a mode that may
    /// fly.
    [[nodiscard]] static Abilities for_game_type(i32 game_type, bool was_flying) noexcept;
};

/// Everything this server models about a player, in the units it keeps.
struct PlayerRecord {
    // ── The entity ──────────────────────────────────────────────────────────
    f64         x{0.5};
    f64         y{0.0};
    f64         z{0.5};
    f64         motion_x{0.0};
    f64         motion_y{0.0};
    f64         motion_z{0.0};
    f32         yaw{0.0F};
    f32         pitch{0.0F};
    bool        on_ground{true};
    f32         fall_distance{0.0F};
    std::string dimension{"minecraft:overworld"};

    // ── The mode ────────────────────────────────────────────────────────────
    /// 0 survival, 1 creative, 2 adventure, 3 spectator.
    i32       game_type{0};
    /// -1 for none, which vanilla writes as well.
    i32       previous_game_type{-1};
    Abilities abilities{};

    // ── Being alive ─────────────────────────────────────────────────────────
    f32 health{20.0F};
    f32 absorption{0.0F};
    i16 hurt_time{0};
    i16 death_time{0};
    i16 air{300};

    gameplay::FoodState food{};

    i32 xp_level{0};
    /// Points into the current level — the server's integer, see
    /// SurvivalSession. The file's `XpP` is a float and is derived.
    i32 xp_points{0};
    i32 xp_total{0};

    // ── What they carry ─────────────────────────────────────────────────────
    /// In window numbering: 5..8 armour, 9..35 backpack, 36..44 hotbar, 45 off
    /// hand. 0..4 (the 2x2 grid) are folded into free slots before saving —
    /// vanilla gives them back to the inventory when the screen closes, and a
    /// file has no place for them.
    std::array<net::ItemStack, kPlayerSlots> inventory{};
    i32                                      selected_slot{0};

    // ── Effects ─────────────────────────────────────────────────────────────
    gameplay::ActiveEffects effects{};
    gameplay::AttributeMap  attributes = gameplay::AttributeMap::player();
};

/// Why a file was not read.
enum class PlayerDataErrorKind : u8 {
    Unreadable,
    NotNbt,
    NoDataVersion,
    WrongDataVersion,
    WrongUuid,
    UnsupportedDimension,
};

struct PlayerDataError {
    PlayerDataErrorKind kind;
    /// Names the file and the reason, ready for a log line or a kick message.
    std::string message;
};

/// A file, read: the record, and the compound it came from.
struct LoadedPlayer {
    PlayerRecord record;
    /// The whole root compound as read. What the next save is merged onto.
    nbt::Tag original;
    /// Inventory or ender-chest entries naming an item this registry does not
    /// have. Kept inside `original` and written back; counted here so the log
    /// can say so.
    usize unknown_items{0};
    /// ActiveEffects entries with an id outside 1..33. Same treatment.
    usize unknown_effects{0};
};

/// Read a root compound. `expected_uuid` is the file's name; a file whose
/// `UUID` says otherwise is refused. Null skips the check — level.dat's
/// `Data.Player` carries the vanilla account's UUID, never our offline one.
/// `where` names the source in messages.
[[nodiscard]] std::expected<LoadedPlayer, PlayerDataError> read_player(
    const nbt::Tag& root, const net::Uuid* expected_uuid, const ItemNames& names,
    std::string_view where);

/// The `UUID` int-array of a compound, if it has a well-formed one.
[[nodiscard]] std::optional<net::Uuid> uuid_of(const nbt::Tag& root);

/// The root compound for a record, merged onto `original` when there is one.
/// The same function writes playerdata files and level.dat's `Data.Player`.
[[nodiscard]] nbt::Tag write_player(const PlayerRecord& record, const nbt::Tag* original,
                                    const net::Uuid& uuid, const ItemNames& names);

/// gzip + NBT, the bytes of a playerdata file.
[[nodiscard]] std::vector<u8> encode_player_file(const nbt::Tag& root);

/// Parse the bytes of a playerdata file to its root compound.
[[nodiscard]] std::expected<nbt::Tag, PlayerDataError> decode_player_file(
    std::span<const u8> bytes, std::string_view where);

// ── The server's side ───────────────────────────────────────────────────────

/// A player's pose, which lives on the server's own Player record.
struct PlayerPose {
    f64  x{0.0};
    f64  y{0.0};
    f64  z{0.0};
    f32  yaw{0.0F};
    f32  pitch{0.0F};
    bool on_ground{true};
};

/// Build the record from the live state.
///
/// `carried` is the cursor stack: like the crafting grid, vanilla puts it back
/// into the inventory when the screen closes, so it is folded in too. Stacks
/// that find no free slot are reported in `overflow` — vanilla would throw
/// them on the ground; the caller decides.
[[nodiscard]] PlayerRecord capture_player(const PlayerPose& pose, i32 game_type,
                                          std::span<const net::ItemStack> inventory,
                                          const net::ItemStack& carried, i16 held_slot,
                                          const SurvivalSession& survival,
                                          const EffectSession& effects, usize* overflow = nullptr);

/// Put a record back on the live state. The effects arrive as `Added` events,
/// so the effect session's next tick sends them to the client as it would any
/// new effect; Set Health and Set Experience go out on the first survival
/// tick, as for any joining player.
void restore_player(const PlayerRecord& record, PlayerPose& pose,
                    std::span<net::ItemStack> inventory, i16& held_slot,
                    SurvivalSession& survival, EffectSession& effects);

/// The player files of one world, and level.dat's `Data.Player`.
///
/// Holds the compound each player was last read or written as, so a save
/// merges onto what vanilla wrote rather than onto nothing. Called from the
/// network thread (join, leave) and the tick thread (autosave): one mutex,
/// held around the map and the write — never around the world.
class PlayerDataStore {
public:
    /// `host` is the singleplayer host's name, empty on a dedicated server.
    PlayerDataStore(std::filesystem::path world_dir, ItemNames names, std::string host);

    /// Read level.dat's `Data.Player`, if any. Called once at start-up.
    void load_level_player(const std::filesystem::path& level_dat);

    /// The player's saved state. Nullopt for a first arrival. An error when a
    /// file exists and cannot be used — the caller refuses the login.
    ///
    /// The singleplayer host reads `Data.Player` first when level.dat has one:
    /// that is where vanilla keeps the host, under an account UUID ours does
    /// not share, and it wins over the playerdata file in vanilla too.
    [[nodiscard]] std::expected<std::optional<LoadedPlayer>, PlayerDataError> load(
        const net::Uuid& uuid, std::string_view name);

    /// Merge, encode and write `<uuid>.dat`, keeping the previous file as
    /// `<uuid>.dat_old`. For the host, `Data.Player` is updated as well and
    /// goes out with the next level.dat. False on a write failure, logged.
    bool save(const net::Uuid& uuid, std::string_view name, const PlayerRecord& record);

    /// level.dat as `world::encode_level_dat` would write it, plus
    /// `Data.Player` when there is one to carry.
    [[nodiscard]] std::vector<u8> encode_level_dat(const world::LevelSettings& settings);

    [[nodiscard]] std::filesystem::path file_of(const net::Uuid& uuid) const;

private:
    std::filesystem::path                     directory_;
    ItemNames                                 names_;
    std::string                               host_;
    std::mutex                                mutex_;
    std::unordered_map<std::string, nbt::Tag> originals_;
    /// Players whose file was refused. `save` will not write over it, even if
    /// a caller forgets to keep them out.
    std::unordered_set<std::string> refused_;
    std::optional<nbt::Tag>         level_player_;
};

}  // namespace ov::server
