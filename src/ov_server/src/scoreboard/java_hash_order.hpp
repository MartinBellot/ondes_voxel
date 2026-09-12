// The order a Java HashMap<String, …> iterates in.
//
// Vanilla keeps objectives, teams, score holders and a team's members in
// hash maps and hash sets keyed by string, and several of its answers walk
// them as they are: `scoreboard objectives list`, `team list`, the holders a
// `*` names, the order teams reach a newcomer. The capture pins those orders
// (scripts/capture_scoreboard.py), and they are reproduced here rather than
// replaced by a sort that would read better and compare worse.
//
// What the order is made of — the documented behaviour of java.util.HashMap
// and String.hashCode, not anyone's code:
//
//   * a key's hash is `s[0]*31^(n-1) + … + s[n-1]` over its UTF-16 units, in
//     32-bit arithmetic, spread as `h ^ (h >>> 16)`;
//   * the table starts at 16 buckets and doubles whenever the entry count
//     exceeds three quarters of it, and never shrinks;
//   * iteration walks the buckets in index order (`hash & (capacity - 1)`)
//     and, inside one, the entries in the order they were inserted — a
//     resize splits a bucket without reordering it.
//
// Checked against the capture: the 18 objectives of phase one, the four
// teams, and the three holders of `*` come out in vanilla's order.
//
// (A bucket of eight or more entries in a table of 64 becomes a tree with
// another order; no scoreboard in any capture is that crowded.)
#pragma once

#include "ov/base/types.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace ov::server {

/// Java's `String.hashCode()` of a UTF-8 string, over its UTF-16 units.
[[nodiscard]] i32 java_string_hash(std::string_view utf8) noexcept;

/// Insertion bookkeeping for one Java hash map. The owner keeps its entries
/// wherever it likes and asks here for the order.
class JavaHashOrder {
public:
    /// A new key: returns its sequence number, and grows the table as Java's
    /// would.
    u64 insert() noexcept {
        ++size_;
        while (static_cast<u64>(size_) * 4 > static_cast<u64>(capacity_) * 3) {
            capacity_ *= 2;
        }
        return next_sequence_++;
    }
    void erase() noexcept {
        if (size_ > 0) {
            --size_;
        }
    }
    void clear() noexcept { size_ = 0; }

    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }

    /// Sort `items` into iteration order. `key(item)` gives the string and
    /// `sequence(item)` the number `insert` returned for it.
    template<typename T, typename Key, typename Sequence>
    void sort(std::vector<T>& items, Key key, Sequence sequence) const {
        const u32 mask = capacity_ - 1;
        std::ranges::sort(items, [&](const T& a, const T& b) {
            const u32 ba = bucket(java_string_hash(key(a)), mask);
            const u32 bb = bucket(java_string_hash(key(b)), mask);
            return ba != bb ? ba < bb : sequence(a) < sequence(b);
        });
    }

private:
    [[nodiscard]] static u32 bucket(i32 hash, u32 mask) noexcept {
        const auto h = static_cast<u32>(hash);
        return (h ^ (h >> 16)) & mask;
    }

    u32 size_{0};
    u32 capacity_{16};
    u64 next_sequence_{0};
};

}  // namespace ov::server
