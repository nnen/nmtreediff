#pragma once

/// \file
/// \brief The handoff between the worker pool and the frame loop.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "core/diff.h"
#include "core/source.h"
#include "core/textdiff.h"
#include "core/tree.h"

namespace nmxd {

class IFormatProvider;

/// \brief How far the pipeline has got.
///
/// \remarks Shown in the interface, so partial results are never presented as
///          complete.
enum class Stage {
    Idle,          ///< Nothing has been opened.
    Loading,       ///< Reading the two files.
    SourcesReady,  ///< Both sides read; raw text can be shown.
    TextReady,     ///< Line diff computed; the text view is usable.
    TreesParsed,   ///< Both sides parsed into trees.
    TreeReady,     ///< Trees matched; the node view is usable.
    Failed,        ///< Something went wrong; see DiffSnapshot::message.
};

/// \brief Converts a stage into a phrase suitable for a status readout.
///
/// \param stage The stage to describe.
///
/// \returns A short lower-case phrase, never null.
[[nodiscard]] const char* describe(Stage stage) noexcept;

/// \brief Everything the views may read about one comparison.
///
/// \remarks A snapshot is built on a worker and never changed after it is
///          published, so the frame loop can read it without locking anything
///          beyond acquiring the pointer.
///
///          Each stage adds to the snapshot rather than replacing what came
///          before, which is why a document the parser rejects still has a
///          working text view.
struct DiffSnapshot {
    /// \brief How far the pipeline got before producing this snapshot.
    Stage stage = Stage::Idle;

    /// \brief The left file. Present from Stage::SourcesReady onward.
    std::shared_ptr<const SourceFile> left;
    /// \brief The right file. Present from Stage::SourcesReady onward.
    std::shared_ptr<const SourceFile> right;

    /// \brief The line diff. Present from Stage::TextReady onward.
    std::shared_ptr<const TextDiff> text;

    /// \brief The parsed left tree. Present from Stage::TreesParsed onward.
    std::shared_ptr<const Tree> leftTree;
    /// \brief The parsed right tree. Present from Stage::TreesParsed onward.
    std::shared_ptr<const Tree> rightTree;

    /// \brief The provider that parsed both trees.
    ///
    /// \remarks Held alongside them because reading a tree means asking the
    ///          provider for titles, colours and property order. Owned by the
    ///          session's registry, which outlives every snapshot.
    const IFormatProvider* provider = nullptr;

    /// \brief The tree diff. Present from Stage::TreeReady onward.
    std::shared_ptr<const DiffModel> treeDiff;

    /// \brief What went wrong, set when stage is Stage::Failed.
    ///
    /// \remarks Shown verbatim, so it says what failed and on which side.
    std::string message;

    /// \brief Wall-clock milliseconds spent producing this snapshot.
    double elapsedMillis = 0.0;

    /// \brief Reports whether both files have been read.
    ///
    /// \returns `true` when both sides are present.
    [[nodiscard]] bool hasSources() const noexcept { return left && right; }
};

/// \brief A single publication point for one immutable value.
///
/// \tparam T The published type, treated as immutable once published.
///
/// \remarks A worker builds a value, then publishes it as a shared pointer to
///          const. The frame loop takes one reference at the top of a frame and
///          renders from that for the whole frame. Nothing is mutated after
///          publication, so there is no tearing, no lock in the render path
///          beyond the pointer swap, and a value stays alive as long as any
///          frame still holds it.
///
///          Safe for one writer and any number of readers.
template <class T>
class SnapshotBox {
public:
    /// \brief Takes a reference to the current value.
    ///
    /// \returns The published value, or an empty pointer when nothing has been
    ///          published.
    ///
    /// \remarks Cheap enough to call once per frame: it copies a shared pointer
    ///          under a mutex rather than touching the payload.
    [[nodiscard]] std::shared_ptr<const T> get() const {
        std::lock_guard lock(mutex_);
        return value_;
    }

    /// \brief Returns how many times anything has been published.
    ///
    /// \returns A counter that only increases.
    ///
    /// \remarks Lets the frame loop tell a new value from the one it drew last
    ///          frame without comparing contents.
    [[nodiscard]] std::uint64_t version() const {
        std::lock_guard lock(mutex_);
        return version_;
    }

    /// \brief Publishes a new value.
    ///
    /// \param value The value to publish.
    void publish(std::shared_ptr<const T> value) {
        std::lock_guard lock(mutex_);
        value_ = std::move(value);
        ++version_;
    }

    /// \brief Drops the current value.
    void clear() {
        std::lock_guard lock(mutex_);
        value_.reset();
        ++version_;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const T> value_;
    std::uint64_t version_ = 0;
};

}  // namespace nmxd
