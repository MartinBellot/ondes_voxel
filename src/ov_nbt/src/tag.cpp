#include "ov/nbt/tag.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace ov::nbt {

std::string_view to_string(TagType type) noexcept {
    switch (type) {
        case TagType::End: return "TAG_End";
        case TagType::Byte: return "TAG_Byte";
        case TagType::Short: return "TAG_Short";
        case TagType::Int: return "TAG_Int";
        case TagType::Long: return "TAG_Long";
        case TagType::Float: return "TAG_Float";
        case TagType::Double: return "TAG_Double";
        case TagType::ByteArray: return "TAG_Byte_Array";
        case TagType::String: return "TAG_String";
        case TagType::List: return "TAG_List";
        case TagType::Compound: return "TAG_Compound";
        case TagType::IntArray: return "TAG_Int_Array";
        case TagType::LongArray: return "TAG_Long_Array";
    }
    return "TAG_Unknown";
}

bool is_valid_tag_type(u8 raw) noexcept {
    return raw < kTagTypeCount;
}

// ── Special members ─────────────────────────────────────────────────────────
// Defined here, where ListData and CompoundData are complete, so that
// unique_ptr can destroy and clone them.

Tag::Tag() noexcept                 = default;
Tag::~Tag()                         = default;
Tag::Tag(Tag&&) noexcept            = default;
Tag& Tag::operator=(Tag&&) noexcept = default;

namespace {

/// Deep-copy a tag's payload.
///
/// The variant is not copy-assignable as a whole, because two of its
/// alternatives are unique_ptr. Copying has to go alternative by alternative,
/// cloning the boxed ones and copying the rest — which is also what makes Tag
/// a value type despite the indirection.
template<typename Value>
[[nodiscard]] Value clone_value(const Value& source) {
    return std::visit(
        [](const auto& alternative) -> Value {
            using T = std::decay_t<decltype(alternative)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ListData>>) {
                return std::make_unique<ListData>(*alternative);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<CompoundData>>) {
                return std::make_unique<CompoundData>(*alternative);
            } else {
                return alternative;
            }
        },
        source);
}

}  // namespace

Tag::Tag(const Tag& other) : value_{clone_value(other.value_)} {}

Tag& Tag::operator=(const Tag& other) {
    if (this != &other) {
        value_ = clone_value(other.value_);
    }
    return *this;
}

Tag::Tag(i8 value) noexcept : value_{value} {}

Tag::Tag(i16 value) noexcept : value_{value} {}

Tag::Tag(i32 value) noexcept : value_{value} {}

Tag::Tag(i64 value) noexcept : value_{value} {}

Tag::Tag(f32 value) noexcept : value_{value} {}

Tag::Tag(f64 value) noexcept : value_{value} {}

Tag::Tag(std::string value) : value_{std::move(value)} {}

Tag::Tag(ByteArray value) : value_{std::move(value)} {}

Tag::Tag(IntArray value) : value_{std::move(value)} {}

Tag::Tag(LongArray value) : value_{std::move(value)} {}

Tag Tag::make_list(TagType element_type) {
    Tag tag;
    tag.value_ = std::make_unique<ListData>(ListData{element_type, {}});
    return tag;
}

Tag Tag::make_compound() {
    Tag tag;
    tag.value_ = std::make_unique<CompoundData>();
    return tag;
}

Tag Tag::make_bool(bool value) noexcept {
    return Tag{static_cast<i8>(value ? 1 : 0)};
}

TagType Tag::type() const noexcept {
    switch (value_.index()) {
        case 0: return TagType::End;
        case 1: return TagType::Byte;
        case 2: return TagType::Short;
        case 3: return TagType::Int;
        case 4: return TagType::Long;
        case 5: return TagType::Float;
        case 6: return TagType::Double;
        case 7: return TagType::ByteArray;
        case 8: return TagType::String;
        case 9: return TagType::List;
        case 10: return TagType::Compound;
        case 11: return TagType::IntArray;
        case 12: return TagType::LongArray;
        default: return TagType::End;
    }
}

i64 Tag::as_i64(i64 fallback) const noexcept {
    if (const auto* v = get_if<i8>())
        return *v;
    if (const auto* v = get_if<i16>())
        return *v;
    if (const auto* v = get_if<i32>())
        return *v;
    if (const auto* v = get_if<i64>())
        return *v;
    if (const auto* v = get_if<f32>())
        return static_cast<i64>(*v);
    if (const auto* v = get_if<f64>())
        return static_cast<i64>(*v);
    return fallback;
}

