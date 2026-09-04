#pragma once

// A Session owns one pair of documents and the work that turns them into a
// snapshot. It is the only thing the interface talks to: the frame loop asks
// for the current snapshot and calls open() or cancel(), and never touches a
// thread or a job directly.

#include <atomic>
#include <filesystem>
#include <string>

#include "core/jobs.h"
#include "core/snapshot.h"

namespace nmxd {

struct SessionRequest {
    std::filesystem::path leftPath;
    std::filesystem::path rightPath;
    std::string leftLabel;
    std::string rightLabel;
};

class Session {
public:
    explicit Session(unsigned threadCount = 0);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Cancels anything in flight and starts reading the pair. Returns
    // immediately; progress arrives through snapshot().
    void open(SessionRequest request);

    // Cancels the work in flight and leaves the last snapshot in place.
    void cancel();

    [[nodiscard]] std::shared_ptr<const DiffSnapshot> snapshot() const { return box_.get(); }
    [[nodiscard]] std::uint64_t snapshotVersion() const { return box_.version(); }

    [[nodiscard]] bool busy() const { return jobs_.outstanding() > 0; }
    [[nodiscard]] unsigned threadCount() const noexcept { return jobs_.threadCount(); }

    // Blocks until the pipeline settles. For tests and headless runs only.
    void waitIdle() const { jobs_.waitIdle(); }

private:
    void runOpen(const SessionRequest& request, std::stop_token token, Generation generation);
    void publish(DiffSnapshot snapshot, Generation generation);

    JobSystem jobs_;
    SnapshotBox<DiffSnapshot> box_;
};

}  // namespace nmxd
