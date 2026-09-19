#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

#include "core/log.h"

using nmtreediff::Log;
using nmtreediff::LogLine;
using nmtreediff::LogStream;

// The sink is process-wide, so every test clears it and reads from its own
// starting sequence rather than from zero.

TEST_CASE("a line comes back from the sink with its stream", "[log]") {
    Log& log = Log::instance();
    log.forwardTo(nullptr, nullptr);
    const std::uint64_t before = log.lastSequence();

    nmtreediff::logOut("report line\n");
    nmtreediff::logErr("nmtreediff: something failed");

    const std::vector<LogLine> lines = log.linesAfter(before);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].stream == LogStream::Out);
    CHECK(lines[0].text == "report line");  // the newline is the line's own
    CHECK(lines[1].stream == LogStream::Err);
    CHECK(lines[1].text == "nmtreediff: something failed");
    CHECK(lines[1].sequence == lines[0].sequence + 1);
    CHECK(log.lastErrorSequence() == lines[1].sequence);
    CHECK(log.lastSequence() == lines[1].sequence);

    log.forwardTo(stdout, stderr);
}

TEST_CASE("a multi-line message stays one entry", "[log]") {
    // A Lua traceback is several lines and one event; splitting it would turn
    // one failure into a dozen error entries in the pane.
    Log& log = Log::instance();
    log.forwardTo(nullptr, nullptr);
    const std::uint64_t before = log.lastSequence();

    nmtreediff::logErr("boom\nstack traceback:\n\t[C]: in function 'error'\n");

    const std::vector<LogLine> lines = log.linesAfter(before);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "boom\nstack traceback:\n\t[C]: in function 'error'");

    log.forwardTo(stdout, stderr);
}

TEST_CASE("the buffer is bounded and the oldest lines go first", "[log]") {
    Log& log = Log::instance();
    log.forwardTo(nullptr, nullptr);
    log.clear();

    for (std::size_t i = 0; i < Log::kCapacity + 10; ++i) {
        nmtreediff::logOut(std::to_string(i));
    }

    CHECK(log.size() == Log::kCapacity);
    const std::vector<LogLine> lines = log.linesAfter(0);
    REQUIRE(lines.size() == Log::kCapacity);
    CHECK(lines.front().text == "10");
    CHECK(lines.back().text == std::to_string(Log::kCapacity + 9));

    log.clear();
    log.forwardTo(stdout, stderr);
}

TEST_CASE("clearing keeps the sequence counting", "[log]") {
    // A reader that remembers where it got to must not be handed the same
    // lines again after someone pressed Clear.
    Log& log = Log::instance();
    log.forwardTo(nullptr, nullptr);

    nmtreediff::logOut("one");
    const std::uint64_t seen = log.lastSequence();
    log.clear();
    nmtreediff::logOut("two");

    const std::vector<LogLine> lines = log.linesAfter(seen);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "two");
    CHECK(lines[0].sequence == seen + 1);

    log.forwardTo(stdout, stderr);
}

TEST_CASE("writes from several threads all arrive", "[log]") {
    Log& log = Log::instance();
    log.forwardTo(nullptr, nullptr);
    log.clear();

    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) {
                nmtreediff::logErr("t" + std::to_string(t) + " " + std::to_string(i));
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }

    CHECK(log.size() == kThreads * kPerThread);

    log.clear();
    log.forwardTo(stdout, stderr);
}
