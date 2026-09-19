/// \file
/// \brief Implementation of change classification and the change list.

#include "core/diff.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "core/hash.h"

namespace nmtreediff {

bool propertiesDiffer(const Property& left, const Property& right) {
    return hashProperty(left) != hashProperty(right);
}

const Property* counterpartByOccurrence(const std::vector<Property>& own, std::size_t index,
                                        const std::vector<Property>& other) noexcept {
    const std::string& name = own[index].name;

    // Which of the same-named properties this one is, counting from the top.
    std::size_t occurrence = 0;
    for (std::size_t i = 0; i < index; ++i) {
        if (own[i].name == name) {
            ++occurrence;
        }
    }

    // The same occurrence on the other side, or none when it has fewer.
    for (const Property& candidate : other) {
        if (candidate.name != name) {
            continue;
        }
        if (occurrence == 0) {
            return &candidate;
        }
        --occurrence;
    }
    return nullptr;
}

namespace {

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

/// \brief A node's properties grouped by name, each group a sorted list of
///        property hashes.
///
/// \remarks Names may repeat, so a name maps to a bag rather than to one
///          property. Two bags are equal when they hold the same hashes the
///          same number of times, which is what makes a second `cooldown`
///          with a different value count as a change rather than hide behind
///          the first.
using PropertyBags = std::unordered_map<std::string_view, std::vector<std::uint64_t>>;

/// \brief Groups a node's properties by name.
///
/// \param node The node whose properties to group.
///
/// \returns One sorted bag of hashes per distinct name. The keys point into
///          the node's own strings and are valid as long as it is.
[[nodiscard]] PropertyBags bagProperties(const Node& node) {
    PropertyBags bags;
    bags.reserve(node.properties.size());
    for (const Property& property : node.properties) {
        bags[property.name].push_back(hashProperty(property));
    }
    for (auto& [name, hashes] : bags) {
        std::sort(hashes.begin(), hashes.end());
    }
    return bags;
}

/// \brief Lists the property names that differ between two matched nodes.
///
/// \param provider The provider whose ranking orders the result.
/// \param leftTree The left tree.
/// \param leftId The left node.
/// \param rightTree The right tree.
/// \param rightId The right node.
///
/// \returns The differing names, ordered the way the provider would display
///          them so the list reads like the node card.
///
/// \remarks Properties are compared as a multiset, so a reordered attribute
///          list yields nothing and a repeated name is counted as many times
///          as it appears. A name is listed once however many of its
///          properties changed: the change list names the outermost property
///          that differs, and the details panel is where the parts are.
std::vector<std::string> changedPropertyNames(const IFormatProvider& provider, const Tree& leftTree,
                                              NodeId leftId, const Tree& rightTree,
                                              NodeId rightId) {
    const PropertyBags leftBags = bagProperties(leftTree.node(leftId));
    const PropertyBags rightBags = bagProperties(rightTree.node(rightId));

    std::vector<std::string> changed;
    for (const auto& [name, hashes] : leftBags) {
        const auto it = rightBags.find(name);
        if (it == rightBags.end() || it->second != hashes) {
            changed.emplace_back(name);
        }
    }
    for (const auto& [name, hashes] : rightBags) {
        if (!leftBags.contains(name)) {
            changed.emplace_back(name);
        }
    }

    std::sort(changed.begin(), changed.end());

    // Reported in the order the provider would display them, so the list reads
    // the same way the node card does.
    std::stable_sort(changed.begin(), changed.end(),
                     [&](const std::string& a, const std::string& b) {
                         return provider.propertyRank(rightTree, rightId, a) <
                                provider.propertyRank(rightTree, rightId, b);
                     });
    return changed;
}

/// \brief Turns a matching into per-node statuses and an ordered change list.
class Classifier {
public:
    /// \brief Prepares a classifier for one matched pair of trees.
    ///
    /// \param left The left tree.
    /// \param right The right tree.
    /// \param provider The format provider.
    /// \param match The matching to classify, consumed by this call.
    Classifier(const Tree& left, const Tree& right, const IFormatProvider& provider,
               MatchResult match)
        : left_(left),
          right_(right),
          provider_(provider),
          leftSibling_(siblingIndices(left)),
          rightSibling_(siblingIndices(right)) {
        model_.matching = std::move(match.matching);
        model_.quality = match.quality;
        model_.trimmedParents = match.trimmedParents;
        model_.cancelled = match.cancelled;
        model_.leftStatus.assign(left.size(), NodeStatus::Unchanged);
        model_.rightStatus.assign(right.size(), NodeStatus::Unchanged);
        model_.leftChangeIndex.assign(left.size(), kNoChange);
        model_.rightChangeIndex.assign(right.size(), kNoChange);
    }

