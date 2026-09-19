#pragma once

/// \file
/// \brief The worker pool that keeps unbounded work off the frame loop.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace nmtreediff {

/// \brief Identifies one round of work.
///
/// \remarks Cancelling advances the generation, and any job still queued from an
///          older generation is dropped rather than run.
using Generation = std::uint64_t;

/// \brief A small pool of worker threads with cancellable, coarse jobs.
///
/// \remarks Two things keep this small rather than general. Jobs are coarse, a
///          handful per diff rather than thousands, so a plain mutex and queue
///          are ample. Cancellation is routine rather than exceptional, so every
///          job is handed a stop token whether it expects to be cancelled or
///          not.
class JobSystem {
public:
    /// \brief The largest pool this class will create on its own.
    static constexpr unsigned kMaxThreads = 4;

    /// \brief Starts the pool.
    ///
    /// \param threadCount How many workers to start, or zero to pick hardware
    ///        concurrency minus one, clamped to kMaxThreads.
    ///
    /// \remarks Oversubscribing buys nothing here, because the work is a few
    ///          coarse jobs rather than fine-grained parallelism.
    explicit JobSystem(unsigned threadCount = 0);

    /// \brief Cancels outstanding work, stops the workers and joins them.
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// \brief Queues a job in the current generation.
    ///
    /// \param job The work to run. It receives a stop token that is already
    ///        signalled if cancelAll() ran between the submit and the job
    ///        starting.
    ///
    /// \returns The generation the job was queued in.
    Generation submit(std::function<void(std::stop_token)> job);

    /// \brief Requests a stop on everything running and discards everything
    ///        queued.
    ///
    /// \returns The new generation.
    ///
    /// \remarks Returns immediately without waiting for running jobs to notice.
    Generation cancelAll();

    /// \brief Returns the current generation.
    ///
    /// \returns The generation new work would be queued in.
    [[nodiscard]] Generation generation() const;

    /// \brief Reports whether a generation is still the current one.
    ///
    /// \param generation The generation to test.
    ///
    /// \returns `true` when nothing has been cancelled since.
    [[nodiscard]] bool isCurrent(Generation generation) const;

    /// \brief Returns the number of worker threads.
    ///
    /// \returns The pool size.
    [[nodiscard]] unsigned threadCount() const noexcept {
        return static_cast<unsigned>(workers_.size());
    }

    /// \brief Returns how much work is outstanding.
    ///
    /// \returns Jobs queued but not started, plus jobs currently running.
    [[nodiscard]] std::size_t outstanding() const;

    /// \brief Blocks until nothing is queued or running.
    ///
    /// \remarks For tests and shutdown, never for the frame loop.
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

}  // namespace nmtreediff
