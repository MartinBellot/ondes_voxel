// The registries, as an argument parser needs them.
//
// Names resolve to Mojang's ids through the packs the server already loads —
// never through a table written here — and the lists a suggestion walks are
// built once, when the dispatcher is.
#pragma once

#include "ov/base/types.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server::cmd {

class ParseEnv {
public:
    ParseEnv() = default;
    ParseEnv(const registry::BlockRegistry* blocks, const registry::Registries* registries);

    [[nodiscard]] const registry::BlockRegistry* blocks() const noexcept { return blocks_; }
    [[nodiscard]] const registry::Registries*    registries() const noexcept { return registries_; }

    /// "stone" and "minecraft:stone" alike; nullopt for anything else.
    [[nodiscard]] std::optional<registry::BlockId> find_block(std::string_view id) const;
    [[nodiscard]] std::optional<i32>               find_item(std::string_view id) const;
    [[nodiscard]] bool                             has_entity_type(std::string_view id) const;
    [[nodiscard]] std::optional<i32>               entity_type_id(std::string_view id) const;

    [[nodiscard]] std::string_view item_name(i32 item) const;

    [[nodiscard]] bool has_block_tag(std::string_view tag) const;
    [[nodiscard]] bool has_item_tag(std::string_view tag) const;
    [[nodiscard]] bool has_entity_tag(std::string_view tag) const;
    [[nodiscard]] bool block_in_tag(std::string_view tag, registry::BlockId block) const;
    [[nodiscard]] bool item_in_tag(std::string_view tag, i32 item) const;
    [[nodiscard]] bool entity_in_tag(std::string_view tag, std::string_view type) const;

    /// Every entry of a registry by name, e.g. "minecraft:mob_effect".
    [[nodiscard]] std::vector<std::string> registry_ids(std::string_view registry) const;

    /// Mojang's id for a parser, from `minecraft:command_argument_type`.
    [[nodiscard]] std::optional<i32> parser_id(std::string_view name) const;

    // Suggestion lists, full ids, in registry order.
    [[nodiscard]] const std::vector<std::string>& block_ids() const noexcept { return block_ids_; }
    [[nodiscard]] const std::vector<std::string>& item_ids() const noexcept { return item_ids_; }
    [[nodiscard]] const std::vector<std::string>& entity_ids() const noexcept { return entity_ids_; }
    [[nodiscard]] const std::vector<std::string>& block_tags() const noexcept { return block_tags_; }
    [[nodiscard]] const std::vector<std::string>& item_tags() const noexcept { return item_tags_; }
    [[nodiscard]] const std::vector<std::string>& entity_tags() const noexcept { return entity_tags_; }

private:
    const registry::BlockRegistry* blocks_{nullptr};
    const registry::Registries*    registries_{nullptr};
    std::optional<registry::RegistryId> item_registry_;
    std::optional<registry::RegistryId> entity_registry_;
    std::optional<registry::RegistryId> block_registry_;
    std::optional<registry::RegistryId> argument_registry_;

    std::vector<std::string> block_ids_;
    std::vector<std::string> item_ids_;
    std::vector<std::string> entity_ids_;
    std::vector<std::string> block_tags_;
    std::vector<std::string> item_tags_;
    std::vector<std::string> entity_tags_;
};

/// "stone" → "minecraft:stone"; anything with a colon unchanged.
[[nodiscard]] std::string full_id(std::string_view id);

}  // namespace ov::server::cmd
