/// \file
/// \brief Implementation of the three matching passes.

#include "core/match.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>

#include "core/hash.h"

namespace nmxd {

namespace {

/// \brief Reports whether one node sits inside another's subtree.
///
/// \param tree The tree both nodes belong to.
/// \param root The subtree to test against.
/// \param candidate The node to look for.
///
/// \returns `true` when \p candidate is a strict descendant of \p root.
///
/// \remarks A node's descendants occupy a contiguous run of indices, because
///          trees are built depth-first in document order. That turns this into
///          a range check, which the similarity score leans on heavily.
bool inSubtree(const Tree& tree, NodeId root, NodeId candidate) {
    const Node& node = tree.node(root);
    return candidate > root && candidate <= root + node.descendantCount;
}

/// \brief Records each node's position among its siblings.
///
/// \param tree The tree to index.
///
/// \returns A position per node, indexed by NodeId. The root's entry is zero.
std::vector<std::uint32_t> siblingIndices(const Tree& tree) {
    std::vector<std::uint32_t> indices(tree.size(), 0);
    for (const Node& node : tree.nodes()) {
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            indices[node.children[i]] = static_cast<std::uint32_t>(i);
        }
    }
    return indices;
}

/// \brief Summarises a node's properties as a sorted list of hashes.
///
/// \param node The node to summarise.
///
/// \returns One hash per property, covering its name and value, sorted so two
///          nodes can be compared as sets without allocating strings.
std::vector<std::uint64_t> propertyFingerprints(const Node& node) {
    std::vector<std::uint64_t> out;
    out.reserve(node.properties.size());
    for (const auto& property : node.properties) {
        std::uint64_t h = hashBytes(property.name);
        h = hashBytes("=", h);
        h = hashBytes(property.value, h);
        out.push_back(h);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// \brief Counts values present in both sorted lists.
///
/// \param a The first sorted list.
/// \param b The second sorted list.
///
/// \returns How many values the two lists share.
std::size_t commonCount(const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b) {
    std::size_t common = 0;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            ++common;
            ++i;
            ++j;
        } else if (a[i] < b[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return common;
}

/// \brief Weight given to what a node says about itself.
///
/// \remarks Kind equality is a prerequisite rather than a term, so the three
///          weights divide up what is left: what the node says about itself,
///          what its contents say, and where it sits.
constexpr double kPropertyWeight = 0.30;

/// \brief Weight given to how much of a node's subtree already matches.
constexpr double kDescendantWeight = 0.45;

/// \brief Weight given to a node's position among its siblings.
constexpr double kPositionWeight = 0.25;

/// \brief The Dice coefficient of two sets.
///
/// \param common How many members the sets share.
/// \param sizeA Size of the first set.
/// \param sizeB Size of the second set.
///
/// \returns A value from 0 to 1. Two empty sets score 1, because two nodes with
///          nothing to compare are not dissimilar.
double dice(std::size_t common, std::size_t sizeA, std::size_t sizeB) {
    const std::size_t total = sizeA + sizeB;
    if (total == 0) {
        return 1.0;  // two nodes with nothing to compare are not dissimilar
    }
    return (2.0 * static_cast<double>(common)) / static_cast<double>(total);
}

/// \brief Runs the matching passes over one pair of trees.
class Matcher {
public:
    /// \brief Prepares a matcher for one pair of trees.
    ///
    /// \param left The left tree.
    /// \param right The right tree.
    /// \param provider The format provider.
    /// \param options The guards bounding how much work to do.
    Matcher(const Tree& left, const Tree& right, const IFormatProvider& provider,
            const MatchOptions& options)
        : left_(left),
          right_(right),
          provider_(provider),
          options_(options),
          leftSibling_(siblingIndices(left)),
          rightSibling_(siblingIndices(right)) {
        result_.matching.resize(left.size(), right.size());
    }

    /// \brief Runs every pass and returns the result.
    ///
    /// \param token Checked between and within passes.
    ///
    /// \returns The matching, its quality, and what each pass contributed.
    MatchResult run(const std::stop_token& token) {
        anchorByIdentity(token);
        if (stopped(token)) {
            return std::move(result_);
        }

        anchorByIdenticalSubtrees(token);
        if (stopped(token)) {
            return std::move(result_);
        }

        anchorRoots();
        matchBySimilarity(token);
        return std::move(result_);
    }

private:
    /// \brief Records cancellation and reports whether to stop.
    ///
    /// \param token The token to test.
    ///
    /// \returns `true` when a stop has been requested.
    bool stopped(const std::stop_token& token) {
        if (token.stop_requested()) {
            result_.cancelled = true;
            return true;
        }
        return false;
    }

    /// \brief First pass: pairs nodes by strong identity key.
    ///
    /// \param token Checked while collecting keys.
    ///
    /// \remarks A strong key appearing exactly once on each side is an anchor:
    ///          those two nodes are the same node however far apart they have
    ///          moved. A key appearing more than once on a side identifies
    ///          nothing, so it is dropped rather than guessed at.
    void anchorByIdentity(const std::stop_token& token) {
        std::unordered_map<std::string, NodeId> leftKeys;
        std::unordered_map<std::string, NodeId> rightKeys;

        const auto collect = [&](const Tree& tree, std::unordered_map<std::string, NodeId>& out) {
            for (const Node& node : tree.nodes()) {
                const IdentityKey key = provider_.identity(tree, node.id);
                if (!key.strong || key.value.empty()) {
                    continue;
                }
                const auto [it, inserted] = out.emplace(key.value, node.id);
                if (!inserted) {
                    it->second = kInvalidNode;  // ambiguous, so it anchors nothing
                }
            }
        };

        collect(left_, leftKeys);
        if (token.stop_requested()) {
            return;
        }
        collect(right_, rightKeys);

        for (const auto& [key, leftId] : leftKeys) {
            if (leftId == kInvalidNode) {
                continue;
            }
            const auto it = rightKeys.find(key);
            if (it == rightKeys.end() || it->second == kInvalidNode) {
                continue;
            }
            if (result_.matching.pair(leftId, it->second)) {
                ++result_.anchoredByIdentity;
            }
        }
    }

    /// \brief Second pass: pairs identical subtrees, largest first.
    ///
    /// \param token Checked while sweeping the tree.
    ///
    /// \remarks This is the pass that makes the common case, a small edit in a
    ///          big file, fast: everything unchanged pairs up in one sweep and
    ///          never reaches the expensive pass. Largest first, so a big
    ///          unchanged block claims its counterpart before one of its own
    ///          leaves does.
    void anchorByIdenticalSubtrees(const std::stop_token& token) {
        std::unordered_map<std::uint64_t, std::vector<NodeId>> rightByHash;
        for (const Node& node : right_.nodes()) {
            if (!result_.matching.rightMatched(node.id)) {
                rightByHash[node.contentHash].push_back(node.id);
            }
        }
        if (token.stop_requested()) {
            return;
        }

        // Largest subtrees first, so a big unchanged block claims its
        // counterpart before one of its own leaves does.
        std::vector<NodeId> order;
        order.reserve(left_.size());
        for (const Node& node : left_.nodes()) {
            if (!result_.matching.leftMatched(node.id)) {
                order.push_back(node.id);
            }
        }
        std::sort(order.begin(), order.end(), [this](NodeId a, NodeId b) {
            const auto da = left_.node(a).descendantCount;
            const auto db = left_.node(b).descendantCount;
            return da != db ? da > db : a < b;
        });

        for (const NodeId leftId : order) {
            if (result_.matching.leftMatched(leftId)) {
                continue;
            }
            const auto it = rightByHash.find(left_.node(leftId).contentHash);
            if (it == rightByHash.end()) {
                continue;
            }
            for (const NodeId rightId : it->second) {
                if (result_.matching.rightMatched(rightId)) {
                    continue;
                }
                if (result_.matching.pair(leftId, rightId)) {
                    ++result_.anchoredBySubtree;
                    pairIdenticalSubtree(leftId, rightId);
                }
                break;
            }
            if (token.stop_requested()) {
                return;
            }
        }
    }

    /// \brief Pairs the descendants of two nodes known to be identical.
    ///
    /// \param leftId The left node.
    /// \param rightId The right node, whose content hash equals the left one's.
    ///
    /// \remarks Two nodes with the same content hash have the same subtree, so
    ///          their descendants pair up without any further comparison.
    ///          Ordered children pair by position; unordered children pair by
    ///          hash, because their position carries nothing.
    void pairIdenticalSubtree(NodeId leftId, NodeId rightId) {
        const Node& leftNode = left_.node(leftId);
        const Node& rightNode = right_.node(rightId);
        if (leftNode.children.size() != rightNode.children.size()) {
            return;  // a hash collision; leave the descendants to later passes
        }

        if (provider_.childrenOrdered(left_, leftId)) {
            for (std::size_t i = 0; i < leftNode.children.size(); ++i) {
                const NodeId l = leftNode.children[i];
                const NodeId r = rightNode.children[i];
                if (left_.node(l).contentHash == right_.node(r).contentHash &&
                    result_.matching.pair(l, r)) {
                    ++result_.anchoredBySubtree;
                    pairIdenticalSubtree(l, r);
                }
            }
            return;
        }

        // Unordered children hash as a set, so position means nothing and the
        // pairing has to go by hash.
        std::vector<NodeId> available(rightNode.children.begin(), rightNode.children.end());
        for (const NodeId l : leftNode.children) {
            const auto hash = left_.node(l).contentHash;
            const auto it = std::find_if(available.begin(), available.end(), [&](NodeId r) {
                return right_.node(r).contentHash == hash;
            });
            if (it == available.end()) {
                continue;
            }
            if (result_.matching.pair(l, *it)) {
                ++result_.anchoredBySubtree;
                pairIdenticalSubtree(l, *it);
            }
            available.erase(it);
        }
    }

    /// \brief Pairs the two roots when they are the same kind of element.
    ///
    /// \remarks Runs outside the size guard, because it is constant work and
    ///          skipping it is catastrophic: a root's hash covers the whole
    ///          document, so it never survives an edit, and an unpaired root
    ///          makes the classifier report the entire file as deleted and
    ///          rewritten. The two files are two versions of one document, so
    ///          their roots are the same node whenever they are the same kind.
    void anchorRoots() {
        if (left_.empty() || right_.empty()) {
            return;
        }
        const NodeId leftRoot = left_.root();
        const NodeId rightRoot = right_.root();
        if (result_.matching.leftMatched(leftRoot) || result_.matching.rightMatched(rightRoot)) {
            return;
        }
        if (left_.node(leftRoot).kind != right_.node(rightRoot).kind) {
            return;
        }
        if (result_.matching.pair(leftRoot, rightRoot)) {
            ++result_.anchoredBySimilarity;
        }
    }

    /// \brief Third pass: pairs what remains, by similarity, top down.
    ///
    /// \param token Checked once per parent pair.
    ///
    /// \remarks Restricting candidates to one parent pair at a time is what
    ///          keeps this from comparing every node against every other node.
    ///          Sets MatchQuality::SimilarityTrimmed and returns when either
    ///          guard runs out.
    void matchBySimilarity(const std::stop_token& token) {
        if (left_.size() > options_.maxNodesForSimilarity ||
            right_.size() > options_.maxNodesForSimilarity) {
            result_.quality = MatchQuality::SimilarityTrimmed;
            return;
        }
        if (left_.empty() || right_.empty()) {
            return;
        }

        steps_ = 0;
        std::deque<std::pair<NodeId, NodeId>> work = startingPairs();

        while (!work.empty()) {
            const auto [leftParent, rightParent] = work.front();
            work.pop_front();

            if (token.stop_requested()) {
                result_.cancelled = true;
                return;
            }
            if (!matchChildrenOf(leftParent, rightParent)) {
                return;
            }
            enqueueMatchedChildren(leftParent, work);
        }
    }

    /// \brief One possible pairing and how good it looks.
    struct Candidate {
        double score;   ///< How alike the two nodes are, from zero to one.
        NodeId left;    ///< The candidate in the left tree.
        NodeId right;   ///< The candidate in the right tree.
    };

    /// \brief Chooses where the top-down walk begins.
    ///
    /// \returns The parent pairs to start from.
    ///
    /// \remarks The root pair when there is one. Otherwise every pair an
    ///          earlier pass established is its own starting point, because a
    ///          document whose root changed still has matched subtrees inside
    ///          it and abandoning them would throw away real work.
    [[nodiscard]] std::deque<std::pair<NodeId, NodeId>> startingPairs() const {
        std::deque<std::pair<NodeId, NodeId>> work;
        if (result_.matching.toRight(left_.root()) == right_.root()) {
            work.emplace_back(left_.root(), right_.root());
            return work;
        }
        for (const Node& node : left_.nodes()) {
            const NodeId partner = result_.matching.toRight(node.id);
            if (partner != kInvalidNode) {
                work.emplace_back(node.id, partner);
            }
        }
        return work;
    }

    /// \brief Collects the left children that nothing has claimed.
    ///
    /// \param parent The parent whose children to look at.
    ///
    /// \returns The unmatched children, in document order.
    ///
    /// \remarks Written out once per side rather than taking a flag. The two
    ///          are seven lines each and say which side they mean in their name,
    ///          which a boolean argument at the call site would not.
    [[nodiscard]] std::vector<NodeId> unmatchedLeftChildren(NodeId parent) const {
        std::vector<NodeId> unmatched;
        for (const NodeId child : left_.node(parent).children) {
            if (!result_.matching.leftMatched(child)) {
                unmatched.push_back(child);
            }
        }
        return unmatched;
    }

    /// \brief Collects the right children that nothing has claimed.
    ///
    /// \param parent The parent whose children to look at.
    ///
    /// \returns The unmatched children, in document order.
    [[nodiscard]] std::vector<NodeId> unmatchedRightChildren(NodeId parent) const {
        std::vector<NodeId> unmatched;
        for (const NodeId child : right_.node(parent).children) {
            if (!result_.matching.rightMatched(child)) {
                unmatched.push_back(child);
            }
        }
        return unmatched;
    }

    /// \brief Scores every unclaimed pairing under one matched parent pair.
    ///
    /// \param unmatchedLeft Candidates from the left tree.
    /// \param unmatchedRight Candidates from the right tree.
    ///
    /// \returns The pairings worth considering, best first.
    ///
    /// \remarks Sorted so that one strong match is never blocked by a weaker
    ///          one that happened to be considered first. Ties break on node id
    ///          so that a run produces the same answer every time.
    [[nodiscard]] std::vector<Candidate> scoreCandidates(
        const std::vector<NodeId>& unmatchedLeft,
        const std::vector<NodeId>& unmatchedRight) {
        std::vector<Candidate> candidates;
        candidates.reserve(unmatchedLeft.size() * unmatchedRight.size());
        for (const NodeId l : unmatchedLeft) {
            for (const NodeId r : unmatchedRight) {
                const double score = similarity(l, r);
                if (score >= options_.minSimilarity) {
                    candidates.push_back(Candidate{score, l, r});
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.score != b.score) {
                          return a.score > b.score;
                      }
                      return a.left != b.left ? a.left < b.left : a.right < b.right;
                  });
        return candidates;
    }

    /// \brief Pairs up the unmatched children of one matched parent pair.
    ///
    /// \param leftParent The parent in the left tree.
    /// \param rightParent Its counterpart in the right tree.
    ///
    /// \returns `false` when the step budget ran out, which ends the pass.
    ///
    /// \remarks Comparing only candidates under one parent pair is what keeps
    ///          this from comparing every node against every other node.
    [[nodiscard]] bool matchChildrenOf(NodeId leftParent, NodeId rightParent) {
        const std::vector<NodeId> unmatchedLeft = unmatchedLeftChildren(leftParent);
        const std::vector<NodeId> unmatchedRight = unmatchedRightChildren(rightParent);

        steps_ += static_cast<std::uint64_t>(unmatchedLeft.size()) * unmatchedRight.size();
        if (steps_ > options_.maxSimilaritySteps) {
            result_.quality = MatchQuality::SimilarityTrimmed;
            return false;
        }

        for (const Candidate& candidate : scoreCandidates(unmatchedLeft, unmatchedRight)) {
            if (result_.matching.leftMatched(candidate.left) ||
                result_.matching.rightMatched(candidate.right)) {
                continue;
            }
            if (result_.matching.pair(candidate.left, candidate.right)) {
                ++result_.anchoredBySimilarity;
            }
        }
        return true;
    }

    /// \brief Queues every matched child of one node for its own turn.
    ///
    /// \param leftParent The parent whose children to descend into.
    /// \param work The queue to add to.
    ///
    /// \remarks Includes pairs an earlier pass established, because their
    ///          children may still be unmatched.
    void enqueueMatchedChildren(NodeId leftParent,
                                std::deque<std::pair<NodeId, NodeId>>& work) const {
        for (const NodeId child : left_.node(leftParent).children) {
            const NodeId partner = result_.matching.toRight(child);
            if (partner != kInvalidNode) {
                work.emplace_back(child, partner);
            }
        }
    }

    /// \brief Scores how likely two nodes are to be the same node.
    ///
    /// \param leftId The left candidate.
    /// \param rightId The right candidate.
    ///
    /// \returns A value from 0 to 1, where 0 means the two cannot be the same
    ///          node.
    ///
    /// \remarks Nodes of different kinds score zero: comparing an element to one
    ///          of a different name is almost never right, and letting it
    ///          through produces confident nonsense.
    double similarity(NodeId leftId, NodeId rightId) {
        const Node& l = left_.node(leftId);
        const Node& r = right_.node(rightId);

        // Comparing an element to one of a different name is almost never
        // right, and letting it through produces confident nonsense.
        if (l.kind != r.kind) {
            return 0.0;
        }

        const auto leftProperties = propertyFingerprints(l);
        const auto rightProperties = propertyFingerprints(r);
        const double propertyScore = dice(commonCount(leftProperties, rightProperties),
                                          leftProperties.size(), rightProperties.size());

        // Position among siblings. Kind equality is already a prerequisite, so
        // a same-kind node sitting in the same place is good evidence on its
        // own. Without this term a subtree that moved out of a container drags
        // the container's identity along with it.
        const double positionScore = positionSimilarity(leftId, rightId);

        // How much of each subtree is already paired with the other's. This is
        // what carries a renamed container: its contents already match.
        // Scanning a subtree is the expensive part of scoring a candidate, so
        // it is charged to the same budget. Counting only candidate pairs would
        // let a handful of comparisons near the root of a large tree cost
        // millions of operations unbilled.
        steps_ += l.descendantCount + r.descendantCount;

        std::size_t common = 0;
        std::size_t leftMatchedDescendants = 0;
        for (NodeId id = leftId + 1; id <= leftId + l.descendantCount; ++id) {
            const NodeId mapped = result_.matching.toRight(id);
            if (mapped == kInvalidNode) {
                continue;
            }
            ++leftMatchedDescendants;
            if (inSubtree(right_, rightId, mapped)) {
                ++common;
            }
        }
        std::size_t rightMatchedDescendants = 0;
        for (NodeId id = rightId + 1; id <= rightId + r.descendantCount; ++id) {
            if (result_.matching.toLeft(id) != kInvalidNode) {
                ++rightMatchedDescendants;
            }
        }

        // When neither subtree contains a single matched node, the descendant
        // term is not weak evidence, it is no evidence: nothing below either
        // node has been decided yet. Scoring it as zero would punish a pair for
        // being considered before its own children were, which is how a
        // reordered node with identical attributes ends up read as a deletion
        // and an unrelated addition.
        const bool descendantsInformative =
            leftMatchedDescendants > 0 || rightMatchedDescendants > 0;
        if (!descendantsInformative) {
            return (kPropertyWeight * propertyScore + kPositionWeight * positionScore) /
                   (kPropertyWeight + kPositionWeight);
        }

        const double descendantScore = dice(common, l.descendantCount, r.descendantCount);

        return kPropertyWeight * propertyScore + kDescendantWeight * descendantScore +
               kPositionWeight * positionScore;
    }

    /// \brief Scores how close two nodes sit to the same relative position.
    ///
    /// \param leftId The left candidate.
    /// \param rightId The right candidate.
    ///
    /// \returns 1 when both sit at the same relative position under their
    ///          parents, falling to 0 at opposite ends.
    double positionSimilarity(NodeId leftId, NodeId rightId) const {
        const NodeId leftParent = left_.node(leftId).parent;
        const NodeId rightParent = right_.node(rightId).parent;
        if (leftParent == kInvalidNode || rightParent == kInvalidNode) {
            return 1.0;
        }

        const auto leftCount = left_.node(leftParent).children.size();
        const auto rightCount = right_.node(rightParent).children.size();
        const double leftSpan = leftCount > 1 ? static_cast<double>(leftCount - 1) : 1.0;
        const double rightSpan = rightCount > 1 ? static_cast<double>(rightCount - 1) : 1.0;

        const double leftAt = static_cast<double>(leftSibling_[leftId]) / leftSpan;
        const double rightAt = static_cast<double>(rightSibling_[rightId]) / rightSpan;
        return 1.0 - std::min(1.0, std::abs(leftAt - rightAt));
    }

    const Tree& left_;
    const Tree& right_;
    const IFormatProvider& provider_;
    MatchOptions options_;
    std::vector<std::uint32_t> leftSibling_;
    std::vector<std::uint32_t> rightSibling_;
    std::uint64_t steps_ = 0;
    MatchResult result_;
};

}  // namespace

void Matching::resize(std::size_t leftCount, std::size_t rightCount) {
    leftToRight_.assign(leftCount, kInvalidNode);
    rightToLeft_.assign(rightCount, kInvalidNode);
    pairs_ = 0;
}

bool Matching::pair(NodeId left, NodeId right) {
    if (left >= leftToRight_.size() || right >= rightToLeft_.size()) {
        return false;
    }
    if (leftToRight_[left] != kInvalidNode || rightToLeft_[right] != kInvalidNode) {
        return false;
    }
    leftToRight_[left] = right;
    rightToLeft_[right] = left;
    ++pairs_;
    return true;
}

const char* describe(MatchQuality quality) noexcept {
    switch (quality) {
        case MatchQuality::Full:
            return "complete";
        case MatchQuality::SimilarityTrimmed:
            return "similarity matching was trimmed by the size guard";
    }
    return "unknown";
}

MatchResult matchTrees(const Tree& left, const Tree& right, const IFormatProvider& provider,
                       std::stop_token token, MatchOptions options) {
    Matcher matcher(left, right, provider, options);
    return matcher.run(token);
}

}  // namespace nmxd
