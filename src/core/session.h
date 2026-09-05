#pragma once

/// \file
/// \brief One pair of documents and the work that turns them into a snapshot.

#include <atomic>
#include <filesystem>
#include <string>

#include "core/jobs.h"
#include "core/registry.h"
#include "core/snapshot.h"

namespace nmxd {

/// \brief What to open, and how to label it.
struct SessionRequest {
    /// \brief Path to the left, usually older, file.
    std::filesystem::path leftPath;
    /// \brief Path to the right, usually newer, file.
    std::filesystem::path rightPath;

    /// \brief Title to show for the left side, defaulting to the path.
    std::string leftLabel;
    /// \brief Title to show for the right side, defaulting to the path.
    std::string rightLabel;

    /// \brief The format to use, or empty to sniff one.
    ///
    /// \remarks A name the registry does not know is reported rather than
    ///          quietly ignored, because it is usually a typo in a diff-tool
    ///          configuration that would otherwise go unnoticed for a long time.
    std::string format;
};

/// \brief Owns a comparison: its worker pool, its registry, and its snapshot.
///
/// \remarks This is the only thing the interface talks to. The frame loop asks
///          for the current snapshot and calls open() or cancel(), and never
///          touches a thread or a job directly.
///
///          Work runs in stages and each stage publishes, so the raw text is on
///          screen while the alignment is still running and the text view works
///          even when parsing later fails.
class Session {
public:
    /// \brief Starts a session and its worker pool.
    ///
    /// \param threadCount How many workers to start, or zero to choose
    ///        automatically.
    explicit Session(unsigned threadCount = 0);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// \brief Cancels anything in flight and starts reading a new pair.
    ///
    /// \param request What to open.
    ///
    /// \remarks Returns immediately. Progress arrives through snapshot().
    void open(SessionRequest request);

    /// \brief Cancels the work in flight.
    ///
    /// \remarks The last published snapshot stays in place.
    void cancel();

    /// \brief Takes a reference to the current snapshot.
    ///
    /// \returns The snapshot, safe to read for as long as the reference is held.
    [[nodiscard]] std::shared_ptr<const DiffSnapshot> snapshot() const { return box_.get(); }

    /// \brief Returns how many snapshots have been published.
    ///
    /// \returns A counter that only increases.
    [[nodiscard]] std::uint64_t snapshotVersion() const { return box_.version(); }

    /// \brief Reports whether any work is outstanding.
    ///
    /// \returns `true` when a job is queued or running.
    [[nodiscard]] bool busy() const { return jobs_.outstanding() > 0; }

    /// \brief Returns the size of the worker pool.
    ///
    /// \returns The number of worker threads.
    [[nodiscard]] unsigned threadCount() const noexcept { return jobs_.threadCount(); }

    /// \brief Returns the formats this session can resolve.
    ///
    /// \returns The registry, valid for the lifetime of the session.
    [[nodiscard]] const ProviderRegistry& registry() const noexcept { return registry_; }

    /// \brief Blocks until the pipeline settles.
    ///
    /// \remarks For tests and headless runs only, never for the frame loop.
    void waitIdle() const { jobs_.waitIdle(); }

private:
    void runOpen(const SessionRequest& request, std::stop_token token, Generation generation);
    void publish(DiffSnapshot snapshot, Generation generation);

    JobSystem jobs_;
    ProviderRegistry registry_ = makeDefaultRegistry();
    SnapshotBox<DiffSnapshot> box_;
};

}  // namespace nmxd