    /// \brief Classifies the matching.
    ///
    /// \returns The finished model, with statuses, counts and the change list.
    DiffModel run() {
        markReorderedChildren();
        if (left_.empty() && right_.empty()) {
            return std::move(model_);
        }
        if (left_.empty()) {
            addSubtree(right_.root());
            return finish();
        }
        if (right_.empty()) {
            deleteSubtree(left_.root());
            return finish();
        }

        const NodeId rootMatch = model_.matching.toRight(left_.root());
        if (rootMatch != right_.root()) {
            // The two documents do not share a root, so nothing below can
            // correspond either. Reporting that plainly beats pairing two
            // unrelated roots and describing the result as edits.
            deleteSubtree(left_.root());
            addSubtree(right_.root());
            return finish();
        }

        walkPair(left_.root(), right_.root());
        return finish();
    }

private:
    /// \brief Counts unchanged nodes and hands the model over.
    ///
    /// \returns The finished model.
    DiffModel finish() {
        for (std::size_t i = 0; i < model_.leftStatus.size(); ++i) {
            if (model_.leftStatus[i] == NodeStatus::Unchanged) {
                ++model_.unchanged;
            }
        }
        return std::move(model_);
    }

    /// \brief Records one change and updates the per-node statuses.
    ///
    /// \param change The change to record. An unchanged status is ignored.
    void emit(Change change) {
        switch (change.status) {
            case NodeStatus::Added:
                ++model_.added;
                break;
            case NodeStatus::Deleted:
                ++model_.deleted;
                break;
            case NodeStatus::Modified:
                ++model_.modified;
                break;
            case NodeStatus::Moved:
                ++model_.moved;
                break;
            case NodeStatus::Unchanged:
                return;
        }
        // Recorded before the move, because the index is where the change
        // is about to land.
        const auto at = static_cast<std::uint32_t>(model_.changes.size());
        if (change.left != kInvalidNode) {
            model_.leftStatus[change.left] = change.status;
            model_.leftChangeIndex[change.left] = at;
        }
        if (change.right != kInvalidNode) {
            model_.rightStatus[change.right] = change.status;
            model_.rightChangeIndex[change.right] = at;
        }
        model_.changes.push_back(std::move(change));
    }

    /// \brief Reports a whole right-hand subtree as added.
    ///
    /// \param rightId The root of the subtree.
    ///
    /// \remarks Walks with a stack of its own rather than the call stack, as
    ///          every walk in this class does: a document is as deep as
    ///          whatever wrote it, and the call stack is not. Children go on in
    ///          reverse so they come off in document order, which keeps the
    ///          change list in the order a reader would read the file.
    void addSubtree(NodeId rightId) {
        std::vector<NodeId> pending{rightId};
        while (!pending.empty()) {
            const NodeId id = pending.back();
            pending.pop_back();
            Change change;
            change.status = NodeStatus::Added;
            change.right = id;
            emit(std::move(change));
            const auto& children = right_.node(id).children;
            pending.insert(pending.end(), children.rbegin(), children.rend());
        }
    }

