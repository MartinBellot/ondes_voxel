// The other direction: what a *client* has to read, and what it sends back.
//
// play.hpp and survival.hpp are written from the server's point of view —
// encoders for what it sends, parsers for what it receives. Every one of those
// packets has a mirror image nobody had needed until a client had to draw an
// interface: the hearts come from Set Health, the hotbar from Set Container
// Content, an open chest from Open Screen, and moving a stack means sending
// Click Container.
//
// They live in their own file rather than beside their opposites so that the
// two directions stay legible, and so that a round-trip test can encode with
// one and parse with the other. That test is the point: a parser written from
// a specification and never run against the encoder that will feed it is a
// parser that reads the right fields in the wrong order.
#pragma once

#include "ov/base/types.hpp"
#include "ov/protocol/play.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ov::net {

/// Set Health (0x57). What the hearts and the haunches draw.
struct HealthUpdate {
    /// 0 to 20, in half-hearts of one unit each. Zero is the death screen.
    f32 health{20.0F};
    /// 0 to 20 haunches.
    i32 food{20};
    /// Never drawn directly; it decides whether the hunger bar wobbles.
    f32 saturation{5.0F};
};

[[nodiscard]] std::optional<HealthUpdate> parse_set_health(std::span<const u8> payload);

/// Set Experience (0x56).
struct ExperienceUpdate {
    /// 0..1 *within the current level*, which is what the bar draws. A
    /// fraction of the lifetime total would leave it nearly empty forever.
    f32 bar{0.0F};
    i32 level{0};
    i32 total{0};
};

[[nodiscard]] std::optional<ExperienceUpdate> parse_set_experience(std::span<const u8> payload);

/// Set Container Content (0x12): every slot of one window at once.
struct ContainerContent {
    u8  window_id{0};
    i32 state_id{0};
    /// In window order. For window 0 that is: 0 crafting output, 1..4 the 2×2
    /// grid, 5..8 the armour, 9..35 the main inventory, 36..44 the hotbar,
    /// 45 the off hand.
    std::vector<ItemStack> slots;
    /// What the cursor is holding. Sent by the server, so it is authoritative.
    ItemStack carried;
};

[[nodiscard]] std::optional<ContainerContent> parse_container_content(std::span<const u8> payload);

/// Set Container Slot (0x14): one slot of one window.
///
/// ⚠️ `window_id` is **signed** here and unsigned in Set Container Content.
/// −1 is not a window: it means "this stack is now on the cursor", and −2
/// means "a slot of the player's inventory, whatever window is open". Reading
/// it as a u8 turns the cursor into window 255 and drops the update.
struct ContainerSlotUpdate {
    i8        window_id{0};
    i32       state_id{0};
    i16       slot{0};
    ItemStack stack;
};

[[nodiscard]] std::optional<ContainerSlotUpdate> parse_container_slot(std::span<const u8> payload);

/// Open Screen (0x30).
struct OpenScreen {
    i32 window_id{0};
    /// An index into the `minecraft:menu` registry: 2 is a 9×3 chest, 5 a 9×6
    /// one, 11 a crafting table, 14 a furnace. Different numbers, different
    /// window shapes over the same slot list.
    i32 type{0};
    /// The title, as the chat component JSON arrived. Left as JSON on purpose:
    /// turning a component into a line of text is the interface's job and it
    /// needs the language file to do it.
    std::string title_json;
};

[[nodiscard]] std::optional<OpenScreen> parse_open_screen(std::span<const u8> payload);

/// Close Container, clientbound (0x11). The server closes the window.
[[nodiscard]] std::optional<u8> parse_clientbound_close_container(std::span<const u8> payload);

/// One slot the client believes changed, as Click Container carries them.
struct ClickChange {
    i16       slot{0};
    ItemStack stack;
};

/// Click Container (0x0B), serverbound.
///
/// The `changed` array is the client saying what it *predicts*; the server is
/// free to ignore it and ours does. It is still sent, because vanilla sends it
/// and a server that requires it would otherwise refuse our clicks.
///
/// `mode` is what makes this six operations in one packet:
///   0 pick up (button 0 whole stack, 1 half) · 1 shift-click · 2 number key
///   3 middle-click clone · 4 drop (slot −999 throws the cursor)
///   5 drag, in three phases · 6 double-click to gather
[[nodiscard]] std::vector<u8> encode_container_click(u8 window_id, i32 state_id, i16 slot,
                                                     i8 button, i32 mode,
                                                     std::span<const ClickChange> changed,
                                                     const ItemStack&             carried);

/// Close Container (0x0C), serverbound.
[[nodiscard]] std::vector<u8> encode_serverbound_close_container(u8 window_id);

}  // namespace ov::net
