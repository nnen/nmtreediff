/// \file
/// \brief Implementation of the union tree and its layout.

#include "core/layout_tree.h"

#include <algorithm>
#include <unordered_map>

namespace nmxd {

namespace {

/// \brief Packs a side and a node id into one key.
///
/// \param side Which document the node belongs to.
/// \param node The node's id within that tree.
///
/// \returns A key unique across both trees.
std::uint64_t nodeKey(Side side, NodeId node) {
    return (static_cast<std::uint64_t>(side == Side::Right ? 1u : 0u) << 32) | node;
}

/// \brief Shortens a title that would make a card wider than allowed.
///
/// \param text The text to fit.
/// \param maximumCharacters How many characters fit.
///
/// \returns \p text unchanged, or a prefix ending in an ellipsis.
std::string elide(std::string text, std::size_t maximumCharacters) {
    if (text.size() <= maximumCharacters || maximumCharacters < 2) {
        return text;
    }
    text.resize(maximumCharacters - 1);
    text += "\xE2\x80\xA6";  // horizontal ellipsis
    return text;
}

/// \brief Builds the union of both trees and lays it out.
class LayoutBuilder {
public:
    /// \brief Prepares a builder for one comparison.
    ///
    /// \param left The left tree.
    /// \param right The right tree.
    /// \param model The classified diff.
    /// \param provider The format provider.
    /// \param metrics The sizes to lay out in.
    LayoutBuilder(const Tree& left, const Tree& right, const DiffModel& model,
                  const IFormatProvider& provider, const LayoutMetrics& metrics)
        : left_(left), right_(right), model_(model), provider_(provider), metrics_(metrics) {}

    /// \brief Builds and positions the union.
    ///
    /// \param token Checked while walking the trees.
    ///
    /// \returns The positioned layout.
    TreeLayout build(const std::stop_token& token) {
        if (right_.empty() && left_.empty()) {
            return std::move(layout_);
        }

        if (right_.empty()) {
            layout_.root = addSubtree(Side::Left, left_.root(), kInvalidLayout, token);
        } else {
            layout_.root = addCard(Side::Right, right_.root(), kInvalidLayout);
            if (!left_.empty() && model_.matching.toLeft(right_.root()) != kInvalidNode) {
                walkPair(model_.matching.toLeft(right_.root()), right_.root(), layout_.root, token);
            } else {
                addChildrenOfRight(right_.root(), layout_.root, token);
            }

            // A left tree that shares no root with the right one is a wholesale
            // replacement, so its nodes are shown alongside rather than inside.
            if (!left_.empty() && model_.matching.toRight(left_.root()) == kInvalidNode) {
                addSubtree(Side::Left, left_.root(), kInvalidLayout, token);
            }
        }

        if (token.stop_requested()) {
            layout_.cancelled = true;
            return std::move(layout_);
        }

        markChangedSubtrees();
        resolveMoves();
        position();
        return std::move(layout_);
    }

private:
    /// \brief Creates one card for a document node.
    ///
    /// \param side Which document the node belongs to.
    /// \param node The node the card stands for.
    /// \param parent The parent card, or kInvalidLayout.
    ///
    /// \returns The new card's id.
    LayoutId addCard(Side side, NodeId node, LayoutId parent) {
        const Tree& tree = side == Side::Left ? left_ : right_;
        const NodeStyle style = provider_.style(tree, node);

        const auto maximumCharacters = static_cast<std::size_t>(
            (metrics_.maximumWidth - 2 * metrics_.padding) / metrics_.characterWidth);

        LayoutNode card;
        card.side = side;
        card.node = node;
        card.status = model_.statusOf(side, node);
        card.parent = parent;
        card.title = elide(style.title, maximumCharacters);
        card.subtitle = elide(style.subtitle, maximumCharacters);
        card.accent = style.accent;

        const std::size_t widest = std::max(card.title.size(), card.subtitle.size());
        card.width = std::clamp(static_cast<float>(widest) * metrics_.characterWidth +
                                    2 * metrics_.padding,
                                metrics_.minimumWidth, metrics_.maximumWidth);
        card.height = 2 * metrics_.padding + metrics_.lineHeight *
                                                 (card.subtitle.empty() ? 1.0f : 2.0f);

        const auto id = static_cast<LayoutId>(layout_.nodes.size());
        layout_.nodes.push_back(std::move(card));
        index_.emplace(nodeKey(side, node), id);

        if (parent != kInvalidLayout) {
            layout_.nodes[parent].children.push_back(id);
        }
        return id;
    }