    /// \brief Reports a whole left-hand subtree as deleted.
    ///
    /// \param leftId The root of the subtree.
    void deleteSubtree(NodeId leftId) {
        std::vector<NodeId> pending{leftId};
        while (!pending.empty()) {
            const NodeId id = pending.back();
            pending.pop_back();
            Change change;
            change.status = NodeStatus::Deleted;
            change.left = id;
            emit(std::move(change));
            const auto& children = left_.node(id).children;
            pending.insert(pending.end(), children.rbegin(), children.rend());
        }
    }

    /// \brief Reports whether a matched pair changed position.
    ///
    /// \param leftId The left node.
    /// \param rightId The right node it matched.
    ///
    /// \returns `true` when the node changed parent, or was reordered among the
    ///          siblings it still shares a parent with.
    bool isMove(NodeId leftId, NodeId rightId) const {
        const NodeId leftParent = left_.node(leftId).parent;
        const NodeId rightParent = right_.node(rightId).parent;
        if (leftParent == kInvalidNode || rightParent == kInvalidNode) {
            return leftParent != rightParent;
        }
        if (model_.matching.toRight(leftParent) != rightParent) {
            return true;
        }
        return reordered_[leftId];
    }

    /// \brief Marks reordering relative to the matching, not to raw position.
    ///
    /// \remarks A node whose index shifted only because a sibling before it was
    ///          deleted or inserted has not moved, and reporting it as moved
    ///          buries the one node that really did. So among the children a
    ///          parent pair still shares, the longest run that stayed in order
    ///          is treated as having stayed put, and only what breaks that order
    ///          is a move.
    void markReorderedChildren() {
        reordered_.assign(left_.size(), false);

        for (const Node& leftParent : left_.nodes()) {
            const NodeId rightParentId = model_.matching.toRight(leftParent.id);
            if (rightParentId == kInvalidNode) {
                continue;
            }
            if (!provider_.childrenOrdered(right_, rightParentId)) {
                continue;  // position carries nothing under this parent
            }

            std::vector<NodeId> retained;
            std::vector<std::uint32_t> positions;
            for (const NodeId leftChild : leftParent.children) {
                const NodeId rightChild = model_.matching.toRight(leftChild);
                if (rightChild == kInvalidNode ||
                    right_.node(rightChild).parent != rightParentId) {
                    continue;  // deleted, or moved to another parent entirely
                }
                retained.push_back(leftChild);
                positions.push_back(rightSibling_[rightChild]);
            }
            if (retained.size() < 2) {
                continue;
            }

            const auto keep = longestIncreasingRun(positions);
            for (std::size_t i = 0; i < retained.size(); ++i) {
                if (!keep[i]) {
                    reordered_[retained[i]] = true;
                }
            }
        }
    }

    /// \brief Finds the longest strictly increasing subsequence.
    ///
    /// \param values The sequence to examine.
    ///
    /// \returns One flag per element, `true` for every element the longest
    ///          increasing run keeps.
    ///
    /// \remarks Patience sorting, so the cost is linearithmic rather than
    ///          quadratic.
    static std::vector<bool> longestIncreasingRun(const std::vector<std::uint32_t>& values) {
        std::vector<std::size_t> tailIndex;    // index into values, per run length
        std::vector<std::size_t> predecessor(values.size(), values.size());

        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto it = std::lower_bound(
                tailIndex.begin(), tailIndex.end(), values[i],
                [&values](std::size_t index, std::uint32_t value) { return values[index] < value; });
            const auto slot = static_cast<std::size_t>(it - tailIndex.begin());
            if (slot > 0) {
                predecessor[i] = tailIndex[slot - 1];
            }
            if (it == tailIndex.end()) {
                tailIndex.push_back(i);
            } else {
                *it = i;
            }
        }

        std::vector<bool> keep(values.size(), false);
        if (tailIndex.empty()) {
            return keep;
        }
        for (std::size_t i = tailIndex.back(); i != values.size(); i = predecessor[i]) {
            keep[i] = true;
        }
        return keep;
    }

