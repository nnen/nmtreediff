#pragma once

/// \file
/// \brief Matching two trees: which node on the left is which node on the right.

#include <cstdint>
#include <stop_token>
#include <vector>

#include "core/provider.h"
#include "core/tree.h"

namespace nmxd {

/// \brief A correspondence between the nodes of two trees.
///
/// \remarks This is a matching, not an edit script, and keeping it that way is
///          what leaves three-way merge open: a merge is the composition of a
///          base-to-left matching with a base-to-right matching, with nothing
///          here needing to change.
class Matching {
public:
    /// \brief Sizes the matching for two trees and clears every pair.
    ///
    /// \param leftCount Number of nodes in the left tree.
    /// \param rightCount Number of nodes in the right tree.
    void resize(std::size_t leftCount, std::size_t rightCount);

    /// \brief Pairs two nodes.
    ///
    /// \param left The left node.
    /// \param right The right node.
    ///
    /// \returns `true` when the pair was recorded, `false` when either node was
    ///          already paired or out of range.
    ///
    /// \remarks Refusing to overwrite means an earlier and more trustworthy pass
    ///          always wins over a later heuristic.
    bool pair(NodeId left, NodeId right);

    /// \brief Finds the right-hand counterpart of a left node.
    ///
    /// \param left The left node.
    ///
    /// \returns The paired right node, or kInvalidNode when unmatched.
    [[nodiscard]] NodeId toRight(NodeId left) const {
        return left < leftToRight_.size() ? leftToRight_[left] : kInvalidNode;
    }

    /// \brief Finds the left-hand counterpart of a right node.
    ///
    /// \param right The right node.
    ///
    /// \returns The paired left node, or kInvalidNode when unmatched.
    [[nodiscard]] NodeId toLeft(NodeId right) const {
        return right < rightToLeft_.size() ? rightToLeft_[right] : kInvalidNode;
    }

    /// \brief Reports whether a left node is paired.
    ///
    /// \param left The left node.
    ///
    /// \returns `true` when the node has a counterpart.
    [[nodiscard]] bool leftMatched(NodeId left) const { return toRight(left) != kInvalidNode; }

    /// \brief Reports whether a right node is paired.
    ///
    /// \param right The right node.
    ///
    /// \returns `true` when the node has a counterpart.
    [[nodiscard]] bool rightMatched(NodeId right) const { return toLeft(right) != kInvalidNode; }

    /// \brief Returns how many pairs have been recorded.
    ///
    /// \returns The pair count.
    [[nodiscard]] std::size_t pairCount() const noexcept { return pairs_; }

private:
    std::vector<NodeId> leftToRight_;
    std::vector<NodeId> rightToLeft_;
    std::size_t pairs_ = 0;
};

/// \brief How complete a matching is.
///
/// \remarks Anything but MatchQuality::Full is shown to the user.
///          Under-reporting differences without saying so is the one failure a
///          diff tool does not recover from.
enum class MatchQuality : std::uint8_t {
    Full,               ///< Every pass ran to completion.
    SimilarityTrimmed,  ///< The third pass was cut short by the size guard.
};

/// \brief Converts a quality value into a phrase suitable for a message.
///
/// \param quality The value to describe.
///
/// \returns A short sentence fragment, never null.
[[nodiscard]] const char* describe(MatchQuality quality) noexcept;

/// \brief The guards that bound how much work matching may do.
struct MatchOptions {
    /// \brief Above this many nodes on a side, the similarity pass is skipped.
    ///
    /// \remarks Skipped outright rather than started and abandoned.
    std::uint32_t maxNodesForSimilarity = 400000;

    /// \brief Ceiling on candidate comparisons in the similarity pass.
    ///
    /// \remarks That pass is the only quadratic part, so this is what bounds the
    ///          worst case.
    std::uint64_t maxSimilaritySteps = 20'000'000;

    /// \brief Below this score a candidate pair is not considered the same node.
    double minSimilarity = 0.4;
};

/// \brief What matching produced, and how it got there.
struct MatchResult {
    /// \brief The correspondence between the two trees.
    Matching matching;
    /// \brief How complete the matching is.
    MatchQuality quality = MatchQuality::Full;
    /// \brief Whether matching stopped early because it was cancelled.
    bool cancelled = false;

    std::uint32_t anchoredByIdentity = 0;    ///< Pairs from the first pass.
    std::uint32_t anchoredBySubtree = 0;     ///< Pairs from the second pass.
    std::uint32_t anchoredBySimilarity = 0;  ///< Pairs from the third pass.
};

/// \brief Matches two trees against each other.
///
/// \param left The left, usually older, tree.
/// \param right The right, usually newer, tree.
/// \param provider The format provider, consulted for identity and child
///        ordering.
/// \param token Checked between and within passes.
/// \param options The guards bounding how much work to do.
///
/// \returns The matching, its quality, and a count of what each pass
///          contributed. MatchResult::cancelled is set when the token stopped
///          the work.
///
/// \remarks Runs three passes, cheapest first, and each only sees what the
///          previous ones left unmatched.
///
///          The first pass anchors nodes whose strong identity key appears
///          exactly once on each side, which is what makes a node with a stable
///          identifier follow its move however far it went. The second pairs
///          identical subtrees largest first, which is what makes the common
///          case of a small edit in a big file fast. The third pairs whatever
///          remains by similarity, inside already-matched parents only, which is
///          what keeps it from comparing every node against every other node.
///
///          Requires both trees to have been finalised and hashed.
[[nodiscard]] MatchResult matchTrees(const Tree& left, const Tree& right,
                                     const IFormatProvider& provider, std::stop_token token = {},
                                     MatchOptions options = {});

}  // namespace nmxd