    /// \brief Adds a whole subtree from one side, with no matching to consult.
    ///
    /// \param side Which document the subtree belongs to.
    /// \param node The subtree's root.
    /// \param parent The parent card, or kInvalidLayout.
    /// \param token Checked as the walk descends.
    ///
    /// \returns The subtree root's card id.
    LayoutId addSubtree(Side side, NodeId node, LayoutId parent, const std::stop_token& token) {
        const LayoutId id = addCard(side, node, parent);
        if (token.stop_requested()) {
            return id;
        }
        const Tree& tree = side == Side::Left ? left_ : right_;
        for (const NodeId child : tree.node(node).children) {
            addSubtree(side, child, id, token);
        }
        return id;
    }

    /// \brief Adds every child of a right node, ignoring the matching.
    ///
    /// \param rightId The right node whose children to add.
    /// \param card The card standing for \p rightId.
    /// \param token Checked as the walk descends.
    void addChildrenOfRight(NodeId rightId, LayoutId card, const std::stop_token& token) {
        for (const NodeId child : right_.node(rightId).children) {
            addSubtree(Side::Right, child, card, token);
        }
    }

    /// \brief Walks a matched pair, interleaving the nodes the left side lost.
    ///
    /// \param leftId The left node.
    /// \param rightId The right node it matched.
    /// \param card The card standing for the pair.
    /// \param token Checked as the walk descends.
    ///
    /// \remarks Mirrors how the classifier merges the two child lists, so a
    ///          deletion appears where it happened rather than in a block at the
    ///          end.
    void walkPair(NodeId leftId, NodeId rightId, LayoutId card, const std::stop_token& token) {
        if (token.stop_requested()) {
            return;
        }

        const auto& leftChildren = left_.node(leftId).children;
        const auto& rightChildren = right_.node(rightId).children;

        std::size_t leftCursor = 0;
        for (const NodeId rightChild : rightChildren) {
            const NodeId partner = model_.matching.toLeft(rightChild);

            if (partner == kInvalidNode) {
                addSubtree(Side::Right, rightChild, card, token);
                continue;
            }
            if (left_.node(partner).parent != leftId) {
                const LayoutId childCard = addCard(Side::Right, rightChild, card);
                walkPair(partner, rightChild, childCard, token);
                continue;
            }

            while (leftCursor < leftChildren.size() && leftChildren[leftCursor] != partner) {
                const NodeId skipped = leftChildren[leftCursor];
                if (!model_.matching.leftMatched(skipped)) {
                    addSubtree(Side::Left, skipped, card, token);
                }
                ++leftCursor;
            }
            if (leftCursor < leftChildren.size()) {
                ++leftCursor;
            }

            const LayoutId childCard = addCard(Side::Right, rightChild, card);
            walkPair(partner, rightChild, childCard, token);
        }

        while (leftCursor < leftChildren.size()) {
            const NodeId skipped = leftChildren[leftCursor];
            if (!model_.matching.leftMatched(skipped)) {
                addSubtree(Side::Left, skipped, card, token);
            }
            ++leftCursor;
        }
    }

    /// \brief Flags every card with a change anywhere below it.
    ///
    /// \remarks Walks backwards, so a card is visited after all its children.
    ///          Cards are created parents before children, which is what makes
    ///          that sound.
    void markChangedSubtrees() {
        for (std::size_t i = layout_.nodes.size(); i-- > 0;) {
            LayoutNode& card = layout_.nodes[i];
            if (card.status != NodeStatus::Unchanged) {
                card.subtreeChanged = true;
            }
            card.hiddenDescendants = 0;
            for (const LayoutId child : card.children) {
                card.hiddenDescendants += layout_.nodes[child].hiddenDescendants + 1;
                if (layout_.nodes[child].subtreeChanged) {
                    card.subtreeChanged = true;
                }
            }
        }
    }

    /// \brief Points every moved card at where it used to sit.
    ///
    /// \remarks The former parent is the card standing for the matched left
    ///          parent, which is only present when that parent survived.
    void resolveMoves() {
        for (LayoutNode& card : layout_.nodes) {
            if (card.status != NodeStatus::Moved && !movedAndModified(card)) {
                continue;
            }
            if (card.side != Side::Right) {
                continue;
            }
            const NodeId leftId = model_.matching.toLeft(card.node);
            if (leftId == kInvalidNode) {
                continue;
            }
            const NodeId leftParent = left_.node(leftId).parent;
            if (leftParent == kInvalidNode) {
                continue;
            }
            const NodeId rightParent = model_.matching.toRight(leftParent);
            if (rightParent == kInvalidNode) {
                continue;
            }
            const auto it = index_.find(nodeKey(Side::Right, rightParent));
            const LayoutId from = it == index_.end() ? kInvalidLayout : it->second;
            if (from != kInvalidLayout && from != card.parent) {
                card.movedFrom = from;
            }
        }
    }

