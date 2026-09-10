#include "names.hpp"

#include "json.hpp"
#include "snbt.hpp"

namespace ov::server::cmd {

Text player_display_name(std::string_view name, const net::Uuid& uuid) {
    Text out = Text::literal(std::string{name});
    out.style.insertion = std::string{name};
    out.style.click     = ClickEvent{"suggest_command", "/tell " + std::string{name} + " "};
    HoverEvent hover;
    hover.action      = HoverEvent::Action::ShowEntity;
    hover.entity_type = "minecraft:player";
    hover.id          = uuid.to_string();
    hover.text.push_back(Text::literal(std::string{name}));
    out.style.hover.push_back(std::move(hover));
    return out;
}

std::string entity_translation_key(std::string_view type) {
    const auto colon = type.find(':');
    if (colon == std::string_view::npos) {
        return "entity.minecraft." + std::string{type};
    }
    return "entity." + std::string{type.substr(0, colon)} + "." + std::string{type.substr(colon + 1)};
}

Text entity_display_name(const EntityInfo& entity, const ParseEnv& env, const Lang* lang) {
    if (entity.player) {
        return player_display_name(entity.name, entity.uuid);
    }
    Text name = entity.type == "minecraft:item" && !entity.item.empty()
                    ? item_name(entity.item, nullptr, env, lang)
                    : Text::translatable(entity_translation_key(entity.type));
    Text out = name;
    out.style.insertion = entity.uuid.to_string();
    HoverEvent hover;
    hover.action      = HoverEvent::Action::ShowEntity;
    hover.entity_type = entity.type;
    hover.id          = entity.uuid.to_string();
    hover.text.push_back(std::move(name));
    out.style.hover.push_back(std::move(hover));
    return out;
}

Text item_name(std::string_view id, const nbt::Tag* tag, const ParseEnv& env, const Lang* lang) {
    if (tag != nullptr) {
        if (const nbt::Tag* display = tag->find("display")) {
            if (const nbt::Tag* custom = display->find("Name")) {
                usize consumed = 0;
                if (const auto json = parse_json(custom->as_string(), consumed)) {
                    if (auto text = text_from_json(*json)) {
                        return std::move(*text);
                    }
                }
            }
        }
    }
    const auto        colon      = id.find(':');
    const std::string name_space = colon == std::string_view::npos ? "minecraft"
                                                                   : std::string{id.substr(0, colon)};
    const std::string path = colon == std::string_view::npos ? std::string{id}
                                                             : std::string{id.substr(colon + 1)};
    const std::string item_key  = "item." + name_space + "." + path;
    const std::string block_key = "block." + name_space + "." + path;
    if (lang != nullptr) {
        if (lang->has(item_key)) {
            return Text::translatable(item_key);
        }
        if (lang->has(block_key)) {
            return Text::translatable(block_key);
        }
    }
    return Text::translatable(env.find_block(id) ? block_key : item_key);
}

Text item_display(std::string_view id, i32 count, const nbt::Tag* tag, const ParseEnv& env,
                  const Lang* lang) {
    Text inner = Text::literal("");
    const bool custom = tag != nullptr && tag->find("display") != nullptr &&
                        tag->find("display")->find("Name") != nullptr;
    inner.append(item_name(id, tag, env, lang));
    if (custom) {
        inner.style.italic = true;
    }
    Text out = Text::translatable("chat.square_brackets", {std::move(inner)});
    // Rarity decides the colour, and rarity is Java code this server has no
    // table of: every item is shown as common (white). Named in
    // docs/provenance/commandes.md.
    out.color("white");
    HoverEvent hover;
    hover.action = HoverEvent::Action::ShowItem;
    hover.id     = std::string{id};
    hover.count  = count;
    if (tag != nullptr && !tag->empty()) {
        hover.tag = to_snbt(*tag);
    }
    out.style.hover.push_back(std::move(hover));
    return out;
}

Text copy_on_click(std::string text) {
    Text inner = Text::literal(text);
    inner.color("green");
    inner.style.insertion = text;
    inner.style.click     = ClickEvent{"copy_to_clipboard", text};
    HoverEvent hover;
    hover.action = HoverEvent::Action::ShowText;
    hover.text.push_back(Text::translatable("chat.copy.click"));
    inner.style.hover.push_back(std::move(hover));
    return Text::translatable("chat.square_brackets", {std::move(inner)});
}

}  // namespace ov::server::cmd
