// The NBT value type.
//
// NBT is Minecraft's binary tree format: chunks, level.dat, entities, item
// stacks, structure files and part of the wire protocol are all NBT. Thirteen
// tag types, big-endian throughout.
//
// Three design choices worth stating, because all three are load-bearing:
//
//   Compounds preserve insertion order. The format itself is unordered, but
//   keeping the order makes read-then-write byte-identical, which turns "does
//   our Anvil support work?" into a checkable question instead of an opinion.
//   A hash map would make that test impossible to write.
//
//   Lists carry their element type separately from their contents. An empty
//   list still declares an element type on disk — TAG_End in vanilla — and
//   dropping it changes the bytes on the way back out.
//
//   Lists and compounds live behind a pointer. std::variant requires complete
//   alternatives, so a directly recursive std::vector<Tag> does not compile;
//   the indirection is what makes the tree expressible at all. Scalars and
//   arrays stay inline, which is the overwhelming majority of tags in a chunk.
#pragma once

#include "ov/base/types.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ov::nbt {

/// Tag type ids, exactly as they appear on disk and on the wire.
enum class TagType : u8 {
    End       = 0,
    Byte      = 1,
    Short     = 2,
    Int       = 3,
    Long      = 4,
    Float     = 5,
    Double    = 6,
    ByteArray = 7,
    String    = 8,
    List      = 9,
    Compound  = 10,
    IntArray  = 11,
    LongArray = 12,
};

inline constexpr u8 kTagTypeCount = 13;

[[nodiscard]] std::string_view to_string(TagType type) noexcept;
[[nodiscard]] bool             is_valid_tag_type(u8 raw) noexcept;

struct ListData;
struct CompoundData;
struct CompoundEntry;

class Tag {
public:
    using ByteArray = std::vector<u8>;
    using IntArray  = std::vector<i32>;
    using LongArray = std::vector<i64>;

    /// An empty Tag is TAG_End, which is also what a failed lookup yields.
    Tag() noexcept;
    ~Tag();
    Tag(const Tag& other);
    Tag& operator=(const Tag& other);
    Tag(Tag&& other) noexcept;
    Tag& operator=(Tag&& other) noexcept;

    Tag(i8 value) noexcept;   // NOLINT(*-explicit-constructor)
    Tag(i16 value) noexcept;  // NOLINT(*-explicit-constructor)
    Tag(i32 value) noexcept;  // NOLINT(*-explicit-constructor)
    Tag(i64 value) noexcept;  // NOLINT(*-explicit-constructor)
    Tag(f32 value) noexcept;  // NOLINT(*-explicit-constructor)
    Tag(f64 value) noexcept;  // NOLINT(*-explicit-constructor)
    Tag(std::string value);   // NOLINT(*-explicit-constructor)
    Tag(ByteArray value);     // NOLINT(*-explicit-constructor)
    Tag(IntArray value);      // NOLINT(*-explicit-constructor)
    Tag(LongArray value);     // NOLINT(*-explicit-constructor)

    /// A list must be built with its element type, since an empty list still
    /// declares one on disk.
    [[nodiscard]] static Tag make_list(TagType element_type);
    [[nodiscard]] static Tag make_compound();

    /// Booleans are TAG_Byte, matching vanilla.
    [[nodiscard]] static Tag make_bool(bool value) noexcept;

    [[nodiscard]] TagType type() const noexcept;

    [[nodiscard]] bool is_end() const noexcept { return type() == TagType::End; }

    // ── Scalar and array access ─────────────────────────────────────────────
    // Returns nullptr on a type mismatch. NBT read from a world or a packet is
    // untrusted: a field that "must" be an int may not be one, and the caller
    // has to be able to find that out.

    template<typename T>
    [[nodiscard]] const T* get_if() const noexcept {
        return std::get_if<T>(&value_);
    }

    template<typename T>
    [[nodiscard]] T* get_if() noexcept {
        return std::get_if<T>(&value_);
    }

    /// Numeric value widened to i64, whatever integral tag holds it.
    ///
    /// Deliberately permissive: vanilla has changed a field's tag type between
    /// versions more than once, and refusing to read a TAG_Int where a TAG_Byte
    /// was expected would fail on real worlds. Returns fallback for non-numeric
    /// tags.
    [[nodiscard]] i64              as_i64(i64 fallback = 0) const noexcept;
    [[nodiscard]] f64              as_f64(f64 fallback = 0.0) const noexcept;
    [[nodiscard]] bool             as_bool(bool fallback = false) const noexcept;
    [[nodiscard]] std::string_view as_string(std::string_view fallback = {}) const noexcept;

    // ── List access ─────────────────────────────────────────────────────────

    /// Element type of a list; TagType::End for anything else.
    [[nodiscard]] TagType list_element_type() const noexcept;

    /// The elements, or nullptr if this is not a list.
    [[nodiscard]] const std::vector<Tag>* list() const noexcept;
    [[nodiscard]] std::vector<Tag>*       list() noexcept;

    /// Append to a list. Returns false if this is not a list, or if the element
    /// type does not match what the list declared — NBT lists are homogeneous,
    /// and vanilla refuses to load a file that breaks this.
    bool push(Tag value);

    // ── Compound access ─────────────────────────────────────────────────────

    /// The entries in insertion order, or nullptr if this is not a compound.
    [[nodiscard]] const std::vector<CompoundEntry>* compound() const noexcept;
    [[nodiscard]] std::vector<CompoundEntry>*       compound() noexcept;

    /// Look up by name. Returns nullptr when absent or when this is not a
    /// compound. Linear scan: NBT compounds are small — a chunk section has a
    /// handful of keys — and a flat vector beats a hash map at that size while
    /// also preserving order.
    [[nodiscard]] const Tag* find(std::string_view name) const noexcept;
    [[nodiscard]] Tag*       find(std::string_view name) noexcept;

    [[nodiscard]] bool contains(std::string_view name) const noexcept {
        return find(name) != nullptr;
    }

    /// Insert or replace, keeping an existing key's position. Returns false if
    /// this is not a compound.
    bool put(std::string name, Tag value);

    /// Remove by name. Returns true if something was removed.
    bool erase(std::string_view name);

    // ── Common ──────────────────────────────────────────────────────────────

    /// Children for a list or compound, elements for an array or string,
    /// 0 otherwise.
    [[nodiscard]] usize size() const noexcept;

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Structural equality, order included for compounds.
    [[nodiscard]] bool operator==(const Tag& other) const noexcept;

private:
    // std::monostate stands for TAG_End. Lists and compounds are boxed because
    // std::variant needs complete alternatives and they contain Tags.
    using Value =
        std::variant<std::monostate, i8, i16, i32, i64, f32, f64, ByteArray, std::string,
                     std::unique_ptr<ListData>, std::unique_ptr<CompoundData>, IntArray, LongArray>;

    Value value_;
};

struct CompoundEntry {
    std::string name;
    Tag         value;

    [[nodiscard]] bool operator==(const CompoundEntry&) const noexcept = default;
};

struct ListData {
    TagType          element_type{TagType::End};
    std::vector<Tag> items;
};

struct CompoundData {
    std::vector<CompoundEntry> entries;
};

}  // namespace ov::nbt
