/// \file
/// \brief Implementation of the shaping context and its drain.

#include "core/shape.h"

#include "core/log.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace nmxd {

namespace {

/// \brief One edge of an element's span, for the sweep.
struct SpanEvent {
    std::uint32_t position = 0;    ///< Byte offset of the edge.
    DomId element = kInvalidDom;   ///< The element whose span it is.
    bool opens = false;            ///< `true` at the start of the span.
};

/// \brief Orders events by position, with a closing edge before an opening
///        one at the same position so that adjacent elements do not overlap.
///
/// \param a One event.
/// \param b The other.
///
/// \returns `true` when \p a comes first in the sweep.
///
/// \remarks Two elements opening at one position open outermost first, and
///          two closing there close innermost first. Arena order is pre-order,
///          so a lower id is the outer element.
bool operator<(const SpanEvent& a, const SpanEvent& b) noexcept {
    if (a.position != b.position) {
        return a.position < b.position;
    }
    if (a.opens != b.opens) {
        return !a.opens;
    }
    return a.opens ? a.element < b.element : a.element > b.element;
}

}  // namespace

ShapeContext::ShapeContext(const Dom& dom, TreeBuilder& out, std::stop_token token,
                           std::string label)
    : dom_(&dom), out_(&out), token_(std::move(token)), label_(std::move(label)) {}

void ShapeContext::later(Job job, DomId element, RefId owner) {
    queue_.push_back(Queued{std::move(job), element, owner});
}

void ShapeContext::next(Job job, DomId element, RefId owner) {
    // While a job runs, what it queues with next() is held back and put at
    // the front as one block once it finishes, in the order it was queued.
    // Prepending one at a time would run a parent's children last to first,
    // and since sibling order is call order that would reverse every list.
    if (running_) {
        batch_.push_back(Queued{std::move(job), element, owner});
    } else {
        queue_.push_front(Queued{std::move(job), element, owner});
    }
}

void ShapeContext::run(Job entry) {
    next(std::move(entry));
    drain();
}

void ShapeContext::drain() {
    while (!queue_.empty()) {
        if (token_.stop_requested()) {
            queue_.clear();
            batch_.clear();
            return;
        }
        // Taken off the queue before it runs, so a job that queues more work
        // never sees itself, and so the deque can grow while it runs.
        Queued queued = std::move(queue_.front());
        queue_.pop_front();
        runOne(queued);
    }
}

void ShapeContext::recordFailure(const Queued& queued, const std::string& message,
                                 const std::string& detail) {
    out_->recordFailure(queued.owner, dom_->at(queued.element).span(), message, detail);

    // The whole of it goes to the log once, here, where it happened. The
    // report line and the card keep the one-line message; a person fixing a
    // script wants the traceback, and this is the only place it is written.
    const std::string& document = label_.empty() ? out_->formatName() : label_;
    logErr("nmxmldiff: " + document + ": shaping failed: " + (detail.empty() ? message : detail));
}

void ShapeContext::runOne(Queued& queued) {
    running_ = true;
    try {
        queued.job(*this);
    } catch (const ShapeError& error) {
        recordFailure(queued, error.what(), error.detail());
    } catch (const std::exception& error) {
        recordFailure(queued, error.what(), {});
    } catch (...) {
        recordFailure(queued, "the shaping job raised something that is not an error", {});
    }
    running_ = false;

    // The block this job queued for the front goes in ahead of everything
    // else, first queued first.
    for (std::size_t i = batch_.size(); i-- > 0;) {
        queue_.push_front(std::move(batch_[i]));
    }
    batch_.clear();
}

Result<Tree, ParseError> shapeTree(const Dom& dom, std::string formatName,
                                   const ShapeContext::Job& shape, std::stop_token token,
                                   std::string label) {
    TreeBuilder out(std::move(formatName), token);
    ShapeContext context(dom, out, token, std::move(label));
    context.run(shape);
    if (token.stop_requested()) {
        return fail(ParseError::Cancelled);
    }

    // Nothing built means nothing to show. A failure is the likely reason
    // and the tree would have carried its message; without a tree the error
    // says so instead.
    if (out.nodeCount() == 0) {
        return fail(out.failureCount() > 0 ? ParseError::ShapeFailed : ParseError::Empty);
    }

    out.setUnrepresented(unrepresentedSpans(dom, out));
    return out.finish();
}

void copyDocument(ShapeContext& context) {
    const Dom& dom = context.dom();
    TreeBuilder& out = context.out();
    if (dom.size() == 0) {
        return;
    }

    // Arena order is pre-order, so by the time an element comes up its
    // parent's handle is already in the table.
    std::vector<RefId> made(dom.size());
    for (DomId id = 0; id < dom.size(); ++id) {
        const DomNode element = dom.at(id);
        const DomNode parent = element.parent();
        Ref ref;
        if (parent.valid()) {
            ref = out.at(made[parent.id()]).child(element);
        } else {
            ref = out.root(element.name(), element.span());
            ref.setSource(element.id());
        }
        for (const DomProperty property : element.properties()) {
            ref.property(property);
        }
        ref.setChildrenOrdered(dom.tree().node(id).childrenOrdered);
        made[id] = ref.id();
    }
}

std::vector<SourceSpan> unrepresentedSpans(const Dom& dom, const TreeBuilder& out) {
    // A byte belongs to the innermost element whose span holds it, so the
    // question per byte is whether that element was represented. Sweep the
    // span edges in order with a stack of open elements; the top of the stack
    // is the innermost, and each stretch between two edges takes its answer.
    std::vector<SpanEvent> events;
    events.reserve(dom.size() * 2);
    for (DomId id = 0; id < dom.size(); ++id) {
        const SourceSpan span = dom.at(id).span();
        if (span.end <= span.begin) {
            continue;
        }
        events.push_back(SpanEvent{span.begin, id, true});
        events.push_back(SpanEvent{span.end, id, false});
    }
    std::sort(events.begin(), events.end());

    std::vector<SourceSpan> spans;
    std::vector<DomId> open;
    std::uint32_t position = 0;
    for (const SpanEvent& event : events) {
        // The stretch up to this edge belongs to whatever is open now.
        if (!open.empty() && event.position > position && !out.represents(open.back())) {
            if (!spans.empty() && spans.back().end == position) {
                spans.back().end = event.position;
            } else {
                spans.push_back(SourceSpan{position, event.position});
            }
        }
        position = event.position;

        if (event.opens) {
            open.push_back(event.element);
        } else {
            // Close down to and including this element. A span that overran
            // its parent's closes the parent as well rather than leaving it
            // open forever.
            while (!open.empty()) {
                const DomId closed = open.back();
                open.pop_back();
                if (closed == event.element) {
                    break;
                }
            }
        }
    }
    return spans;
}

}  // namespace nmxd
