#pragma once

// Matching two trees.
//
// The result is a correspondence between nodes, not an edit script. Keeping it
// that way is what leaves three-way merge open: a merge is the composition of
// a base-to-left matching with a base-to-right matching, with nothing here
// needing to change.

#include <cstdint>
#include <stop_token>
#include <vector>

#include "core/provider.h"
#include "core/tree.h"

namespace nmxd {

class Matching {
public:
    void resize(std::size_t leftCount, std::size_t rightCount);

    // Pairs two nodes. Ignored if either is already paired, so an earlier and
    // more trustworthy pass always wins over a later heuristic.
    bool pair(NodeId left, NodeId right);

    [[nodiscard]] NodeId toRight(NodeId left) const {
        return left < leftToRight_.size() ? leftToRight_[left] : kInvalidNode;
    }
    [[nodiscard]] NodeId toLeft(NodeId right) const {
        return right < rightToLeft_.size() ? rightToLeft_[right] : kInvalidNode;
    }

    [[nodiscard]] bool leftMatched(NodeId left) const { return toRight(left) != kInvalidNode; }
    [[nodiscard]] bool rightMatched(NodeId right) const { return toLeft(right) != kInvalidNode; }

    [[nodiscard]] std::size_t pairCount() const noexcept { return pairs_; }

private:
    std::vector<NodeId> leftToRight_;
    std::vector<NodeId> rightToLeft_;
    std::size_t pairs_ = 0;
};

// Anything but Full is shown to the user. Under-reporting differences without
// saying so is the one failure a diff tool does not recover from.
enum class MatchQuality : std::uint8_t {
    Full,
    SimilarityTrimmed,  // the third pass was cut short by the size guard
};

[[nodiscard]] const char* describe(MatchQuality quality) noexcept;

struct MatchOptions {
    // Above this many nodes on a side, the similarity pass is skipped outright
    // rather than started and abandoned.
    std::uint32_t maxNodesForSimilarity = 400000;

    // Bounds the similarity pass, which is the only quadratic part. Counted in
    // candidate comparisons.
    std::uint64_t maxSimilaritySteps = 20'000'000;

    // Below this score a candidate pair is not considered the same node.
    double minSimilarity = 0.4;
};

struct MatchResult {
    Matching matching;
    MatchQuality quality = MatchQuality::Full;
    bool cancelled = false;

    std::uint32_t anchoredByIdentity = 0;
    std::uint32_t anchoredBySubtree = 0;
    std::uint32_t anchoredBySimilarity = 0;
};

// Runs the four passes, cheapest first. Each pass only sees what the previous
// ones left unmatched.
[[nodiscard]] MatchResult matchTrees(const Tree& left, const Tree& right,
                                     const IFormatProvider& provider, std::stop_token token = {},
                                     MatchOptions options = {});

}  // namespace nmxd
