#pragma once

// Turning a matching into something the views can read.
//
// The matching says which nodes correspond. This says what happened to each of
// them, and in what order a reader should be walked through the changes.

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "core/match.h"
#include "core/provider.h"
#include "core/tree.h"

namespace nmxd {

enum class Side : std::uint8_t { Left, Right };

enum class NodeStatus : std::uint8_t {
    Unchanged,
    Added,
    Deleted,
    Modified,
    Moved,
};

[[nodiscard]] const char* describe(NodeStatus status) noexcept;

struct Change {
    NodeStatus status = NodeStatus::Unchanged;

    // True alongside a Modified status when the node also changed parent or
    // sibling position, so a move that also edited something is not reported
    // as only one of the two.
    bool moved = false;

    NodeId left = kInvalidNode;
    NodeId right = kInvalidNode;

    // Named for a Modified node. Order follows the provider's property ranking.
    std::vector<std::string> changedProperties;
};

struct DiffModel {
    Matching matching;
    MatchQuality quality = MatchQuality::Full;

    // Walked as a union of both trees, so the order a reader is taken through
    // the changes follows the document rather than the arena.
    std::vector<Change> changes;

    std::vector<NodeStatus> leftStatus;
    std::vector<NodeStatus> rightStatus;

    std::uint32_t added = 0;
    std::uint32_t deleted = 0;
    std::uint32_t modified = 0;
    std::uint32_t moved = 0;
    std::uint32_t unchanged = 0;

    double elapsedMillis = 0.0;
    bool cancelled = false;

    [[nodiscard]] NodeStatus statusOf(Side side, NodeId id) const;
    [[nodiscard]] bool identical() const noexcept {
        return added == 0 && deleted == 0 && modified == 0 && moved == 0;
    }
    [[nodiscard]] std::uint32_t changedNodes() const noexcept {
        return added + deleted + modified + moved;
    }
};

// Runs the matcher and then classifies the result.
[[nodiscard]] DiffModel diffTrees(const Tree& left, const Tree& right,
                                  const IFormatProvider& provider, std::stop_token token = {},
                                  MatchOptions options = {});

// Classification alone, for tests that supply their own matching.
[[nodiscard]] DiffModel classify(const Tree& left, const Tree& right,
                                 const IFormatProvider& provider, MatchResult match);

// A stable textual form of the change list, used by the golden-file tests and
// by the headless report. Paths name a node by its kind and sibling position,
// so they survive edits elsewhere in the document.
[[nodiscard]] std::string serializeChanges(const Tree& left, const Tree& right,
                                           const DiffModel& model);

// The path a serialized change uses, exposed for tests.
[[nodiscard]] std::string nodePath(const Tree& tree, NodeId id);

}  // namespace nmxd
