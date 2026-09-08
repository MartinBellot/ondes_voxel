// Opaque resource handles: an index and a generation, never a pointer.
//
// A renderer destroys and recreates resources constantly — a swapchain on
// resize, a chunk mesh every time a block changes — and a pointer that outlives
// its resource is a use-after-free that reproduces once a week and never in a
// debugger. A generation counter turns that into a clean rejection: the slot is
// reused, the generation moves on, and the stale handle stops matching.
//
// Eight bytes, trivially copyable, and safe to hold across frames. The rule
// this enforces is that nothing outside ov_rhi ever holds a Vulkan object.
#pragma once

#include "ov/base/types.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace ov::rhi {

/// A handle to a resource of one kind. The tag stops a BufferHandle being
/// passed where an ImageHandle belongs — they are both a pair of integers, and
/// the compiler is the only thing that will ever notice the difference.
template<typename Tag>
struct Handle {
    u32 index{0};
    /// Zero is never a live generation, so a default-constructed handle is
    /// invalid without needing a sentinel index.
    u32 generation{0};

    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }

    friend constexpr bool operator==(Handle, Handle) noexcept = default;
};

/// A slot map keyed by Handle.
///
/// Slots are reused, so the array stays dense and lookup stays one bounds check
/// and one comparison. `T` is whatever the backend needs to store; nothing here
/// knows what that is.
template<typename T, typename Tag>
class HandlePool {
public:
    using HandleType = Handle<Tag>;

    [[nodiscard]] HandleType insert(T value) {
        if (!free_.empty()) {
            const u32 index = free_.back();
            free_.pop_back();
            slots_[index].value = std::move(value);
            slots_[index].live  = true;
            return HandleType{index, slots_[index].generation};
        }
        slots_.push_back(Slot{std::move(value), 1, true});
        return HandleType{static_cast<u32>(slots_.size() - 1), 1};
    }

    /// The stored value, or nullptr if the handle is stale or was never valid.
    [[nodiscard]] T* get(HandleType handle) noexcept {
        if (!handle.valid() || handle.index >= slots_.size()) {
            return nullptr;
        }
        Slot& slot = slots_[handle.index];
        if (!slot.live || slot.generation != handle.generation) {
            return nullptr;
        }
        return &slot.value;
    }

    [[nodiscard]] const T* get(HandleType handle) const noexcept {
        return const_cast<HandlePool*>(this)->get(handle);
    }

    /// Frees the slot and retires every handle to it. Returns the value so the
    /// caller can destroy the backend object it names.
    [[nodiscard]] bool remove(HandleType handle, T& out) noexcept {
        T* value = get(handle);
        if (value == nullptr) {
            return false;
        }
        out = std::move(*value);

        Slot& slot = slots_[handle.index];
        slot.live  = false;
        // Wrapping past zero would resurrect the oldest stale handles, so skip
        // it. A slot would have to be recycled four billion times to get here.
        slot.generation = slot.generation == kMaxGeneration ? 1 : slot.generation + 1;
        free_.push_back(handle.index);
        return true;
    }

    /// Live values, for the teardown that has to destroy whatever is left.
    template<typename Fn>
    void for_each(Fn&& fn) {
        for (auto& slot : slots_) {
            if (slot.live) {
                fn(slot.value);
            }
        }
    }

    [[nodiscard]] usize live_count() const noexcept {
        usize count = 0;
        for (const auto& slot : slots_) {
            count += slot.live ? 1 : 0;
        }
        return count;
    }

    [[nodiscard]] usize capacity() const noexcept { return slots_.size(); }

private:
    static constexpr u32 kMaxGeneration = 0xFFFFFFFFu;

    struct Slot {
        T    value{};
        u32  generation{1};
        bool live{false};
    };

    std::vector<Slot> slots_;
    std::vector<u32>  free_;
};

}  // namespace ov::rhi

template<typename Tag>
struct std::hash<ov::rhi::Handle<Tag>> {
    [[nodiscard]] std::size_t operator()(ov::rhi::Handle<Tag> handle) const noexcept {
        return std::hash<ov::u64>{}((static_cast<ov::u64>(handle.generation) << 32) | handle.index);
    }
};
