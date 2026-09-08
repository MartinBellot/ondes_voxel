#include "ov/render/block_state_model.hpp"

#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace ov::render {

namespace {

[[nodiscard]] std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    usize                    start = 0;
    while (start <= text.size()) {
        const auto end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

/// `when` values are strings in every vanilla file, but the format allows the
/// bare JSON literals and packs use them. Writing them back out as text keeps
/// the comparison one code path instead of three.
[[nodiscard]] std::string value_text(const json::Value& value) {
    if (value.is_string()) {
        return std::string(value.as_string());
    }
    if (value.is_bool()) {
        return value.as_bool() ? "true" : "false";
    }
    if (value.is_number()) {
        char       buffer[32];
        const auto number = static_cast<i64>(value.as_number());
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
        return std::string(buffer, result.ptr);
    }
    return {};
}

[[nodiscard]] std::optional<ModelVariant> parse_variant(const json::Value& value) {
    auto model = ResourceLocation::parse(value["model"].as_string());
    if (!model) {
        return std::nullopt;
    }
    return ModelVariant{
        .model  = std::move(*model),
        .x      = static_cast<i32>(value["x"].as_number(0.0)),
        .y      = static_cast<i32>(value["y"].as_number(0.0)),
        .uvlock = value["uvlock"].as_bool(false),
        .weight = static_cast<i32>(value["weight"].as_number(1.0)),
    };
}

/// `apply` and a variant value are both "one object, or an array of them".
[[nodiscard]] VariantGroup parse_group(const json::Value& value) {
    VariantGroup group;
    if (value.is_array()) {
        group.alternatives.reserve(value.size());
        for (u32 i = 0; i < value.size(); ++i) {
            if (auto variant = parse_variant(value[i])) {
                group.alternatives.push_back(std::move(*variant));
            }
        }
    } else if (value.is_object()) {
        if (auto variant = parse_variant(value)) {
            group.alternatives.push_back(std::move(*variant));
        }
    }
    return group;
}

/// "facing=east,half=bottom" -> two requirements. The empty key yields none,
/// and so matches every state.
[[nodiscard]] std::vector<PropertyTest> parse_variant_key(std::string_view key) {
    std::vector<PropertyTest> tests;
    if (key.empty()) {
        return tests;
    }
    for (auto& clause : split(key, ',')) {
        const auto equals = clause.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        PropertyTest test;
        test.name = clause.substr(0, equals);
        test.values.emplace_back(clause.substr(equals + 1));
        tests.push_back(std::move(test));
    }
    return tests;
}

/// One conjunction: every listed property must hold. `north=side|up` is one
/// test with two accepted values, not two tests.
[[nodiscard]] std::vector<PropertyTest> parse_conjunction(const json::Value& value) {
    std::vector<PropertyTest> tests;
    for (u32 i = 0; i < value.size(); ++i) {
        PropertyTest test;
        test.name   = std::string(value.key_at(i));
        test.values = split(value_text(value.value_at(i)), '|');
        tests.push_back(std::move(test));
    }
    return tests;
}

[[nodiscard]] MultipartCondition parse_condition(const json::Value& value) {
    MultipartCondition condition;
    if (!value.is_object()) {
        return condition;
    }

    if (const auto disjunction = value["OR"]; disjunction.is_array()) {
        for (u32 i = 0; i < disjunction.size(); ++i) {
            condition.any_of.push_back(parse_conjunction(disjunction[i]));
        }
        return condition;
    }

    // AND is not used anywhere in the 1.20.1 assets, but the format has it and
    // a pack may. Flattening it into one conjunction is exactly its meaning.
    if (const auto conjunction = value["AND"]; conjunction.is_array()) {
        std::vector<PropertyTest> tests;
        for (u32 i = 0; i < conjunction.size(); ++i) {
            auto part = parse_conjunction(conjunction[i]);
            tests.insert(tests.end(), std::make_move_iterator(part.begin()),
                         std::make_move_iterator(part.end()));
        }
        condition.any_of.push_back(std::move(tests));
        return condition;
    }

    condition.any_of.push_back(parse_conjunction(value));
    return condition;
}

[[nodiscard]] bool tests_match(const std::vector<PropertyTest>& tests, PropertyList properties) {
    for (const auto& test : tests) {
        const auto property = std::ranges::find_if(
            properties, [&test](const auto& pair) { return pair.first == test.name; });
        // A condition on a property the block does not have can never hold.
        // Silently treating it as satisfied would draw fence sides on a wall.
        if (property == properties.end()) {
            return false;
        }
        if (std::ranges::find(test.values, property->second) == test.values.end()) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool condition_matches(const MultipartCondition& condition, PropertyList properties) {
    if (condition.always()) {
        return true;
    }
    return std::ranges::any_of(condition.any_of, [properties](const auto& tests) {
        return tests_match(tests, properties);
    });
}

std::expected<BlockStateFile, ModelError> BlockStateFile::parse(std::span<const u8> bytes) {
    auto document = json::Document::parse(bytes);
    if (!document) {
        return std::unexpected(ModelError::Malformed);
    }

    const auto root = document->root();
    if (!root.is_object()) {
        return std::unexpected(ModelError::Malformed);
    }

    BlockStateFile file;

    if (const auto variants = root["variants"]; variants.is_object()) {
        file.variants_.reserve(variants.size());
        for (u32 i = 0; i < variants.size(); ++i) {
            VariantCase entry;
            entry.requirements = parse_variant_key(variants.key_at(i));
            entry.group        = parse_group(variants.value_at(i));
            file.variants_.push_back(std::move(entry));
        }
    }

    if (const auto multipart = root["multipart"]; multipart.is_array()) {
        file.multipart_.reserve(multipart.size());
        for (u32 i = 0; i < multipart.size(); ++i) {
            const auto    entry = multipart[i];
            MultipartCase result;
            result.condition = parse_condition(entry["when"]);
            result.group     = parse_group(entry["apply"]);
            file.multipart_.push_back(std::move(result));
        }
    }

    if (file.variants_.empty() && file.multipart_.empty()) {
        return std::unexpected(ModelError::Malformed);
    }

    return file;
}

std::vector<VariantGroup> BlockStateFile::select(PropertyList properties) const {
    std::vector<VariantGroup> groups;

    if (is_multipart()) {
        for (const auto& entry : multipart_) {
            if (condition_matches(entry.condition, properties)) {
                groups.push_back(entry.group);
            }
        }
        return groups;
    }

    // Variants are exclusive: the first key all of whose requirements hold
    // wins. Vanilla writes every combination out, so exactly one matches; a
    // pack that leaves a gap draws nothing there rather than drawing twice.
    for (const auto& entry : variants_) {
        if (tests_match(entry.requirements, properties)) {
            groups.push_back(entry.group);
            break;
        }
    }
    return groups;
}

}  // namespace ov::render
