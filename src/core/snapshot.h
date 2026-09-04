#pragma once

// The handoff between the worker pool and the frame loop.
//
// A worker builds a snapshot, then publishes it as a shared_ptr to const. The
// frame loop takes one reference at the top of a frame and renders from that
// for the whole frame. Nothing is mutated after publication, so there is no
// tearing, no lock in the render path beyond the pointer swap, and a snapshot
// stays alive as long as any frame still holds it.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "core/source.h"
#include "core/textdiff.h"

namespace nmxd {

// How far the pipeline has got. The interface shows this, so it never presents
// partial results as complete.
enum class Stage {
    Idle,
    Loading,
    SourcesReady,  // both sides read; raw text can be shown
    TextReady,     // line diff computed; the text view is usable (M1)
    TreeReady,     // parsed and matched; the node view is usable (M3)
    Failed,
};

[[nodiscard]] const char* describe(Stage stage) noexcept;

// Everything the views may read. Later milestones add the matching, the change
// list, and the node layout; the publication mechanism does not change.
struct DiffSnapshot {
    Stage stage = Stage::Idle;

    std::shared_ptr<const SourceFile> left;
    std::shared_ptr<const SourceFile> right;

    // Present from Stage::TextReady onward.
    std::shared_ptr<const TextDiff> text;

    // Set when stage is Failed. Shown verbatim, so it says what went wrong and
    // which side it went wrong on.
    std::string message;

    // Wall-clock milliseconds spent producing this snapshot, for the budget
    // readout in the status bar.
    double elapsedMillis = 0.0;

    [[nodiscard]] bool hasSources() const noexcept { return left && right; }
};

// A single publication point. One writer at a time, any number of readers.
template <class T>
class SnapshotBox {
public:
    // Cheap enough to call once per frame; it copies a shared_ptr under a
    // mutex rather than touching the payload.
    [[nodiscard]] std::shared_ptr<const T> get() const {
        std::lock_guard lock(mutex_);
        return value_;
    }

    [[nodiscard]] std::uint64_t version() const {
        std::lock_guard lock(mutex_);
        return version_;
    }

    void publish(std::shared_ptr<const T> value) {
        std::lock_guard lock(mutex_);
        value_ = std::move(value);
        ++version_;
    }

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
