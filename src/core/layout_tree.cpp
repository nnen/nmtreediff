/// \file
/// \brief Implementation of the union tree and its layout.

#include "core/layout_tree.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

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
    /// \param direction Which way the graph runs.
    LayoutBuilder(const Tree& left, const Tree& right, const DiffModel& model,
                  const IFormatProvider& provider, const LayoutMetrics& metrics,
                  GraphDirection direction)
        : left_(left), right_(right), model_(model), provider_(provider), metrics_(metrics),
          direction_(direction) {}

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

    /// \brief Reports whether the graph runs left to right.
    ///
    /// \returns `true` when depth advances along x and breadth along y.
    [[nodiscard]] bool horizontal() const { return direction_ == GraphDirection::LeftToRight; }

    /// \brief Returns how much of the breadth axis a card occupies.
    ///
    /// \param card The card to measure.
    ///
    /// \returns The card's size along the axis siblings are laid out on.
    [[nodiscard]] float breadthOf(const LayoutNode& card) const {
        return horizontal() ? card.height : card.width;
    }

    /// \brief Returns how much of the depth axis a card occupies.
    ///
    /// \param card The card to measure.
    ///
    /// \returns The card's size along the axis that grows with depth.
    [[nodiscard]] float depthOf(const LayoutNode& card) const {
        return horizontal() ? card.width : card.height;
    }

    /// \brief Writes a breadth and depth position back as x and y.
    ///
    /// \param card The card to place.
    /// \param breadth Position along the sibling axis.
    /// \param depth Position along the axis that grows with depth.
    void setPosition(LayoutNode& card, float breadth, float depth) const {
        if (horizontal()) {
            card.x = depth;
            card.y = breadth;
        } else {
            card.x = breadth;
            card.y = depth;
        }
    }

    /// \brief Gives every card the breadth its whole subtree needs.
    ///
    /// \returns The subtree breadth of each card, indexed by card id.
    ///
    /// \remarks Runs backwards, so a card's children are already measured
    ///          when it is reached. Parents before children in the card order is
    ///          what makes that true.
    [[nodiscard]] std::vector<float> measureSubtrees() const {
        std::vector<float> breadth(layout_.nodes.size(), 0.0f);
        for (std::size_t i = layout_.nodes.size(); i-- > 0;) {
            breadth[i] = std::max(breadthOf(layout_.nodes[i]), childrenBreadth(i, breadth));
        }
        return breadth;
    }

    /// \brief Adds up what one card's children occupy, gaps included.
    ///
    /// \param id The parent card.
    /// \param subtreeBreadth Breadth of every subtree, indexed by card id.
    ///
    /// \returns The total breadth of the children laid side by side.
    [[nodiscard]] float childrenBreadth(std::size_t id,
                                        const std::vector<float>& subtreeBreadth) const {
        const std::vector<LayoutId>& children = layout_.nodes[id].children;
        float total = 0.0f;
        for (std::size_t c = 0; c < children.size(); ++c) {
            if (c > 0) {
                total += metrics_.siblingGap;
            }
            total += subtreeBreadth[children[c]];
        }
        return total;
    }

    /// \brief Returns how deep each card sits, counted in levels.
    ///
    /// \returns The level of each card, indexed by card id. A root is zero.
    [[nodiscard]] std::vector<std::uint32_t> measureLevels() const {
        std::vector<std::uint32_t> level(layout_.nodes.size(), 0);
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            const LayoutId parent = layout_.nodes[i].parent;
            if (parent != kInvalidLayout) {
                level[i] = level[parent] + 1;
            }
        }
        return level;
    }

    /// \brief Works out where each level begins on the depth axis.
    ///
    /// \param level The level of each card, indexed by card id.
    ///
    /// \returns The depth offset of each level, indexed by level.
    ///
    /// \remarks A level begins where the deepest card of the level before it
    ///          ended, so cards of one level line up. Advancing by each card's
    ///          own size instead would leave a ragged edge, which reads as
    ///          disorder rather than as depth, and is worst left to right where
    ///          card widths vary most.
    [[nodiscard]] std::vector<float> measureLevelOffsets(
        const std::vector<std::uint32_t>& level) const {
        if (level.empty()) {
            return {};
        }

        const std::uint32_t deepest = *std::max_element(level.begin(), level.end());
        std::vector<float> extent(deepest + 1, 0.0f);
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            extent[level[i]] = std::max(extent[level[i]], depthOf(layout_.nodes[i]));
        }

        std::vector<float> offset(deepest + 1, 0.0f);
        for (std::uint32_t d = 1; d <= deepest; ++d) {
            offset[d] = offset[d - 1] + extent[d - 1] + metrics_.levelGap;
        }
        return offset;
    }

    /// \brief Assigns every card a position.
    ///
    /// \remarks Measure the breadth of every subtree, work out where each
    ///          level starts, then place the roots one after another. Roots are
    ///          stacked along the breadth axis, so a wholesale replacement shows
    ///          both documents beside each other rather than on top of each
    ///          other.
    void position() {
        if (layout_.nodes.empty()) {
            return;
        }

        const std::vector<float> subtreeBreadth = measureSubtrees();
        const std::vector<std::uint32_t> level = measureLevels();
        const std::vector<float> levelOffset = measureLevelOffsets(level);

        float rootCursor = 0.0f;
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            if (layout_.nodes[i].parent != kInvalidLayout) {
                continue;
            }
            place(static_cast<LayoutId>(i), rootCursor, 0, subtreeBreadth, level, levelOffset);
            rootCursor += subtreeBreadth[i] + metrics_.siblingGap * 2.0f;
        }

        measureExtent();
    }

    /// \brief Records how large the finished drawing is.
    void measureExtent() {
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
    /// \param breadth Start of the span allotted to this subtree.
    /// \param depth The level this card sits at.
    /// \param subtreeBreadth Breadth of every subtree, indexed by card id.
    /// \param level The level of each card, indexed by card id.
    /// \param levelOffset Where each level begins, indexed by level.
    void place(LayoutId id, float breadth, std::uint32_t depth,
               const std::vector<float>& subtreeBreadth,
               const std::vector<std::uint32_t>& level, const std::vector<float>& levelOffset) {
        // The card sits centred in the span its parent allotted it.
        LayoutNode& card = layout_.nodes[id];
        setPosition(card, breadth + (subtreeBreadth[id] - breadthOf(card)) * 0.5f,
                    levelOffset[depth]);

        // Children fill that same span from its start, so a parent with one
        // child lines up exactly with it.
        const float total = childrenBreadth(id, subtreeBreadth);
        float cursor = breadth + (subtreeBreadth[id] - total) * 0.5f;

        const std::vector<LayoutId> children = card.children;
        for (const LayoutId child : children) {
            place(child, cursor, depth + 1, subtreeBreadth, level, levelOffset);
            cursor += subtreeBreadth[child] + metrics_.siblingGap;
        }
    }

    const Tree& left_;
    const Tree& right_;
    const DiffModel& model_;
    const IFormatProvider& provider_;
    LayoutMetrics metrics_;
    GraphDirection direction_;
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
                       LayoutMetrics metrics, GraphDirection direction) {
    LayoutBuilder builder(left, right, model, provider, metrics, direction);
    TreeLayout layout = builder.build(token);
    layout.metrics = metrics;
    layout.direction = direction;
    return layout;
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
