#pragma once

/// \file
/// \brief One pair of documents and the work that turns them into a snapshot.

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/jobs.h"
#include "core/layout_tree.h"
#include "core/lua_provider.h"
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

    /// \brief The sizes to lay the node view out in.
    ///
    /// \remarks Supplied by the caller because layout units are character
    ///          cells and the text size is the interface's decision, which the
    ///          core has no way of knowing. The default suits a thirteen-pixel
    ///          font, which is what a headless run and the tests measure in.
    LayoutMetrics layoutMetrics;

    /// \brief Which way to draw the node graph when the format has no
    ///        opinion.
    ///
    /// \remarks The reader's standing choice. A provider that names a
    ///          direction of its own wins over this, because it knows the shape
    ///          of its trees and this is only a default.
    GraphDirection graphDirection = GraphDirection::TopDown;
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

    /// \brief Returns the formats this session resolves with today.
    ///
    /// \returns The current registry. A comparison already in flight may be
    ///          using an earlier one, which its snapshot keeps alive.
    [[nodiscard]] const ProviderRegistry& registry() const noexcept { return *registry_; }

    /// \brief Builds a fresh registry from a provider configuration and moves
    ///        the session on to it.
    ///
    /// \param config The configuration to apply.
    ///
    /// \returns The provider names the configuration mentioned that the
    ///          registry does not know.
    ///
    /// \remarks The registry starts from the built-in formats every time, so
    ///          calling this again with a changed configuration is Reload: a
    ///          scripted provider edited on disk is read afresh and nothing of
    ///          the old one survives. A comparison in flight keeps the registry
    ///          it started with, and so does every snapshot it published, so
    ///          this is safe to call while one is running; call open() after
    ///          it to compare with the new one.
    ///
    ///          Call from the thread that calls open(), never from a worker.
    std::vector<std::string> configureProviders(const ProviderConfig& config);

    /// \brief Blocks until the pipeline settles.
    ///
    /// \remarks For tests and headless runs only, never for the frame loop.
    void waitIdle() const { jobs_.waitIdle(); }

private:
    void runOpen(const SessionRequest& request, std::shared_ptr<const ProviderRegistry> registry,
                 std::stop_token token, Generation generation);
    void publish(DiffSnapshot snapshot, Generation generation);

    JobSystem jobs_;
    std::shared_ptr<const ProviderRegistry> registry_ =
        std::make_shared<const ProviderRegistry>(makeDefaultRegistry());
    SnapshotBox<DiffSnapshot> box_;
};

}  // namespace nmxd
