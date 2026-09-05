#include "core/match.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>

#include "core/hash.h"

namespace nmxd {

namespace {

// A node's descendants occupy a contiguous run of indices, because trees are
// built depth-first in document order. That turns "is this node inside that
// subtree" into a range check, which the similarity score leans on heavily.
bool inSubtree(const Tree& tree, NodeId root, NodeId candidate) {
    const Node& node = tree.node(root);
    return candidate > root && candidate <= root + node.descendantCount;
}

std::vector<std::uint32_t> siblingIndices(const Tree& tree) {
    std::vector<std::uint32_t> indices(tree.size(), 0);
    for (const Node& node : tree.nodes()) {
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            indices[node.children[i]] = static_cast<std::uint32_t>(i);
        }
    }
    return indices;
}

// Sorted hashes of each property as a name=value pair, so two nodes can be
// compared as sets without allocating strings.
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

// How the three signals are weighed against each other. Kind equality is a
// prerequisite rather than a term, so these divide up what is left: what the
// node says about itself, what its contents say, and where it sits.
constexpr double kPropertyWeight = 0.30;
constexpr double kDescendantWeight = 0.45;
constexpr double kPositionWeight = 0.25;

double dice(std::size_t common, std::size_t sizeA, std::size_t sizeB) {
    const std::size_t total = sizeA + sizeB;
    if (total == 0) {
        return 1.0;  // two nodes with nothing to compare are not dissimilar
    }
    return (2.0 * static_cast<double>(common)) / static_cast<double>(total);
}

class Matcher {
public:
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

    MatchResult run(const std::stop_token& token) {
        anchorByIdentity(token);
        if (stopped(token)) {
            return std::move(result_);
        }

        anchorByIdenticalSubtrees(token);
        if (stopped(token)) {
            return std::move(result_);
        }

        matchBySimilarity(token);
        return std::move(result_);
    }

private:
    bool stopped(const std::stop_token& token) {
        if (token.stop_requested()) {
            result_.cancelled = true;
            return true;
        }
        return false;
    }

    // Pass 1. A strong identity key that appears exactly once on each side is
    // an anchor: those two nodes are the same node however far apart they have
    // moved. A key appearing more than once on a side identifies nothing, so it
    // is dropped rather than guessed at.
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

    // Pass 2. Identical subtrees, largest first. This is the pass that makes
    // the common case, a small edit in a big file, fast: everything unchanged
    // pairs up in one sweep and never reaches the expensive pass.
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

    // Two nodes with the same content hash have the same subtree, so their
    // descendants pair up without any further comparison.
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

    // Pass 3. Whatever is left, matched top down inside already-matched
    // parents. Restricting candidates to one parent pair at a time is what
    // keeps this from comparing every node against every other node.
    void matchBySimilarity(const std::stop_token& token) {
        if (left_.size() > options_.maxNodesForSimilarity ||
            right_.size() > options_.maxNodesForSimilarity) {
            result_.quality = MatchQuality::SimilarityTrimmed;
            return;
        }

        std::deque<std::pair<NodeId, NodeId>> work;
        if (left_.empty() || right_.empty()) {
            return;
        }

        // The two files are two versions of one document, so their roots are
        // the same node whenever they are the same kind of element. Without
        // this the whole document reads as a replacement the moment anything
        // inside it changes, because the root's hash covers everything below
        // it and so never survives an edit.
        const NodeId leftRoot = left_.root();
        const NodeId rightRoot = right_.root();
        if (!result_.matching.leftMatched(leftRoot) && !result_.matching.rightMatched(rightRoot) &&
            left_.node(leftRoot).kind == right_.node(rightRoot).kind) {
            if (result_.matching.pair(leftRoot, rightRoot)) {
                ++result_.anchoredBySimilarity;
            }
        }

        // Start from the root pair when there is one; otherwise every matched
        // pair is its own starting point.
        if (result_.matching.toRight(left_.root()) == right_.root()) {
            work.emplace_back(left_.root(), right_.root());
        } else {
            for (const Node& node : left_.nodes()) {
                const NodeId r = result_.matching.toRight(node.id);
                if (r != kInvalidNode) {
                    work.emplace_back(node.id, r);
                }
            }
        }

        std::uint64_t steps = 0;
        while (!work.empty()) {
            const auto [leftParent, rightParent] = work.front();
            work.pop_front();

            if (token.stop_requested()) {
                result_.cancelled = true;
                return;
            }

            const auto& leftChildren = left_.node(leftParent).children;
            const auto& rightChildren = right_.node(rightParent).children;

            std::vector<NodeId> unmatchedLeft;
            for (const NodeId c : leftChildren) {
                if (!result_.matching.leftMatched(c)) {
                    unmatchedLeft.push_back(c);
                }
            }
            std::vector<NodeId> unmatchedRight;
            for (const NodeId c : rightChildren) {
                if (!result_.matching.rightMatched(c)) {
                    unmatchedRight.push_back(c);
                }
            }

            steps += static_cast<std::uint64_t>(unmatchedLeft.size()) * unmatchedRight.size();
            if (steps > options_.maxSimilaritySteps) {
                result_.quality = MatchQuality::SimilarityTrimmed;
                return;
            }

            // Best pair first, so one strong match is not blocked by a weaker
            // one that happened to be considered earlier.
            struct Candidate {
                double score;
                NodeId left;
                NodeId right;
            };
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
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                if (a.score != b.score) {
                    return a.score > b.score;
                }
                return a.left != b.left ? a.left < b.left : a.right < b.right;
            });

            for (const auto& candidate : candidates) {
                if (result_.matching.leftMatched(candidate.left) ||
                    result_.matching.rightMatched(candidate.right)) {
                    continue;
                }
                if (result_.matching.pair(candidate.left, candidate.right)) {
                    ++result_.anchoredBySimilarity;
                }
            }

            // Descend into every matched child pair, including ones an earlier
            // pass established.
            for (const NodeId c : leftChildren) {
                const NodeId r = result_.matching.toRight(c);
                if (r != kInvalidNode) {
                    work.emplace_back(c, r);
                }
            }
        }
    }

    double similarity(NodeId leftId, NodeId rightId) const {
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

    // 1 when the two nodes sit at the same relative position under their
    // parents, falling to 0 at opposite ends.
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
