#include "canvas.hpp"

#include "ov/nbt/tag.hpp"

#include <algorithm>
#include <fmt/format.h>

namespace ov::lab {
namespace {

/// Floor division, because a plot at x = -1 lives in chunk -1 and not chunk 0.
[[nodiscard]] constexpr i32 chunk_of(i32 block) noexcept { return block >> 4; }

[[nodiscard]] constexpr usize local_of(i32 block) noexcept {
    return static_cast<usize>(block & 15);
}

/// A sign line is a JSON text component, not a bare string. `{"text":""}` is
/// the empty line; an actually empty string is rejected by the client.
[[nodiscard]] std::string as_component(std::string_view line) {
    std::string escaped;
    escaped.reserve(line.size() + 2);
    for (const char character : line) {
        if (character == '"' || character == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return fmt::format(R"({{"text":"{}"}})", escaped);
}

}  // namespace

Canvas::Canvas(const registry::BlockRegistry& blocks, const registry::Registries* registries)
    : blocks_(&blocks), registries_(registries), air_(world::AirStates::from(blocks)) {
    if (registries_ != nullptr) {
        if (const auto registry = registries_->find("minecraft:block_entity_type")) {
            if (const auto id = registries_->protocol_id(*registry, "minecraft:sign")) {
                sign_type_id_ = static_cast<i32>(*id);
            }
        }
    }
}

registry::BlockStateId Canvas::state(std::string_view name, const Props& props) const {
    const auto block = blocks_->find_block(name);
    if (!block) {
        const std::string missing{name};
        if (std::ranges::find(unknown_, missing) == unknown_.end()) {
            unknown_.push_back(missing);
        }
        return air_.air;
    }
    if (props.empty()) {
        return blocks_->default_state(*block);
    }
    const auto found = blocks_->state_for(*block, props);
    if (!found) {
        std::string described{name};
        described += '[';
        for (const auto& [key, value] : props) {
            described += fmt::format("{}={},", key, value);
        }
        described += ']';
        if (std::ranges::find(unknown_, described) == unknown_.end()) {
            unknown_.push_back(described);
        }
        return blocks_->default_state(*block);
    }
    return *found;
}

world::Chunk& Canvas::chunk(i32 chunk_x, i32 chunk_z) {
    const std::pair key{chunk_x, chunk_z};
    if (const auto it = chunks_.find(key); it != chunks_.end()) {
        return it->second;
    }
    world::Chunk fresh{ChunkPos{chunk_x, chunk_z}, world::WorldShape::overworld(), air_, blocks_};
    fresh.fill_biome(0);
    return chunks_.emplace(key, std::move(fresh)).first->second;
}

void Canvas::set(i32 x, i32 y, i32 z, registry::BlockStateId state) {
    if (!world::WorldShape::overworld().contains_y(y)) {
        return;
    }
    chunk(chunk_of(x), chunk_of(z)).set_block(local_of(x), y, local_of(z), state);
    ++written_;
}

void Canvas::set(i32 x, i32 y, i32 z, std::string_view name, const Props& props) {
    set(x, y, z, state(name, props));
}

void Canvas::fill(i32 x0, i32 y0, i32 z0, i32 x1, i32 y1, i32 z1, std::string_view name,
                  const Props& props) {
    const auto resolved = state(name, props);
    for (i32 y = std::min(y0, y1); y <= std::max(y0, y1); ++y) {
        for (i32 z = std::min(z0, z1); z <= std::max(z0, z1); ++z) {
            for (i32 x = std::min(x0, x1); x <= std::max(x0, x1); ++x) {
                set(x, y, z, resolved);
            }
        }
    }
}

void Canvas::shell(i32 x0, i32 y0, i32 z0, i32 x1, i32 y1, i32 z1, std::string_view name,
                   const Props& props) {
    const auto resolved = state(name, props);
    const i32  min_x = std::min(x0, x1), max_x = std::max(x0, x1);
    const i32  min_y = std::min(y0, y1), max_y = std::max(y0, y1);
    const i32  min_z = std::min(z0, z1), max_z = std::max(z0, z1);
    for (i32 y = min_y; y <= max_y; ++y) {
        for (i32 z = min_z; z <= max_z; ++z) {
            for (i32 x = min_x; x <= max_x; ++x) {
                const bool on_face = x == min_x || x == max_x || y == min_y || y == max_y ||
                                     z == min_z || z == max_z;
                if (on_face) {
                    set(x, y, z, resolved);
                }
            }
        }
    }
}

void Canvas::sign(i32 x, i32 y, i32 z, std::span<const std::string> lines, i32 rotation) {
    // The rotation string has to outlive the call: Props holds string_views,
    // and a std::to_string temporary dies at the end of the initialiser list.
    const std::string rotation_text = std::to_string(rotation);
    set(x, y, z, "minecraft:oak_sign", {{"rotation", rotation_text}});

    auto messages = nbt::Tag::make_list(nbt::TagType::String);
    for (usize line = 0; line < 4; ++line) {
        messages.list()->emplace_back(
            as_component(line < lines.size() ? std::string_view{lines[line]} : std::string_view{}));
    }

    auto front = nbt::Tag::make_compound();
    front.compound()->push_back({"messages", std::move(messages)});
    front.compound()->push_back({"color", std::string{"black"}});
    front.compound()->push_back({"has_glowing_text", nbt::Tag::make_bool(false)});

    auto back_messages = nbt::Tag::make_list(nbt::TagType::String);
    for (usize line = 0; line < 4; ++line) {
        back_messages.list()->emplace_back(as_component({}));
    }
    auto back = nbt::Tag::make_compound();
    back.compound()->push_back({"messages", std::move(back_messages)});
    back.compound()->push_back({"color", std::string{"black"}});
    back.compound()->push_back({"has_glowing_text", nbt::Tag::make_bool(false)});

    auto data = nbt::Tag::make_compound();
    data.compound()->push_back({"front_text", std::move(front)});
    data.compound()->push_back({"back_text", std::move(back)});
    data.compound()->push_back({"is_waxed", nbt::Tag::make_bool(false)});

    world::BlockEntity entity;
    entity.x       = static_cast<u8>(local_of(x));
    entity.y       = y;
    entity.z       = static_cast<u8>(local_of(z));
    entity.type    = "minecraft:sign";
    entity.type_id = sign_type_id_;
    entity.data    = std::move(data);
    chunk(chunk_of(x), chunk_of(z)).set_block_entity(std::move(entity));
}

void Canvas::lay_ground(i32 chunk_x0, i32 chunk_z0, i32 chunk_x1, i32 chunk_z1) {
    const auto bedrock = state("minecraft:bedrock");
    const auto dirt    = state("minecraft:dirt");
    const auto grass   = state("minecraft:grass_block");
    for (i32 chunk_z = chunk_z0; chunk_z <= chunk_z1; ++chunk_z) {
        for (i32 chunk_x = chunk_x0; chunk_x <= chunk_x1; ++chunk_x) {
            world::Chunk& target = chunk(chunk_x, chunk_z);
            for (usize z = 0; z < 16; ++z) {
                for (usize x = 0; x < 16; ++x) {
                    target.set_block(x, -64, z, bedrock);
                    target.set_block(x, -63, z, dirt);
                    target.set_block(x, -62, z, dirt);
                    target.set_block(x, kGroundY, z, grass);
                }
            }
            written_ += 16 * 16 * 4;
        }
    }
}

}  // namespace ov::lab