    /// \brief Reports whether a card both moved and changed.
    ///
    /// \param card The card to test.
    ///
    /// \returns `true` when the change list marked it as moved as well as
    ///          modified.
    bool movedAndModified(const LayoutNode& card) const {
        if (card.status != NodeStatus::Modified) {
            return false;
        }
        for (const Change& change : model_.changes) {
            if (change.right == card.node && change.moved) {
                return true;
            }
        }
        return false;
    }

    /// \brief Assigns every card a position.
    ///
    /// \remarks Two passes. The first, backwards, gives each subtree a width.
    ///          The second, forwards, places each subtree inside the span its
    ///          parent allotted it. Both are linear.
    void position() {
        if (layout_.nodes.empty()) {
            return;
        }

        std::vector<float> subtreeWidth(layout_.nodes.size(), 0.0f);
        for (std::size_t i = layout_.nodes.size(); i-- > 0;) {
            const LayoutNode& card = layout_.nodes[i];
            float childrenWidth = 0.0f;
            for (std::size_t c = 0; c < card.children.size(); ++c) {
                if (c > 0) {
                    childrenWidth += metrics_.siblingGap;
                }
                childrenWidth += subtreeWidth[card.children[c]];
            }
            subtreeWidth[i] = std::max(card.width, childrenWidth);
        }

        // Roots are stacked left to right, so a wholesale replacement shows both
        // documents side by side rather than on top of each other.
        float rootCursor = 0.0f;
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            if (layout_.nodes[i].parent != kInvalidLayout) {
                continue;
            }
            place(static_cast<LayoutId>(i), rootCursor, 0.0f, subtreeWidth);
            rootCursor += subtreeWidth[i] + metrics_.siblingGap * 2.0f;
        }

        float maxX = 0.0f;
        float maxY = 0.0f;
        for (const LayoutNode& card : layout_.nodes) {
            maxX = std::max(maxX, card.x + card.width);
            maxY = std::max(maxY, card.y + card.height);
        }
        layout_.width = maxX;
        layout_.height = maxY;
    }

    /// \brief Places one card and everything below it.
    ///
    /// \param id The card to place.
    /// \param left Left edge of the span allotted to this subtree.
    /// \param top Top edge of this depth.
    /// \param subtreeWidth Width of every subtree, indexed by card id.
    void place(LayoutId id, float left, float top, const std::vector<float>& subtreeWidth) {
        LayoutNode& card = layout_.nodes[id];
        card.y = top;
        card.x = left + (subtreeWidth[id] - card.width) * 0.5f;

        const float childTop = top + card.height + metrics_.levelGap;
        float cursor = left;
        // Children fill the parent's span from the left, so a parent with one
        // child sits directly above it.
        float childrenWidth = 0.0f;
        for (std::size_t c = 0; c < card.children.size(); ++c) {
            if (c > 0) {
                childrenWidth += metrics_.siblingGap;
            }
            childrenWidth += subtreeWidth[card.children[c]];
        }
        cursor = left + (subtreeWidth[id] - childrenWidth) * 0.5f;

        const std::vector<LayoutId> children = card.children;
        for (const LayoutId child : children) {
            place(child, cursor, childTop, subtreeWidth);
            cursor += subtreeWidth[child] + metrics_.siblingGap;
        }
    }

    const Tree& left_;
    const Tree& right_;
    const DiffModel& model_;
    const IFormatProvider& provider_;
    LayoutMetrics metrics_;
    TreeLayout layout_;
    std::unordered_map<std::uint64_t, LayoutId> index_;
};

}  // namespace

LayoutId TreeLayout::find(Side side, NodeId node) const {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].side == side && nodes[i].node == node) {
            return static_cast<LayoutId>(i);
        }
    }
    return kInvalidLayout;
}

TreeLayout buildLayout(const Tree& left, const Tree& right, const DiffModel& model,
                       const IFormatProvider& provider, std::stop_token token,
                       LayoutMetrics metrics) {
    LayoutBuilder builder(left, right, model, provider, metrics);
    return builder.build(token);
}

NodeId findNodeAt(const Tree& tree, std::uint32_t offset) {
    if (tree.empty()) {
        return kInvalidNode;
    }

    NodeId current = tree.root();
    const SourceSpan rootSpan = tree.node(current).span;
    if (offset < rootSpan.begin || offset >= rootSpan.end) {
        return kInvalidNode;
    }

    for (;;) {
        NodeId next = kInvalidNode;
        for (const NodeId child : tree.node(current).children) {
            const SourceSpan span = tree.node(child).span;
            if (offset >= span.begin && offset < span.end) {
                next = child;
                break;
            }
        }
        if (next == kInvalidNode) {
            return current;
        }
        current = next;
    }
}

}  // namespace nmxd
