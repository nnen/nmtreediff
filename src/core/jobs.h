#pragma once

// The worker pool behind constraint C: the frame loop never blocks.
//
// Two things make this small rather than general. Jobs are coarse, a handful
// per diff rather than thousands, so a plain mutex and queue are ample.
// Cancellation is routine rather than exceptional, so every job is handed a
// stop token whether it expects to be cancelled or not.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace nmxd {

// Identifies one round of work. Cancelling bumps the generation, and any job
// still queued from an older generation is dropped rather than run.
using Generation = std::uint64_t;

class JobSystem {
public:
    // threadCount of 0 picks hardware concurrency minus one, clamped to
    // kMaxThreads, because the work is a few coarse jobs and oversubscribing
    // only costs context switches.
    static constexpr unsigned kMaxThreads = 4;

    explicit JobSystem(unsigned threadCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Queues a job in the current generation. The token it receives is already
    // stopped if cancelAll() ran between the submit and the job starting.
    Generation submit(std::function<void(std::stop_token)> job);

    // Requests a stop on everything in flight and discards everything queued.
    // Returns the new generation. Does not wait for running jobs to notice.
    Generation cancelAll();

    [[nodiscard]] Generation generation() const;

    // True when the given generation is still the current one.
    [[nodiscard]] bool isCurrent(Generation generation) const;

    [[nodiscard]] unsigned threadCount() const noexcept { return static_cast<unsigned>(workers_.size()); }

    // Queued jobs not yet started, plus jobs currently running.
    [[nodiscard]] std::size_t outstanding() const;

    // Blocks until nothing is queued or running. For tests and shutdown, never
    // for the frame loop.
    void waitIdle() const;

private:
    struct Job {
        Generation generation = 0;
        std::stop_token token;
        std::function<void(std::stop_token)> fn;
    };

    void workerLoop(std::stop_token shutdown);

    mutable std::mutex mutex_;
    mutable std::condition_variable_any queued_;
    mutable std::condition_variable_any idle_;

    std::deque<Job> pending_;
    std::size_t running_ = 0;

    Generation generation_ = 1;
    std::stop_source cancelSource_;

    // Declared last so the threads are joined before the state they touch is
    // destroyed.
    std::vector<std::jthread> workers_;
};

}  // namespace nmxd
