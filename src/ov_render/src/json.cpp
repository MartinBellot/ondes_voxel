#include "json.hpp"

#include <simdjson.h>

#include <utility>

namespace ov::render::json {

std::string_view to_string(ParseError error) noexcept {
    switch (error) {
        case ParseError::Malformed: return "malformed JSON";
        case ParseError::TooDeep: return "JSON nested deeper than the limit";
        case ParseError::TooLarge: return "JSON document too large";
    }
    return "unknown JSON error";
}

Kind Value::kind() const noexcept {
    if (document_ == nullptr) {
        return Kind::Null;
    }
    return document_->nodes_[node_].kind;
}

u32 Value::size() const noexcept {
    if (document_ == nullptr) {
        return 0;
    }
    return document_->nodes_[node_].count;
}

Value Value::operator[](std::string_view key) const noexcept {
    if (!is_object()) {
        return {};
    }
    const auto& node = document_->nodes_[node_];
    for (u32 i = 0; i < node.count; ++i) {
        const u32              child      = document_->children_[node.first_child + i];
        const auto&            child_node = document_->nodes_[child];
        const std::string_view name(document_->text_.data() + child_node.key_offset,
                                    child_node.key_length);
        if (name == key) {
            return Value{document_, child};
        }
    }
    return {};
}

Value Value::operator[](u32 index) const noexcept {
    if (document_ == nullptr) {
        return {};
    }
    const auto& node = document_->nodes_[node_];
    if (node.kind != Kind::Array || index >= node.count) {
        return {};
    }
    return Value{document_, document_->children_[node.first_child + index]};
}

std::string_view Value::key_at(u32 index) const noexcept {
    if (!is_object()) {
        return {};
    }
    const auto& node = document_->nodes_[node_];
    if (index >= node.count) {
        return {};
    }
    const auto& child = document_->nodes_[document_->children_[node.first_child + index]];
    return std::string_view(document_->text_.data() + child.key_offset, child.key_length);
}

Value Value::value_at(u32 index) const noexcept {
    if (!is_object()) {
        return {};
    }
    const auto& node = document_->nodes_[node_];
    if (index >= node.count) {
        return {};
    }
    return Value{document_, document_->children_[node.first_child + index]};
}

std::string_view Value::as_string(std::string_view fallback) const noexcept {
    if (!is_string()) {
        return fallback;
    }
    const auto& node = document_->nodes_[node_];
    return std::string_view(document_->text_.data() + node.text_offset, node.text_length);
}

f64 Value::as_number(f64 fallback) const noexcept {
    return is_number() ? document_->nodes_[node_].number : fallback;
}

bool Value::as_bool(bool fallback) const noexcept {
    return is_bool() ? document_->nodes_[node_].boolean : fallback;
}

Value Document::root() const noexcept {
    if (nodes_.empty()) {
        return {};
    }
    return Value{this, 0};
}

std::expected<Document, ParseError> Document::parse(std::string_view text) {
    return parse(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

std::expected<Document, ParseError> Document::parse(std::span<const u8> bytes) {
    simdjson::dom::parser  parser;
    simdjson::dom::element root;

    // padded_string copies; simdjson needs SIMDJSON_PADDING bytes of slack past
    // the end and the caller's buffer has none.
    const auto padded =
        simdjson::padded_string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (parser.parse(padded).get(root) != simdjson::SUCCESS) {
        return std::unexpected(ParseError::Malformed);
    }

    Document document;

    // The work list holds (simdjson element, index of the node already reserved
    // for it, depth). Children are appended as they are discovered, so the walk
    // is a loop over a vector instead of a recursive descent — a resource pack
    // is a file someone downloaded, and a thousand nested arrays has to be a
    // parse error rather than a blown stack.
    struct Pending {
        simdjson::dom::element element;
        u32                    node;
        u32                    depth;
    };

    std::vector<Pending> pending;

    const auto reserve_node = [&document]() -> u32 {
        document.nodes_.emplace_back();
        return static_cast<u32>(document.nodes_.size() - 1);
    };

    // An object member whose key is the empty string is legal JSON and vanilla
    // uses it: the single-variant blockstate file keys its only variant on "".
    // key_length carries the truth; key_offset alone cannot tell "" from absent.
    const auto reserve_member = [&document](std::string_view key) -> u32 {
        Node node;
        node.key_offset = static_cast<u32>(document.text_.size());
        document.text_.append(key);
        node.key_length = static_cast<u32>(key.size());
        document.nodes_.push_back(node);
        return static_cast<u32>(document.nodes_.size() - 1);
    };

    pending.push_back({root, reserve_node(), 0});

    for (usize i = 0; i < pending.size(); ++i) {
        const auto entry = pending[i];
        if (entry.depth > kMaxDepth) {
            return std::unexpected(ParseError::TooDeep);
        }

        switch (entry.element.type()) {
            case simdjson::dom::element_type::OBJECT: {
                simdjson::dom::object object;
                if (entry.element.get(object) != simdjson::SUCCESS) {
                    return std::unexpected(ParseError::Malformed);
                }
                // Reserve every child slot before descending, so a node's
                // children occupy a contiguous run of children_.
                const auto first = static_cast<u32>(document.children_.size());
                u32        count = 0;
                for (auto [key, value] : object) {
                    const u32 child = reserve_member(std::string_view(key));
                    document.children_.push_back(child);
                    pending.push_back({value, child, entry.depth + 1});
                    ++count;
                }
                auto& node       = document.nodes_[entry.node];
                node.kind        = Kind::Object;
                node.first_child = first;
                node.count       = count;
                break;
            }
            case simdjson::dom::element_type::ARRAY: {
                simdjson::dom::array array;
                if (entry.element.get(array) != simdjson::SUCCESS) {
                    return std::unexpected(ParseError::Malformed);
                }
                const auto first = static_cast<u32>(document.children_.size());
                u32        count = 0;
                for (auto value : array) {
                    const u32 child = reserve_node();
                    document.children_.push_back(child);
                    pending.push_back({value, child, entry.depth + 1});
                    ++count;
                }
                auto& node       = document.nodes_[entry.node];
                node.kind        = Kind::Array;
                node.first_child = first;
                node.count       = count;
                break;
            }
            case simdjson::dom::element_type::STRING: {
                std::string_view s;
                if (entry.element.get(s) != simdjson::SUCCESS) {
                    return std::unexpected(ParseError::Malformed);
                }
                const auto offset = static_cast<u32>(document.text_.size());
                document.text_.append(s);
                auto& node       = document.nodes_[entry.node];
                node.kind        = Kind::String;
                node.text_offset = offset;
                node.text_length = static_cast<u32>(s.size());
                break;
            }
            case simdjson::dom::element_type::INT64:
            case simdjson::dom::element_type::UINT64:
            case simdjson::dom::element_type::DOUBLE: {
                f64 number = 0.0;
                if (entry.element.get(number) != simdjson::SUCCESS) {
                    return std::unexpected(ParseError::Malformed);
                }
                auto& node  = document.nodes_[entry.node];
                node.kind   = Kind::Number;
                node.number = number;
                break;
            }
            case simdjson::dom::element_type::BOOL: {
                bool value = false;
                if (entry.element.get(value) != simdjson::SUCCESS) {
                    return std::unexpected(ParseError::Malformed);
                }
                auto& node   = document.nodes_[entry.node];
                node.kind    = Kind::Bool;
                node.boolean = value;
                break;
            }
            case simdjson::dom::element_type::NULL_VALUE: {
                document.nodes_[entry.node].kind = Kind::Null;
                break;
            }
            // An integer too large for 64 bits. Legal JSON, and nothing a model
            // or blockstate file could mean by it, so it is a parse error
            // rather than a silently truncated coordinate.
            case simdjson::dom::element_type::BIGINT: return std::unexpected(ParseError::Malformed);
        }
    }

    return document;
}

}  // namespace ov::render::json
