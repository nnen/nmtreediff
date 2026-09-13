#pragma once

/// \file
/// \brief What a provider is handed to turn a document into a tree.

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/builder.h"
#include "core/dom.h"

namespace nmxd {

/// \brief A failure inside a shaping job that has more to say than one line.
///
/// \remarks What the Lua bridge throws when a script raises: what() is the
///          first line, which lands in the report line and the card tooltip,
///          and detail is the whole of what the interpreter said, traceback
///          included, which goes to the log. A job that throws any other
///          exception is recorded with what() alone.
class ShapeError : public std::runtime_error {
public:
    /// \brief Builds the error.
    ///
    /// \param message The one line a listing wants.
    /// \param detail Everything, when there was more; empty otherwise.
    ShapeError(const std::string& message, std::string detail)
        : std::runtime_error(message), detail_(std::move(detail)) {}

    /// \brief Returns the whole message, or empty when what() was all of it.
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

private:
    std::string detail_;
};

/// \brief The document, the builder, and a queue for the walk.
///
/// \remarks The provider controls the walk completely. Nothing here stops one
///          from recursing if it insists, and a script that calls itself gets
///          whatever depth its own stack affords. What this offers is a way to
///          queue a call instead of making it, and a drain that runs the queue
///          until it is empty, checking the stop token between jobs.
///
///          The queue belongs here rather than to the builder. A provider that
///          would rather answer questions than hold a walk never uses it, and
///          the builder never knows it exists.
class ShapeContext {
public:
    /// \brief One unit of shaping work.
    using Job = std::function<void(ShapeContext&)>;

    /// \brief Prepares a pass over one document.
    ///
    /// \param dom The document to shape. Must outlive the context.
    /// \param out The builder to shape it into. Must outlive the context.
    /// \param token Checked between jobs; the drain stops when a stop is
    ///        requested.
    /// \param label What to call the document in a log line about a failed
    ///        job, usually the file's label; the builder's format name when
    ///        empty.
    ShapeContext(const Dom& dom, TreeBuilder& out, std::stop_token token = {},
                 std::string label = {});

    /// \brief Returns the document being shaped.
    [[nodiscard]] const Dom& dom() const noexcept { return *dom_; }

    /// \brief Returns the builder the tree goes into.
    [[nodiscard]] TreeBuilder& out() noexcept { return *out_; }

    /// \brief Returns the token the drain checks between jobs.
    ///
    /// \remarks For a shape that runs something of its own with a
    ///          cancellation hook, such as a script interpreter.
    [[nodiscard]] const std::stop_token& token() const noexcept { return token_; }

    /// \brief Queues a job to run after everything already queued.
    ///
    /// \param job What to run.
    /// \param element The element the job is about, if any. A failure inside
    ///        the job is recorded against its span.
    /// \param owner The handle the job builds under, if any. A failure is
    ///        recorded against its node.
    ///
    /// \remarks Appending drains breadth-first, which holds a whole level of
    ///          pending jobs at once. next() is the idiom for a walk.
    void later(Job job, DomId element = kInvalidDom, RefId owner = {});

    /// \brief Queues a job to run before everything already queued.
    ///
    /// \param job What to run.
    /// \param element The element the job is about, if any.
    /// \param owner The handle the job builds under, if any.
    ///
    /// \remarks Prepending drains depth-first, which holds about one path from
    ///          the root to a leaf at once. Because build order is free the two
    ///          drains produce identical trees, and this one costs less.
    ///
    ///          Everything one job queues with next() goes to the front as a
    ///          block, in the order it was queued, once that job finishes. A
    ///          job that queues one call per child therefore sees its children
    ///          run first to last, the same as with later().
    void next(Job job, DomId element = kInvalidDom, RefId owner = {});

    /// \brief Returns how many jobs are waiting.
    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }

    /// \brief Reports whether the stop token has been signalled.
    [[nodiscard]] bool cancelled() const noexcept { return token_.stop_requested(); }

