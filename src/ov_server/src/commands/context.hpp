// What a command runs as, and what it can see.
//
// The engine never touches the server's own records. A command is run by a
// `CommandSource` — a player, or the console — and sees the world through a
// snapshot of `EntityInfo`, built by the server once per command. That is what
// lets every parser and every selector be a pure function with unit tests,
// and what keeps this directory free of the server's locals.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/math/vec.hpp"
#include "ov/protocol/types.hpp"

#include <array>
#include <string>

namespace ov::server::cmd {

/// Vanilla's permission levels. 0 everyone, 2 command blocks and ops' game
/// commands, 3 multiplayer management, 4 the console and the server itself.
inline constexpr i32 kPermissionAll        = 0;
inline constexpr i32 kPermissionGameMaster = 2;
inline constexpr i32 kPermissionAdmin      = 3;
inline constexpr i32 kPermissionOwner      = 4;

struct CommandSource {
    enum class Kind : u8 { Console, Player };

    Kind        kind{Kind::Console};
    i32         entity_id{-1};
    std::string name{"Server"};
    net::Uuid   uuid{};
    Vec3d       position{};
    f32         yaw{0.0F};
    f32         pitch{0.0F};
    f32         eye_height{1.62F};
    i32         permission{kPermissionOwner};

    [[nodiscard]] bool is_player() const noexcept { return kind == Kind::Player; }
    [[nodiscard]] bool has_permission(i32 level) const noexcept { return permission >= level; }
};

/// One entity, as a selector and a command read it.
struct EntityInfo {
    i32         id{0};
    bool        player{false};
    /// "minecraft:cow". "minecraft:player" for a player.
    std::string type;
    net::Uuid   uuid{};
    /// A player's name. Mobs carry no custom names on this server, so theirs
    /// is empty and `name=` never matches one — stated, not guessed.
    std::string name;
    Vec3d       position{};
    f32         yaw{0.0F};
    f32         pitch{0.0F};
    f32         width{0.6F};
    f32         height{1.8F};
    f32         eye_height{1.62F};
    /// Players only: 0 survival, 1 creative, 2 adventure, 3 spectator.
    u8  game_mode{0};
    i32 experience_level{0};
    /// Items only: what the stack is, for its display name.
    std::string item;
};

/// One coordinate of `~1 ^ 5`: whether it is relative, and its number.
struct Coordinate {
    bool relative{false};
    f64  value{0.0};
};

/// Three world (`~`) or local (`^`) coordinates.
struct Coordinates {
    bool                      local{false};
    std::array<Coordinate, 3> axes{};

    /// Where they point for this source. Local coordinates turn with the
    /// source's rotation: left, up and forwards along its look.
    [[nodiscard]] Vec3d position(const CommandSource& source) const;

    /// The block they name: the position, floored.
    [[nodiscard]] BlockPos block(const CommandSource& source) const;

    [[nodiscard]] bool any_relative() const noexcept {
        return local || axes[0].relative || axes[1].relative || axes[2].relative;
    }
};

/// `yaw pitch` with `~` allowed on either.
struct RotationArg {
    std::array<Coordinate, 2> axes{};  // [0] yaw, [1] pitch
};

/// One angle, `~` allowed.
struct AngleArg {
    bool relative{false};
    f32  value{0.0F};

    [[nodiscard]] f32 resolve(const CommandSource& source) const noexcept;
};

/// Mth.wrapDegrees for a float.
[[nodiscard]] f32 wrap_degrees(f32 degrees) noexcept;

}  // namespace ov::server::cmd
