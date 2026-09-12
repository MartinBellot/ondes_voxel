// A fixed pool of worker threads, and the queue that feeds it.
//
// This exists so that work which is too expensive for the tick can leave the
// tick thread without leaving the single-writer rule behind. The pool itself is
// deliberately dumb: it runs closures, it has no idea what a chunk is, and it
// never touches the world. Everything that matters about thread safety lives in
// what the caller puts in the closure — see docs/provenance/chunkmap.md.
//
// Two design points that are not accidents:
//
//   * A job is handed its **worker index**. That is what lets a caller give
//     each worker its own private state — the chunk generator's pipeline, for
//     one — and reach it without a lock, because only one thread ever holds a
//     given index. A pool that hid the index would force either a mutex or a
//     thread_local, and thread_local state that outlives the pool is how a
//     "why is the second server's terrain wrong" bug is born.
//
//   * There is no way to wait on a single job. Results come back through a
//     queue the caller owns and drains from the thread that is allowed to
//     publish them. A `future` per job would put the tick thread back in the
//     business of waiting, which is exactly what this is for.
//
// The queue does have a mutex. That is not a violation of CLAUDE.md principle
// 3: the rule forbids a mutex on the *world*, on chunks that the tick thread
// publishes. A mutex around a queue of closures protects the queue, and the
// world it hands over has already left its producer for good.
#pragma once

#include "ov/base/thread.hpp"
#include "ov/base/types.hpp"

#include <functional>
#include <memory>

namespace ov::base {

/// A pool of `worker_count` threads running submitted jobs.
///
/// Jobs run in an unspecified order and possibly at the same time. Nothing here
/// promises otherwise, and a caller that needs a deterministic result must get
/// it from the shape of the job, not from the order the pool happens to pick.
class JobPool {
public:
    /// Start `workers` threads. Zero is accepted and means "run every job on
    /// the calling thread inside `submit`" — the shape a test wants when it is
    /// comparing a parallel run against a serial one.
    ///
    /// `role` is the scheduling class of the threads (thread.hpp).
    explicit JobPool(usize workers, ThreadRole role = ThreadRole::Worker);

    JobPool(const JobPool&)            = delete;
    JobPool& operator=(const JobPool&) = delete;
    JobPool(JobPool&&)                 = delete;
    JobPool& operator=(JobPool&&)      = delete;

    /// Stops accepting jobs, lets the ones already running finish, and joins.
    /// Queued-but-unstarted jobs are dropped: a shutdown that waits for a
    /// backlog of terrain nobody will ever see is a hang, not a courtesy.
    ~JobPool();

    /// Queue a job. `job` is called with the index of the worker running it,
    /// in `[0, worker_count())`.
    void submit(std::function<void(usize worker)> job);

    /// Threads actually started. Zero means inline execution.
    [[nodiscard]] usize worker_count() const noexcept;

    /// Jobs queued but not yet started, plus jobs currently running.
    [[nodiscard]] usize in_flight() const noexcept;

    /// Block until `in_flight()` reaches zero. For tests and shutdown, never
    /// for the tick thread.
    void wait_idle();

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace ov::base
