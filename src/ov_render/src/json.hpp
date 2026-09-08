// A flat, non-template JSON document, and the one place simdjson is allowed.
//
// Two rules meet here and pull in opposite directions. simdjson is the fastest
// way to read the 3000-odd JSON files a resource pack contains, and it is also
// exactly the kind of template-heavy header that CLAUDE.md § 5 keeps out of
// anything another translation unit includes: pulling it into every file that
// touches a model would put the four-minute rebuild of risk R5 back on the
// table.
//
// So simdjson appears in json.cpp and nowhere else. It fills the flat node
// array below, and every other file in this module reads that. The cost is one
// copy of each document at load time, which happens once at startup and never
// in a frame.
//
// The side benefit is that the parser is swappable: nothing outside json.cpp
// names simdjson, so replacing it is a one-file change.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ov::render::json {

enum class Kind : u8 { Null, Bool, Number, String, Array, Object };

enum class ParseError {
    /// Not valid JSON.
    Malformed,
    /// Nesting deeper than kMaxDepth. Not a JSON rule: a defence, so that a
    /// hostile resource pack cannot turn a download into a stack overflow.
    TooDeep,
    /// More nodes than a document can address.
    TooLarge,
};

[[nodiscard]] std::string_view to_string(ParseError error) noexcept;

/// Deeper than any vanilla model or blockstate file by a wide margin: the
/// deepest thing in the 1.20.1 assets is a multipart `when` with an `OR`,
/// which reaches six.
inline constexpr u32 kMaxDepth = 64;

class Document;

/// A cursor into a Document. Cheap to copy, borrows the document, and is
/// invalidated by its destruction — the same contract as string_view.
class Value {
public:
    constexpr Value() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return document_ != nullptr; }

    [[nodiscard]] Kind kind() const noexcept;

    [[nodiscard]] bool is_object() const noexcept { return valid() && kind() == Kind::Object; }

    [[nodiscard]] bool is_array() const noexcept { return valid() && kind() == Kind::Array; }

    [[nodiscard]] bool is_string() const noexcept { return valid() && kind() == Kind::String; }

    [[nodiscard]] bool is_number() const noexcept { return valid() && kind() == Kind::Number; }

    [[nodiscard]] bool is_bool() const noexcept { return valid() && kind() == Kind::Bool; }

    /// Number of members of an object, or elements of an array. Zero otherwise.
    [[nodiscard]] u32 size() const noexcept;

    /// Member of an object by name. An invalid Value when absent, so lookups
    /// chain without a null check at every step.
    [[nodiscard]] Value operator[](std::string_view key) const noexcept;

    /// Element of an array. An invalid Value when out of range.
    [[nodiscard]] Value operator[](u32 index) const noexcept;

    /// The key of the i-th member of an object.
    [[nodiscard]] std::string_view key_at(u32 index) const noexcept;

    /// The value of the i-th member of an object, in file order.
    [[nodiscard]] Value value_at(u32 index) const noexcept;

    [[nodiscard]] std::string_view as_string(std::string_view fallback = {}) const noexcept;

    [[nodiscard]] f64 as_number(f64 fallback = 0.0) const noexcept;

    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;

private:
    friend class Document;

    constexpr Value(const Document* document, u32 node) noexcept
        : document_(document), node_(node) {}

    const Document* document_{nullptr};
    u32             node_{0};
};

/// An owned, parsed document.
///
/// Nodes are stored in one flat vector in depth-first order, so a document is
/// two allocations rather than one per value, and freeing it is two frees
/// rather than a tree walk.
class Document {
public:
    [[nodiscard]] static std::expected<Document, ParseError> parse(std::span<const u8> bytes);

    [[nodiscard]] static std::expected<Document, ParseError> parse(std::string_view text);

    [[nodiscard]] Value root() const noexcept;

    [[nodiscard]] usize node_count() const noexcept { return nodes_.size(); }

private:
    friend class Value;

    struct Node {
        Kind kind{Kind::Null};
        bool boolean{false};
        f64  number{0.0};
        /// Offset and length into text_, for String nodes and for object keys.
        u32 text_offset{0};
        u32 text_length{0};
        u32 key_offset{0};
        u32 key_length{0};
        /// Children of an array or object: `count` nodes whose indices are
        /// stored in children_ starting at `first_child`. An indirection rather
        /// than a contiguous run, because depth-first order interleaves the
        /// grandchildren.
        u32 first_child{0};
        u32 count{0};
    };

    std::vector<Node> nodes_;
    std::vector<u32>  children_;
    std::string       text_;
};

}  // namespace ov::render::json
