#include "ov/protocol/client_play.hpp"

#include "ov/protocol/types.hpp"
#include "ov/protocol/varint.hpp"

namespace ov::net {

std::optional<HealthUpdate> parse_set_health(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     health     = reader.read_f32();
    const auto     food       = read_varint(reader);
    const auto     saturation = reader.read_f32();
    if (!health || !food || !saturation) {
        return std::nullopt;
    }
    return HealthUpdate{*health, *food, *saturation};
}

std::optional<ExperienceUpdate> parse_set_experience(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     bar   = reader.read_f32();
    const auto     level = read_varint(reader);
    const auto     total = read_varint(reader);
    if (!bar || !level || !total) {
        return std::nullopt;
    }
    return ExperienceUpdate{*bar, *level, *total};
}

std::optional<ContainerContent> parse_container_content(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     window_id = reader.read_u8();
    const auto     state_id  = read_varint(reader);
    const auto     count     = read_varint(reader);
    if (!window_id || !state_id || !count || *count < 0) {
        return std::nullopt;
    }

    ContainerContent content;
    content.window_id = *window_id;
    content.state_id  = *state_id;
    content.slots.reserve(static_cast<usize>(*count));
    for (i32 i = 0; i < *count; ++i) {
        auto slot = read_slot(reader);
        if (!slot) {
            return std::nullopt;
        }
        content.slots.push_back(std::move(*slot));
    }
    auto carried = read_slot(reader);
    if (!carried) {
        return std::nullopt;
    }
    content.carried = std::move(*carried);
    return content;
}

std::optional<ContainerSlotUpdate> parse_container_slot(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    // Signed, unlike Set Container Content's. −1 is the cursor and −2 is "the
    // player's inventory, whichever window is open".
    const auto window_id = reader.read_i8();
    const auto state_id  = read_varint(reader);
    const auto slot      = reader.read_i16();
    if (!window_id || !state_id || !slot) {
        return std::nullopt;
    }
    auto stack = read_slot(reader);
    if (!stack) {
        return std::nullopt;
    }
    return ContainerSlotUpdate{*window_id, *state_id, *slot, std::move(*stack)};
}

std::optional<OpenScreen> parse_open_screen(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     window_id = read_varint(reader);
    const auto     type      = read_varint(reader);
    if (!window_id || !type) {
        return std::nullopt;
    }
    auto title = read_string(reader);
    if (!title) {
        return std::nullopt;
    }
    return OpenScreen{*window_id, *type, std::move(*title)};
}

std::optional<u8> parse_clientbound_close_container(std::span<const u8> payload) {
    io::ByteReader reader{payload};
    const auto     window_id = reader.read_u8();
    if (!window_id) {
        return std::nullopt;
    }
    return *window_id;
}

std::vector<u8> encode_container_click(u8 window_id, i32 state_id, i16 slot, i8 button, i32 mode,
                                       std::span<const ClickChange> changed,
                                       const ItemStack&             carried) {
    io::ByteWriter writer;
    writer.write_u8(window_id);
    write_varint(writer, state_id);
    writer.write_i16(slot);
    writer.write_i8(button);
    write_varint(writer, mode);
    write_varint(writer, static_cast<i32>(changed.size()));
    for (const ClickChange& change : changed) {
        writer.write_i16(change.slot);
        write_slot(writer, change.stack);
    }
    write_slot(writer, carried);
    return writer.take();
}

std::vector<u8> encode_serverbound_close_container(u8 window_id) {
    io::ByteWriter writer;
    writer.write_u8(window_id);
    return writer.take();
}

}  // namespace ov::net
