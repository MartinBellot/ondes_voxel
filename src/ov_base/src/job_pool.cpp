#define OV_LOG_CATEGORY "base"

#include "ov/base/job_pool.hpp"

#include "ov/base/thread.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ov::base {

struct JobPool::Impl {
    mutable std::mutex                            mutex;
    std::condition_variable                       work;
    std::condition_variable                       idle;
    std::deque<std::function<void(usize)>>        queue;
    std::vector<std::jthread>                     workers;
    usize                                         running{0};
    bool                                          stopping{false};

    void run(usize index, const std::stop_token& stop) {
        // Named and given a role as the first statement, as thread.hpp
        // requires: an unclassified thread on Apple Silicon migrates onto an
        // efficiency core under load, which is exactly the load this pool
        // exists for.
        set_thread_role("ov-gen" + std::to_string(index), ThreadRole::Worker);

        while (true) {
            std::function<void(usize)> job;
            {
                std::unique_lock lock{mutex};
                work.wait(lock, [&] { return stopping || stop.stop_requested() || !queue.empty(); });
                if (stopping || stop.stop_requested()) {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
                ++running;
            }

            job(index);

            {
                const std::scoped_lock lock{mutex};
                --running;
            }
            idle.notify_all();
        }
    }
};

JobPool::JobPool(usize workers) : impl_(std::make_unique<Impl>()) {
    // `impl`, not `this`. A worker thread outlives the start of `~JobPool`, and
    // libc++'s `~unique_ptr` nulls its stored pointer before running the
    // deleter — so a thread that read `impl_` during destruction would read a
    // null pointer, not a dying object. The raw pointer stays valid until the
    // deleter has finished, and the deleter is what joins these threads.
    Impl* impl = impl_.get();
    impl->workers.reserve(workers);
    for (usize index = 0; index < workers; ++index) {
        impl->workers.emplace_back(
            [impl, index](std::stop_token stop) { impl->run(index, stop); });
    }
}

JobPool::~JobPool() {
    {
        const std::scoped_lock lock{impl_->mutex};
        impl_->stopping = true;
        impl_->queue.clear();
    }
    impl_->work.notify_all();
    for (auto& worker : impl_->workers) {
        worker.request_stop();
    }
    // jthread joins itself; the explicit clear keeps the order obvious rather
    // than leaving it to member destruction order.
    impl_->workers.clear();
}

void JobPool::submit(std::function<void(usize)> job) {
    if (impl_->workers.empty()) {
        // Inline. `running` is still moved so that in_flight() means the same
        // thing in both modes, which is what lets a test drive the two with
        // one body.
        {
            const std::scoped_lock lock{impl_->mutex};
            ++impl_->running;
        }
        job(0);
        {
            const std::scoped_lock lock{impl_->mutex};
            --impl_->running;
        }
        impl_->idle.notify_all();
        return;
    }

    {
        const std::scoped_lock lock{impl_->mutex};
        if (impl_->stopping) {
            return;
        }
        impl_->queue.push_back(std::move(job));
    }
    impl_->work.notify_one();
}

usize JobPool::worker_count() const noexcept { return impl_->workers.size(); }

usize JobPool::in_flight() const noexcept {
    const std::scoped_lock lock{impl_->mutex};
    return impl_->queue.size() + impl_->running;
}

void JobPool::wait_idle() {
    std::unique_lock lock{impl_->mutex};
    impl_->idle.wait(lock, [&] { return impl_->queue.empty() && impl_->running == 0; });
}

}  // namespace ov::base
