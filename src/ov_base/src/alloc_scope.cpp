#include "ov/base/alloc_scope.hpp"

// Detect a sanitizer build. AddressSanitizer replaces operator new itself, with
// redzones and use-after-free detection that are strictly better than a counter.
// Defining our own on top would take precedence and silently disable that, so
// the tracking stands aside: catching a use-after-free matters more than
// counting allocations, and the plain debug build already does the counting.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define OV_SANITIZER_ACTIVE 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define OV_SANITIZER_ACTIVE 1
#endif
#if !defined(OV_SANITIZER_ACTIVE)
#define OV_SANITIZER_ACTIVE 0
#endif

#if !defined(NDEBUG)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace ov {
namespace {

// Per thread, because the invariant is per thread: the tick thread must not
// allocate while the worker pool freely does.
thread_local u32              t_scope_depth = 0;
thread_local std::string_view t_scope_name{};

std::atomic<u64>  g_allocations{0};
std::atomic<u64>  g_bytes{0};
std::atomic<u64>  g_violations{0};
std::atomic<bool> g_abort_on_violation{true};

/// Called from operator new. Must not allocate, or it recurses forever.
///
/// [[maybe_unused]] because a sanitizer build keeps its own operator new and
/// nothing calls this — which is deliberate, not dead code.
[[maybe_unused]] void note_allocation(usize size) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(size, std::memory_order_relaxed);

    if (t_scope_depth == 0) {
        return;
    }
    g_violations.fetch_add(1, std::memory_order_relaxed);

    if (!g_abort_on_violation.load(std::memory_order_relaxed)) {
        return;
    }

    // Written with fprintf rather than the logger: the logger formats, and
    // formatting allocates, and this is the one place that cannot.
    std::fprintf(stderr,
                 "\n"
                 "════════════════════════════════════════════════════════════════\n"
                 " ONDES VOXEL — ALLOCATION IN A NO-ALLOC SCOPE\n"
                 "════════════════════════════════════════════════════════════════\n"
                 "  scope .... %.*s\n"
                 "  size ..... %zu bytes\n"
                 "\n"
                 "  This code runs where allocation is not allowed — the tick loop,\n"
                 "  or something it calls. An allocation here is invisible until the\n"
                 "  day the allocator takes a lock and one tick in a thousand costs\n"
                 "  5 ms instead of 0.3.\n"
                 "\n"
                 "  Usual causes: a std::string built for a message, a vector past\n"
                 "  its reserve, a std::function capturing too much, a std::map node.\n"
                 "════════════════════════════════════════════════════════════════\n\n",
                 static_cast<int>(t_scope_name.size()), t_scope_name.data(), size);
    std::fflush(stderr);
    std::abort();
}

}  // namespace

namespace detail {

void enter_no_alloc_scope(std::string_view name) noexcept {
    if (t_scope_depth == 0) {
        t_scope_name = name;
    }
    ++t_scope_depth;
}

void leave_no_alloc_scope() noexcept {
    if (t_scope_depth > 0) {
        --t_scope_depth;
    }
}

}  // namespace detail

AllocationStats allocation_stats() noexcept {
    return AllocationStats{g_allocations.load(std::memory_order_relaxed),
                           g_bytes.load(std::memory_order_relaxed),
                           g_violations.load(std::memory_order_relaxed)};
}

void reset_allocation_stats() noexcept {
    g_allocations.store(0, std::memory_order_relaxed);
    g_bytes.store(0, std::memory_order_relaxed);
    g_violations.store(0, std::memory_order_relaxed);
}

bool allocation_tracking_active() noexcept {
    return OV_SANITIZER_ACTIVE == 0;
}

void set_abort_on_violation(bool abort) noexcept {
    g_abort_on_violation.store(abort, std::memory_order_relaxed);
}

}  // namespace ov

// ── Global operator new / delete ────────────────────────────────────────────
//
// Replacing these globally is a heavy hammer, and it is why this only exists in
// debug builds without a sanitizer. Every form is provided: leaving one out
// means those allocations go through the default implementation unseen, which
// would make the check quietly incomplete — worse than absent.
#if !OV_SANITIZER_ACTIVE

void* operator new(std::size_t size) {
    ov::note_allocation(size);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    ov::note_allocation(size);
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}

// Aligned forms, C++17. std::aligned_alloc is not available on every platform's
// libc, so this uses what each one provides.
void* operator new(std::size_t size, std::align_val_t alignment) {
    ov::note_allocation(size);
    const std::size_t align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    if (void* p = _aligned_malloc(size == 0 ? align : size, align)) {
        return p;
    }
#else
    void* p = nullptr;
    if (::posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align,
                         size == 0 ? align : size) == 0) {
        return p;
    }
#endif
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete[](void* p, std::align_val_t alignment) noexcept {
    ::operator delete(p, alignment);
}

void operator delete(void* p, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(p, alignment);
}

void operator delete[](void* p, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(p, alignment);
}

#endif  // !OV_SANITIZER_ACTIVE

#endif  // !NDEBUG
