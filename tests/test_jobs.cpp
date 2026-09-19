#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "core/jobs.h"

using namespace std::chrono_literals;
using nmtreediff::JobSystem;

TEST_CASE("jobs run and the system reports when it is idle", "[jobs]") {
    JobSystem jobs(2);
    std::atomic<int> completed{0};

    for (int i = 0; i < 16; ++i) {
        jobs.submit([&completed](std::stop_token) { ++completed; });
    }

    jobs.waitIdle();
    CHECK(completed.load() == 16);
    CHECK(jobs.outstanding() == 0);
}

TEST_CASE("a running job sees the stop token", "[jobs]") {
    JobSystem jobs(1);
    std::atomic<bool> started{false};
    std::atomic<bool> observedStop{false};

    jobs.submit([&](std::stop_token token) {
        started = true;
        // Stands in for a long pass that checks its token on a bounded
        // interval, which is how every real job is written.
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(1ms);
        }
        observedStop = true;
    });

    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }

    jobs.cancelAll();
    jobs.waitIdle();

    CHECK(observedStop.load());
}

TEST_CASE("queued jobs from a cancelled generation never run", "[jobs]") {
    JobSystem jobs(1);
    std::atomic<bool> blocking{true};
    std::atomic<int> ranAfter{0};

    // Occupy the single worker so the rest of the queue cannot start.
    jobs.submit([&](std::stop_token) {
        while (blocking.load()) {
            std::this_thread::sleep_for(1ms);
        }
    });

    for (int i = 0; i < 8; ++i) {
        jobs.submit([&ranAfter](std::stop_token) { ++ranAfter; });
    }

    jobs.cancelAll();
    blocking = false;
    jobs.waitIdle();

    // Dropping superseded work without running it is what makes cancellation
    // cheap for jobs that never started.
    CHECK(ranAfter.load() == 0);
}

TEST_CASE("cancelling advances the generation", "[jobs]") {
    JobSystem jobs(1);
    const auto first = jobs.generation();
    CHECK(jobs.isCurrent(first));

    const auto second = jobs.cancelAll();
    CHECK(second != first);
    CHECK_FALSE(jobs.isCurrent(first));
    CHECK(jobs.isCurrent(second));
}

TEST_CASE("work submitted after a cancel still runs", "[jobs]") {
    JobSystem jobs(2);
    jobs.cancelAll();

    std::atomic<int> completed{0};
    for (int i = 0; i < 4; ++i) {
        jobs.submit([&completed](std::stop_token) { ++completed; });
    }
    jobs.waitIdle();

    CHECK(completed.load() == 4);
}

TEST_CASE("the pool sizes itself and destructs cleanly while busy", "[jobs]") {
    {
        JobSystem jobs;
        CHECK(jobs.threadCount() >= 1);
        CHECK(jobs.threadCount() <= JobSystem::kMaxThreads);

        std::atomic<bool> started{false};
        jobs.submit([&started](std::stop_token token) {
            started = true;
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(1ms);
            }
        });
        while (!started.load()) {
            std::this_thread::sleep_for(1ms);
        }
        // Leaving the scope must cancel, stop the workers and join without
        // hanging, which is the shutdown path the window relies on.
    }
    SUCCEED("destructor completed");
}
