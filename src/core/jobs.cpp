/// \file
/// \brief Implementation of the worker pool.

#include "core/jobs.h"

#include <algorithm>

namespace nmxd {

namespace {

/// \brief Chooses how many workers to start.
///
/// \param requested The caller's request, or zero to choose automatically.
///
/// \returns \p requested when non-zero, otherwise hardware concurrency minus
///          one, clamped to JobSystem::kMaxThreads and never below one.
unsigned pickThreadCount(unsigned requested) {
    if (requested > 0) {
        return requested;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware <= 1) {
        return 1;
    }
    return std::min(hardware - 1, JobSystem::kMaxThreads);
}

}  // namespace

JobSystem::JobSystem(unsigned threadCount) {
    const unsigned count = pickThreadCount(threadCount);
    workers_.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        workers_.emplace_back([this](std::stop_token shutdown) { workerLoop(std::move(shutdown)); });
    }
}

JobSystem::~JobSystem() {
    // Cancel first so long-running jobs see a stopped token, then let jthread
    // request shutdown and join as it is destroyed.
    cancelAll();
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    queued_.notify_all();
}

Generation JobSystem::submit(std::function<void(std::stop_token)> job) {
    Generation generation = 0;
    {
        std::lock_guard lock(mutex_);
        generation = generation_;
        pending_.push_back(Job{generation, cancelSource_.get_token(), std::move(job)});
    }
    queued_.notify_one();
    return generation;
}

Generation JobSystem::cancelAll() {
    std::stop_source previous;
    {
        std::lock_guard lock(mutex_);
        previous = cancelSource_;
        cancelSource_ = std::stop_source{};
        pending_.clear();
        ++generation_;
    }
    // Requested outside the lock: a stop callback could otherwise run while we
    // hold the mutex and try to reach back into the job system.
    previous.request_stop();
    idle_.notify_all();
    return generation();
}

Generation JobSystem::generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
}

bool JobSystem::isCurrent(Generation generation) const {
    std::lock_guard lock(mutex_);
    return generation == generation_;
}

std::size_t JobSystem::outstanding() const {
    std::lock_guard lock(mutex_);
    return pending_.size() + running_;
}

void JobSystem::waitIdle() const {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return pending_.empty() && running_ == 0; });
}

void JobSystem::workerLoop(std::stop_token shutdown) {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            // The predicate overload returns false only when shutdown was
            // requested, which is the one way out of this loop.
            if (!queued_.wait(lock, shutdown, [this] { return !pending_.empty(); })) {
                return;
            }
            job = std::move(pending_.front());
            pending_.pop_front();

            if (job.generation != generation_) {
                // Superseded while it sat in the queue. Dropping it here is
                // what makes cancellation cheap for work that never started.
                if (pending_.empty()) {
                    idle_.notify_all();
                }
                continue;
            }
            ++running_;
        }

        if (job.fn && !job.token.stop_requested()) {
            job.fn(job.token);
        }

        {
            std::lock_guard lock(mutex_);
            --running_;
            if (pending_.empty() && running_ == 0) {
                idle_.notify_all();
            }
        }
    }
}

}  // namespace nmxd
