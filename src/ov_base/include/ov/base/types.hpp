// Fixed-width integer aliases and the strong-typedef helper.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace ov {

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using f32 = float;
using f64 = double;

using usize = std::size_t;
using isize = std::ptrdiff_t;

/// Strong typedef over an integral type.
///
/// The codebase is full of small integers that mean entirely different things:
/// a BlockStateId, an ItemId, an EntityId, a chunk index, a palette index. They
/// are all u16 or u32, and mixing them up compiles cleanly and produces wrong
/// worlds. `Id<Tag, u16>` makes that a type error instead.
///
/// Deliberately not implicitly convertible to its underlying type: `.value()`
/// is explicit at every point where the raw number escapes.
template<typename Tag, typename Repr>
class Id {
public:
    using repr_type = Repr;

    constexpr Id() noexcept = default;

    constexpr explicit Id(Repr v) noexcept : value_(v) {}

    [[nodiscard]] constexpr Repr value() const noexcept { return value_; }

    friend constexpr bool operator==(Id, Id) noexcept  = default;
    friend constexpr auto operator<=>(Id, Id) noexcept = default;

private:
    Repr value_{};
};

}  // namespace ov

// Hash support, so strong ids can key unordered containers without ceremony.
template<typename Tag, typename Repr>
struct std::hash<ov::Id<Tag, Repr>> {
    [[nodiscard]] std::size_t operator()(ov::Id<Tag, Repr> id) const noexcept {
        return std::hash<Repr>{}(id.value());
    }
};
