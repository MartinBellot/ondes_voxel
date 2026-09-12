// The packets a player uses to *do* something: hit, use, swing, sneak.
//
// Placing a block already had a packet here — Use Item On, in play.hpp, which
// was written when placing was the only thing the server did with it. It is the
// same packet the game uses to open a door, till dirt, empty a bucket and light
// a TNT block, and the four of them differ only in what the server decides to
// run. So the packet stays where it is and what joins it here is the rest of
// the family: Interact, Use Item, Swing Arm, Player Command.
//
// Packet ids are protocol 763's own. They were checked the way this repo checks
// every id — by effect, on a real 1.20.1 server: an Interact sent with a wrong
// id removes no health, and a campaign that reports a diamond sword doing no
// damage has found a wrong id rather than a wrong formula.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/play.hpp"

#include <optional>
#include <span>
#include <vector>

namespace ov::net {

namespace serverbound {
/// Interact With Entity: attack it, use an item on it, or use an item at a
/// point on it.
inline constexpr i32 kInteract = 0x10;
// Player Command — sneak and sprint — is already here: it arrived with hunger,
// which needs to know about sprinting, and lives in survival.hpp. It is not
// repeated.
/// Swing Arm. Purely the animation — the *hit* is Interact, and a server that
/// took this one for an attack would let a client hit fifty times a second.
inline constexpr i32 kSwingArm = 0x2F;
/// Use Item: the item acts on nothing in particular. Eating, drawing a bow,
/// throwing a pearl, filling a bucket from the water the player is looking at.
inline constexpr i32 kUseItem = 0x32;
}  // namespace serverbound

namespace clientbound {
/// Set Cooldown — the grey sweep over an item's icon.
///
/// 0x15. It was 0x16 until scripts/protocol_matrix.py checked every id constant
/// against data/protocol/763.json: 0x16 is Chat Suggestions, so a vanilla
/// client handed a pearl's cooldown read two varints as a suggestions action.
inline constexpr i32 kSetCooldown = 0x15;
/// Block Action: the chest lid, the note block's twang, the piston's push.
inline constexpr i32 kBlockAction = 0x09;
}  // namespace clientbound

/// The three things Interact can mean.
///
/// Ordered as the protocol orders them, and named rather than numbered: 0 and 1
/// differ by one and by everything, and a mix-up makes right-click kill things.
enum class InteractKind : u8 { Interact = 0, Attack = 1, InteractAt = 2 };

/// Which hand acted.
enum class Hand : u8 { Main = 0, Off = 1 };

/// One Interact packet.
struct Interact {
    i32          entity_id{0};
    InteractKind kind{InteractKind::Interact};

    /// Where on the target the player clicked, relative to the target's feet.
    /// Only carried by `InteractAt`, which is how the game knows which part of
    /// an armour stand was touched.
    f32 target_x{0.0F};
    f32 target_y{0.0F};
    f32 target_z{0.0F};

    /// Absent for a plain attack, which uses no hand.
    std::optional<Hand> hand;

    /// The client's own sneak flag, sent with every interaction.
    ///
    /// Not decoration: sneaking is what decides whether right-clicking a chest
    /// with a block in hand opens the chest or places the block, so a server
    /// that drops this field gets that rule backwards for every sneaking
    /// player.
    bool sneaking{false};
};

[[nodiscard]] std::optional<Interact> parse_interact(std::span<const u8> payload);

/// Use Item — the hand acts with nothing under the cursor.
struct UseItem {
    Hand hand{Hand::Main};
    i32  sequence{0};
};

[[nodiscard]] std::optional<UseItem> parse_use_item(std::span<const u8> payload);

/// Swing Arm. The hand is the whole packet.
[[nodiscard]] std::optional<Hand> parse_swing_arm(std::span<const u8> payload);

/// Grey out an item for a while. `item_id` is the item's **wire** id.
[[nodiscard]] std::vector<u8> encode_set_cooldown(i32 item_id, i32 ticks);

/// Block Action: two bytes whose meaning is the block's own, plus the block
/// type so the client knows how to read them.
///
/// `block_type` is the block's id in minecraft:block — not a state id. A chest
/// opening is (1, 1); a chest closing is (1, 0).
[[nodiscard]] std::vector<u8> encode_block_action(WirePosition position, u8 action_id,
                                                  u8 action_parameter, i32 block_type);

}  // namespace ov::net
