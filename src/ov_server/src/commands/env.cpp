#include "env.hpp"

namespace ov::server::cmd {

std::string full_id(std::string_view id) {
    if (id.find(':') != std::string_view::npos) {
        return std::string{id};
    }
    return "minecraft:" + std::string{id};
}

ParseEnv::ParseEnv(const registry::BlockRegistry* blocks, const registry::Registries* registries)
    : blocks_{blocks}, registries_{registries} {
    if (registries_ != nullptr) {
        item_registry_     = registries_->find("minecraft:item");
        entity_registry_   = registries_->find("minecraft:entity_type");
        block_registry_    = registries_->find("minecraft:block");
        argument_registry_ = registries_->find("minecraft:command_argument_type");
        if (item_registry_) {
            for (const std::string_view name : registries_->entries(*item_registry_)) {
                item_ids_.emplace_back(name);
            }
        }
        if (entity_registry_) {
            for (const std::string_view name : registries_->entries(*entity_registry_)) {
                entity_ids_.emplace_back(name);
            }
        }
        for (usize t = 0; t < registries_->tag_count(); ++t) {
            const registry::TagId tag{static_cast<u16>(t)};
            const std::string_view name = registries_->tag_name(tag);
            const auto owned_by = [&](const std::optional<registry::RegistryId>& reg) {
                if (!reg) {
                    return false;
                }
                const auto found = registries_->find_tag(*reg, name);
                return found && *found == tag;
            };
            if (owned_by(block_registry_)) {
                block_tags_.emplace_back(name);
            } else if (owned_by(item_registry_)) {
                item_tags_.emplace_back(name);
            } else if (owned_by(entity_registry_)) {
                entity_tags_.emplace_back(name);
            }
        }
    }
    if (blocks_ != nullptr) {
        for (usize b = 0; b < blocks_->block_count(); ++b) {
            block_ids_.emplace_back(blocks_->block_name(registry::BlockId{static_cast<u16>(b)}));
        }
    }
}

std::optional<registry::BlockId> ParseEnv::find_block(std::string_view id) const {
    if (blocks_ == nullptr) {
        return std::nullopt;
    }
    return blocks_->find_block(full_id(id));
}

std::optional<i32> ParseEnv::find_item(std::string_view id) const {
    if (registries_ == nullptr || !item_registry_) {
        return std::nullopt;
    }
    return registries_->protocol_id(*item_registry_, full_id(id));
}

std::optional<i32> ParseEnv::entity_type_id(std::string_view id) const {
    if (registries_ == nullptr || !entity_registry_) {
        return std::nullopt;
    }
    return registries_->protocol_id(*entity_registry_, full_id(id));
}

bool ParseEnv::has_entity_type(std::string_view id) const { return entity_type_id(id).has_value(); }

std::string_view ParseEnv::item_name(i32 item) const {
    if (registries_ == nullptr || !item_registry_) {
        return {};
    }
    return registries_->entry_of(*item_registry_, item);
}

namespace {

std::optional<registry::TagId> tag_in(const registry::Registries*                 registries,
                                      const std::optional<registry::RegistryId>& reg,
                                      std::string_view                           tag) {
    if (registries == nullptr || !reg) {
        return std::nullopt;
    }
    return registries->find_tag(*reg, full_id(tag));
}

}  // namespace

bool ParseEnv::has_block_tag(std::string_view tag) const {
    return tag_in(registries_, block_registry_, tag).has_value();
}
bool ParseEnv::has_item_tag(std::string_view tag) const {
    return tag_in(registries_, item_registry_, tag).has_value();
}
bool ParseEnv::has_entity_tag(std::string_view tag) const {
    return tag_in(registries_, entity_registry_, tag).has_value();
}

bool ParseEnv::block_in_tag(std::string_view tag, registry::BlockId block) const {
    const auto found = tag_in(registries_, block_registry_, tag);
    if (!found || blocks_ == nullptr) {
        return false;
    }
    // The tag holds `minecraft:block` wire ids; the block registry has its own
    // numbering. They are joined by name, which is the one thing both agree on.
    const auto wire = registries_->protocol_id(*block_registry_, blocks_->block_name(block));
    return wire && registries_->tag_contains(*found, *wire);
}

bool ParseEnv::item_in_tag(std::string_view tag, i32 item) const {
    const auto found = tag_in(registries_, item_registry_, tag);
    return found && registries_->tag_contains(*found, item);
}

bool ParseEnv::entity_in_tag(std::string_view tag, std::string_view type) const {
    const auto found = tag_in(registries_, entity_registry_, tag);
    const auto id    = entity_type_id(type);
    return found && id && registries_->tag_contains(*found, *id);
}

std::optional<i32> ParseEnv::parser_id(std::string_view name) const {
    if (registries_ == nullptr || !argument_registry_) {
        return std::nullopt;
    }
    return registries_->protocol_id(*argument_registry_, name);
}

}  // namespace ov::server::cmd