    /// \brief Runs a job and then everything it and its successors queue.
    ///
    /// \param entry The provider's own shape function, run as the first job.
    ///
    /// \remarks Every job runs inside a catch. One that raises does not fail
    ///          the pass: whatever it built stays, whatever it never queued is
    ///          simply missing, and the error is recorded through
    ///          TreeBuilder::recordFailure() against the element and owner it
    ///          was queued with. The drain stops early only when the stop
    ///          token says so, and then discards what was left.
    void run(Job entry);

    /// \brief Runs whatever is queued until nothing is.
    void drain();

    /// \brief Keeps something alive for the rest of the pass.
    ///
    /// \param holder Whatever must outlive the drain.
    ///
    /// \remarks A script interpreter, say: the jobs a script queues are
    ///          closures inside it, so it cannot go away until the last of
    ///          them has run. Released when the context is.
    void keepAlive(std::shared_ptr<void> holder) { held_.push_back(std::move(holder)); }

private:
    /// \brief A job with what it was queued against.
    struct Queued {
        Job job;
        DomId element = kInvalidDom;
        RefId owner;
    };

    /// \brief Runs one job inside a catch.
    void runOne(Queued& queued);

    /// \brief Records a job's failure on the builder and writes it to the log.
    ///
    /// \param queued The job that failed.
    /// \param message The one line the report and the card carry.
    /// \param detail The whole message, or empty when the line was all of it.
    void recordFailure(const Queued& queued, const std::string& message,
                       const std::string& detail);

    const Dom* dom_;
    TreeBuilder* out_;
    std::stop_token token_;

    /// \brief What must outlive the drain.
    ///
    /// \remarks Declared before the queue so that it is destroyed after it:
    ///          a queued job may be a closure inside what is held here.
    std::vector<std::shared_ptr<void>> held_;

    std::deque<Queued> queue_;

    /// \brief What the running job has queued with next(), in order.
    std::vector<Queued> batch_;
    /// \brief Whether a job is running, and so whether next() batches.
    bool running_ = false;

    /// \brief What a log line about a failed job calls the document.
    std::string label_;
};

/// \brief Shapes a document into a tree with a provider's shape function.
///
/// \param dom The document to shape.
/// \param formatName The provider name the tree records.
/// \param shape The provider's shape function.
/// \param token Checked between jobs and by the builder.
/// \param label What a log line about a failed job calls the document,
///        usually the file's label; the format name when empty.
///
/// \returns The tree, or ParseError::Cancelled, or ParseError::ShapeFailed
///          when the pass ended with no root at all, because a tree of zero
///          nodes is not a partial result.
///
/// \remarks The whole of a shaping pass, in one place: a builder, a context,
///          the entry job, the drain, the failures, the spans nothing came
///          from, and finish(). What the default parse() does after read().
[[nodiscard]] Result<Tree, ParseError> shapeTree(const Dom& dom, std::string formatName,
                                                 const ShapeContext::Job& shape,
                                                 std::stop_token token = {},
                                                 std::string label = {});

/// \brief Copies the document into the builder one to one.
///
/// \param context The pass to copy within.
///
/// \remarks What a generic format means by shaping: every element a node of
///          the same kind, every property carried over, child order as the
///          source recorded it. One pass over the document in arena order,
///          which is pre-order, so a parent's handle always exists before its
///          children need it and nothing is queued or recursed.
void copyDocument(ShapeContext& context);

/// \brief Works out which source bytes no handle accounted for.
///
/// \param dom The document that was shaped.
/// \param out The builder that shaped it.
///
/// \returns Disjoint spans in ascending order: every element no handle was
///          made from, less every represented element inside it.
///
/// \remarks An element's span covers everything inside it, so a dropped
///          wrapper whose inner nodes were kept would otherwise highlight the
///          kept content too. A sort and a sweep, no recursion.
[[nodiscard]] std::vector<SourceSpan> unrepresentedSpans(const Dom& dom, const TreeBuilder& out);

}  // namespace nmxd