f64 Tag::as_f64(f64 fallback) const noexcept {
    if (const auto* v = get_if<f32>())
        return static_cast<f64>(*v);
    if (const auto* v = get_if<f64>())
        return *v;
    if (const auto* v = get_if<i8>())
        return static_cast<f64>(*v);
    if (const auto* v = get_if<i16>())
        return static_cast<f64>(*v);
    if (const auto* v = get_if<i32>())
        return static_cast<f64>(*v);
    if (const auto* v = get_if<i64>())
        return static_cast<f64>(*v);
    return fallback;
}

bool Tag::as_bool(bool fallback) const noexcept {
    if (const auto* v = get_if<i8>())
        return *v != 0;
    if (type() == TagType::End)
        return fallback;
    return as_i64(fallback ? 1 : 0) != 0;
}

std::string_view Tag::as_string(std::string_view fallback) const noexcept {
    if (const auto* v = get_if<std::string>())
        return *v;
    return fallback;
}

// ── Lists ───────────────────────────────────────────────────────────────────

TagType Tag::list_element_type() const noexcept {
    const auto* data = get_if<std::unique_ptr<ListData>>();
    return data == nullptr ? TagType::End : (*data)->element_type;
}

const std::vector<Tag>* Tag::list() const noexcept {
    const auto* data = get_if<std::unique_ptr<ListData>>();
    return data == nullptr ? nullptr : &(*data)->items;
}

std::vector<Tag>* Tag::list() noexcept {
    auto* data = get_if<std::unique_ptr<ListData>>();
    return data == nullptr ? nullptr : &(*data)->items;
}

bool Tag::push(Tag value) {
    auto* data = get_if<std::unique_ptr<ListData>>();
    if (data == nullptr) {
        return false;
    }
    // NBT lists are homogeneous. Vanilla refuses to load a file that mixes
    // types in one list, so accepting it here would only defer the failure to
    // somewhere much harder to diagnose.
    if (value.type() != (*data)->element_type) {
        return false;
    }
    (*data)->items.push_back(std::move(value));
    return true;
}

// ── Compounds ───────────────────────────────────────────────────────────────

const std::vector<CompoundEntry>* Tag::compound() const noexcept {
    const auto* data = get_if<std::unique_ptr<CompoundData>>();
    return data == nullptr ? nullptr : &(*data)->entries;
}

std::vector<CompoundEntry>* Tag::compound() noexcept {
    auto* data = get_if<std::unique_ptr<CompoundData>>();
    return data == nullptr ? nullptr : &(*data)->entries;
}

const Tag* Tag::find(std::string_view name) const noexcept {
    const auto* entries = compound();
    if (entries == nullptr) {
        return nullptr;
    }
    const auto it = std::ranges::find(*entries, name, &CompoundEntry::name);
    return it == entries->end() ? nullptr : &it->value;
}

Tag* Tag::find(std::string_view name) noexcept {
    auto* entries = compound();
    if (entries == nullptr) {
        return nullptr;
    }
    const auto it = std::ranges::find(*entries, name, &CompoundEntry::name);
    return it == entries->end() ? nullptr : &it->value;
}

bool Tag::put(std::string name, Tag value) {
    auto* entries = compound();
    if (entries == nullptr) {
        return false;
    }
    const auto it = std::ranges::find(*entries, name, &CompoundEntry::name);
    if (it != entries->end()) {
        // Replace in place, so a read-modify-write keeps the original byte
        // layout as closely as possible.
        it->value = std::move(value);
    } else {
        entries->push_back(CompoundEntry{std::move(name), std::move(value)});
    }
    return true;
}

bool Tag::erase(std::string_view name) {
    auto* entries = compound();
    if (entries == nullptr) {
        return false;
    }
    const auto it = std::ranges::find(*entries, name, &CompoundEntry::name);
    if (it == entries->end()) {
        return false;
    }
    entries->erase(it);
    return true;
}

usize Tag::size() const noexcept {
    if (const auto* v = list())
        return v->size();
    if (const auto* v = compound())
        return v->size();
    if (const auto* v = get_if<ByteArray>())
        return v->size();
    if (const auto* v = get_if<IntArray>())
        return v->size();
    if (const auto* v = get_if<LongArray>())
        return v->size();
    if (const auto* v = get_if<std::string>())
        return v->size();
    return 0;
}

bool Tag::operator==(const Tag& other) const noexcept {
    if (type() != other.type()) {
        return false;
    }

    switch (type()) {
        case TagType::List:
            // Two empty lists declaring different element types are different
            // tags: they serialize to different bytes.
            return list_element_type() == other.list_element_type() && *list() == *other.list();
        case TagType::Compound: return *compound() == *other.compound();
        default: return value_ == other.value_;
    }
}

}  // namespace ov::nbt
