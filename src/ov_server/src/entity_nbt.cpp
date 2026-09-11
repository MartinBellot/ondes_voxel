// ── persistence ── See entity_nbt.hpp.
#include "entity_nbt.hpp"

#include "ov/nbt/binary.hpp"

#include <string>
#include <utility>
#include <vector>

namespace ov::server {
namespace {

[[nodiscard]] nbt::Tag doubles(f64 a, f64 b, f64 c) {
    nbt::Tag list = nbt::Tag::make_list(nbt::TagType::Double);
    (void)list.push(nbt::Tag{a});
    (void)list.push(nbt::Tag{b});
    (void)list.push(nbt::Tag{c});
    return list;
}

}  // namespace

void default_to(nbt::Tag& compound, std::string_view name, nbt::Tag value) {
    if (!compound.contains(name)) {
        (void)compound.put(std::string{name}, std::move(value));
    }
}

void put_entity_base(nbt::Tag& out, std::string_view id, Vec3d position, Vec3d motion, f32 yaw,
                     f32 pitch, const net::Uuid& uuid, bool on_ground, i16 fire) {
    if (out.compound() == nullptr) {
        out = nbt::Tag::make_compound();
    }
    (void)out.put("id", nbt::Tag{std::string{id}});
    (void)out.put("Pos", doubles(position.x, position.y, position.z));
    (void)out.put("Motion", doubles(motion.x, motion.y, motion.z));
    nbt::Tag rotation = nbt::Tag::make_list(nbt::TagType::Float);
    (void)rotation.push(nbt::Tag{yaw});
    (void)rotation.push(nbt::Tag{pitch});
    (void)out.put("Rotation", std::move(rotation));
    (void)out.put("UUID", uuid_tag(uuid));
    (void)out.put("OnGround", nbt::Tag::make_bool(on_ground));
    default_to(out, "FallDistance", nbt::Tag{0.0F});
    default_to(out, "Fire", nbt::Tag{fire});
    default_to(out, "Air", nbt::Tag{i16{300}});
    default_to(out, "Invulnerable", nbt::Tag::make_bool(false));
    default_to(out, "PortalCooldown", nbt::Tag{i32{0}});
}

nbt::Tag uuid_tag(const net::Uuid& uuid) {
    return nbt::Tag{nbt::Tag::IntArray{
        static_cast<i32>(static_cast<u32>(uuid.most_significant >> 32U)),
        static_cast<i32>(static_cast<u32>(uuid.most_significant & 0xFFFFFFFFU)),
        static_cast<i32>(static_cast<u32>(uuid.least_significant >> 32U)),
        static_cast<i32>(static_cast<u32>(uuid.least_significant & 0xFFFFFFFFU))}};
}

std::optional<net::Uuid> uuid_from(const nbt::Tag* tag) {
    if (tag == nullptr) {
        return std::nullopt;
    }
    const auto* ints = tag->get_if<nbt::Tag::IntArray>();
    if (ints == nullptr || ints->size() != 4) {
        return std::nullopt;
    }
    const auto word = [&](usize i) { return static_cast<u64>(static_cast<u32>((*ints)[i])); };
    return net::Uuid{(word(0) << 32U) | word(1), (word(2) << 32U) | word(3)};
}

f64 list_f64(const nbt::Tag& compound, std::string_view name, usize index, f64 fallback) {
    const nbt::Tag* list = compound.find(name);
    if (list == nullptr || list->list() == nullptr || index >= list->list()->size()) {
        return fallback;
    }
    return (*list->list())[index].as_f64(fallback);
}

Vec3d list_vec3(const nbt::Tag& compound, std::string_view name) {
    return Vec3d{list_f64(compound, name, 0), list_f64(compound, name, 1),
                 list_f64(compound, name, 2)};
}

i64 get_i64(const nbt::Tag& compound, std::string_view name, i64 fallback) {
    const nbt::Tag* value = compound.find(name);
    return value != nullptr ? value->as_i64(fallback) : fallback;
}

f64 get_f64(const nbt::Tag& compound, std::string_view name, f64 fallback) {
    const nbt::Tag* value = compound.find(name);
    return value != nullptr ? value->as_f64(fallback) : fallback;
}

bool get_bool(const nbt::Tag& compound, std::string_view name, bool fallback) {
    const nbt::Tag* value = compound.find(name);
    return value != nullptr ? value->as_bool(fallback) : fallback;
}

std::optional<nbt::Tag> item_stack_tag(const registry::Registries& registries,
                                       registry::RegistryId items, const net::ItemStack& stack) {
    if (stack.empty() || stack.item_id <= 0) {
        return std::nullopt;
    }
    const std::string_view name = registries.entry_of(items, stack.item_id);
    if (name.empty()) {
        return std::nullopt;
    }
    nbt::Tag out = nbt::Tag::make_compound();
    (void)out.put("id", nbt::Tag{std::string{name}});
    (void)out.put("Count", nbt::Tag{stack.count});
    if (!stack.nbt.empty()) {
        if (auto document = nbt::read(stack.nbt); document && document->root.compound() != nullptr) {
            (void)out.put("tag", std::move(document->root));
        }
    }
    return out;
}

std::optional<net::ItemStack> item_stack_from(const registry::Registries& registries,
                                              registry::RegistryId items, const nbt::Tag* compound) {
    if (compound == nullptr) {
        return std::nullopt;
    }
    const nbt::Tag* id    = compound->find("id");
    const i64       count = get_i64(*compound, "Count", 0);
    if (id == nullptr || count <= 0) {
        return std::nullopt;
    }
    const auto item = registries.protocol_id(items, id->as_string());
    if (!item || *item == 0) {
        return std::nullopt;
    }
    net::ItemStack stack{static_cast<i32>(*item), static_cast<i8>(std::min<i64>(count, 127)), {}};
    // Kept as the wire wants it: a named document, as in player_data.cpp.
    if (const nbt::Tag* tag = compound->find("tag"); tag != nullptr && tag->compound() != nullptr) {
        stack.nbt = nbt::write(nbt::Document{"tag", *tag});
    }
    return stack;
}

nbt::Tag block_state_tag(const registry::BlockRegistry& blocks, registry::BlockStateId state) {
    const registry::BlockId block = blocks.block_of(state);
    nbt::Tag                out   = nbt::Tag::make_compound();
    const auto              properties = blocks.properties(block);
    if (!properties.empty()) {
        nbt::Tag props = nbt::Tag::make_compound();
        for (const registry::PropertyView& property : properties) {
            (void)props.put(std::string{property.name},
                            nbt::Tag{std::string{blocks.property_value(state, property)}});
        }
        (void)out.put("Properties", std::move(props));
    }
    (void)out.put("Name", nbt::Tag{std::string{blocks.block_name(block)}});
    return out;
}

std::optional<registry::BlockStateId> block_state_from(const registry::BlockRegistry& blocks,
                                                       const nbt::Tag*                compound) {
    if (compound == nullptr) {
        return std::nullopt;
    }
    const nbt::Tag* name  = compound->find("Name");
    const auto      block = name != nullptr ? blocks.find_block(name->as_string()) : std::nullopt;
    if (!block) {
        return std::nullopt;
    }
    registry::BlockStateId state = blocks.default_state(*block);
    const nbt::Tag*        props = compound->find("Properties");
    if (props == nullptr || props->compound() == nullptr) {
        return state;
    }
    // One property at a time from the default: a partial set handed to a
    // resolver has been seen to pick another state (piège 8).
    for (const nbt::CompoundEntry& entry : *props->compound()) {
        const auto property = blocks.find_property(*block, entry.name);
        if (!property) {
            continue;
        }
        const std::string_view wanted = entry.value.as_string();
        for (usize i = 0; i < property->values.size(); ++i) {
            if (property->values[i] == wanted) {
                state = blocks.with_property(state, *property, static_cast<u16>(i));
                break;
            }
        }
    }
    return state;
}

}  // namespace ov::server