    /// \brief Reports what changed about one matched pair, if anything.
    ///
    /// \param leftId The left node.
    /// \param rightId The right node it matched.
    ///
    /// \remarks A node that both moved and changed is reported as modified with
    ///          the move noted, rather than as only one of the two.
    void recordPair(NodeId leftId, NodeId rightId) {
        auto changed = changedPropertyNames(provider_, left_, leftId, right_, rightId);
        const bool moved = isMove(leftId, rightId);
        if (changed.empty() && !moved) {
            return;
        }

        Change change;
        change.left = leftId;
        change.right = rightId;
        change.moved = moved;
        change.changedProperties = std::move(changed);
        // A node that both moved and changed is reported as modified, with the
        // move noted, rather than as only one of the two.
        change.status = change.changedProperties.empty() ? NodeStatus::Moved : NodeStatus::Modified;
        emit(std::move(change));
    }

    /// \brief One matched pair the walk is inside, and how far along its
    ///        children it has got.
    struct WalkFrame {
        NodeId left;                 ///< The left node.
        NodeId right;                ///< The right node it matched.
        std::size_t rightIndex = 0;  ///< The next right child to look at.
        std::size_t leftCursor = 0;  ///< The first left child not yet passed over.
    };

    /// \brief Walks a matched pair and everything below it.
    ///
    /// \param leftRoot The left node.
    /// \param rightRoot The right node it matched.
    ///
    /// \remarks Merges the two child lists so that additions and deletions are
    ///          reported where they happened rather than in a block at the end.
    ///          A child matched to a node under some other parent moved in from
    ///          elsewhere and is reported here, at its new home.
    ///
    ///          The frames are the call stack this used to have, kept on the
    ///          heap: each remembers how far along the right children it is and
    ///          which left children it has passed over, and a matched child is
    ///          recorded and pushed rather than descended into.
    void walkPair(NodeId leftRoot, NodeId rightRoot) {
        std::vector<WalkFrame> frames;
        recordPair(leftRoot, rightRoot);
        frames.push_back(WalkFrame{leftRoot, rightRoot});

        while (!frames.empty()) {
            WalkFrame& frame = frames.back();
            const auto& leftChildren = left_.node(frame.left).children;
            const auto& rightChildren = right_.node(frame.right).children;

            if (frame.rightIndex == rightChildren.size()) {
                // Left children after the last matched one were deleted or
                // moved away; a move is reported where it landed.
                while (frame.leftCursor < leftChildren.size()) {
                    const NodeId skipped = leftChildren[frame.leftCursor++];
                    if (!model_.matching.leftMatched(skipped)) {
                        deleteSubtree(skipped);
                    }
                }
                frames.pop_back();
                continue;
            }

            const NodeId rightChild = rightChildren[frame.rightIndex++];
            const NodeId partner = model_.matching.toLeft(rightChild);
            if (partner == kInvalidNode) {
                addSubtree(rightChild);
                continue;
            }
            if (left_.node(partner).parent != frame.left) {
                // Matched to a node under some other parent: it moved in from
                // elsewhere, and is reported here, at its new home.
                recordPair(partner, rightChild);
                frames.push_back(WalkFrame{partner, rightChild});
                continue;
            }

            // Left children passed over on the way to this one were either
            // deleted or moved away.
            while (frame.leftCursor < leftChildren.size() &&
                   leftChildren[frame.leftCursor] != partner) {
                const NodeId skipped = leftChildren[frame.leftCursor++];
                if (!model_.matching.leftMatched(skipped)) {
                    deleteSubtree(skipped);
                }
            }
            if (frame.leftCursor < leftChildren.size()) {
                ++frame.leftCursor;
            }
            recordPair(partner, rightChild);
            frames.push_back(WalkFrame{partner, rightChild});
        }
    }

    const Tree& left_;
    const Tree& right_;
    const IFormatProvider& provider_;
    std::vector<std::uint32_t> leftSibling_;
    std::vector<std::uint32_t> rightSibling_;
    std::vector<bool> reordered_;
    DiffModel model_;
};

}  // namespace

const char* describe(NodeStatus status) noexcept {
    switch (status) {
        case NodeStatus::Unchanged:
            return "unchanged";
        case NodeStatus::Added:
            return "added";
        case NodeStatus::Deleted:
            return "deleted";
        case NodeStatus::Modified:
            return "modified";
        case NodeStatus::Moved:
            return "moved";
    }
    return "unknown";
}

NodeStatus DiffModel::statusOf(Side side, NodeId id) const {
    const auto& statuses = side == Side::Left ? leftStatus : rightStatus;
    return id < statuses.size() ? statuses[id] : NodeStatus::Unchanged;
}

const Change* DiffModel::changeFor(Side side, NodeId id) const {
    const auto& index = side == Side::Left ? leftChangeIndex : rightChangeIndex;
    if (id >= index.size() || index[id] == kNoChange) {
        return nullptr;
    }
    return &changes[index[id]];
}

DiffModel classify(const Tree& left, const Tree& right, const IFormatProvider& provider,
                   MatchResult match) {
    Classifier classifier(left, right, provider, std::move(match));
    return classifier.run();
}

DiffModel diffTrees(const Tree& left, const Tree& right, const IFormatProvider& provider,
                    std::stop_token token, MatchOptions options) {
    const auto started = std::chrono::steady_clock::now();

    MatchResult match = matchTrees(left, right, provider, token, options);
    if (match.cancelled) {
        DiffModel model;
        model.cancelled = true;
        model.quality = match.quality;
        model.trimmedParents = match.trimmedParents;
        return model;
    }

    DiffModel model = classify(left, right, provider, std::move(match));
    model.elapsedMillis =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return model;
}

std::string nodePath(const Tree& tree, NodeId id) {
    if (id == kInvalidNode || id >= tree.size()) {
        return "?";
    }

    std::vector<std::string> parts;
    for (NodeId current = id; current != kInvalidNode; current = tree.node(current).parent) {
        const Node& node = tree.node(current);
        if (node.parent == kInvalidNode) {
            parts.push_back(node.kind);
            break;
        }
        // Position among siblings of the same kind, so inserting a node of a
        // different kind nearby does not renumber this one.
        std::uint32_t index = 0;
        for (const NodeId sibling : tree.node(node.parent).children) {
            if (sibling == current) {
                break;
            }
            if (tree.node(sibling).kind == node.kind) {
                ++index;
            }
        }
        parts.push_back(node.kind + "[" + std::to_string(index) + "]");
    }

    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        path += '/';
        path += *it;
    }
    return path;
}

std::string serializeChanges(const Tree& left, const Tree& right, const DiffModel& model) {
    std::ostringstream out;

    if (model.quality != MatchQuality::Full) {
        out << "! " << describe(model.quality) << "\n";
    }

    for (const Change& change : model.changes) {
        switch (change.status) {
            case NodeStatus::Added:
                out << "+ " << nodePath(right, change.right);
                break;
            case NodeStatus::Deleted:
                out << "- " << nodePath(left, change.left);
                break;
            case NodeStatus::Moved:
                out << "> " << nodePath(left, change.left) << " -> "
                    << nodePath(right, change.right);
                break;
            case NodeStatus::Modified:
                out << (change.moved ? "~> " : "~ ") << nodePath(right, change.right);
                break;
            case NodeStatus::Unchanged:
                continue;
        }

        if (!change.changedProperties.empty()) {
            out << " [";
            for (std::size_t i = 0; i < change.changedProperties.size(); ++i) {
                if (i > 0) {
                    out << ", ";
                }
                out << change.changedProperties[i];
            }
            out << "]";
        }
        out << "\n";
    }

    if (model.changes.empty()) {
        out << "identical\n";
    }
    return out.str();
}

}  // namespace nmtreediff
