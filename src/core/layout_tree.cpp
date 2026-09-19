/// \file
/// \brief Implementation of the union tree and its layout.

#include "core/layout_tree.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nmtreediff {

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
    /// \param stacking Which way a stacked card sits relative to its parent.
    LayoutBuilder(const Tree& left, const Tree& right, const DiffModel& model,
                  const IFormatProvider& provider, const LayoutMetrics& metrics,
                  GraphDirection direction, StackDirection stacking)
        : left_(left), right_(right), model_(model), provider_(provider), metrics_(metrics),
          direction_(direction), stacking_(stacking) {}

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
        markStacks();
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
        card.stackable = style.stacked;

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

    /// \brief A node waiting for its card, and the card it hangs under.
    struct PendingCard {
        NodeId node;      ///< The node to make a card for.
        LayoutId parent;  ///< The card it hangs under.
    };

    /// \brief Queues a node's children for cards under the node's own card.
    ///
    /// \param tree The tree the node belongs to.
    /// \param node The node whose children to queue.
    /// \param card The node's card.
    /// \param pending The stack to queue them on.
    ///
    /// \remarks In reverse, so they come off the stack in document order and
    ///          their cards are created in it. markChangedSubtrees() relies on
    ///          parents being created before children, and the card order of
    ///          siblings is the order they are drawn in.
    static void queueChildren(const Tree& tree, NodeId node, LayoutId card,
                              std::vector<PendingCard>& pending) {
        const auto& children = tree.node(node).children;
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            pending.push_back(PendingCard{*it, card});
        }
    }

    /// \brief Adds a whole subtree from one side, with no matching to consult.
    ///
    /// \param side Which document the subtree belongs to.
    /// \param node The subtree's root.
    /// \param parent The parent card, or kInvalidLayout.
    /// \param token Checked as the walk descends.
    ///
    /// \returns The subtree root's card id.
    ///
    /// \remarks Walks with a stack of its own rather than the call stack, as
    ///          every walk in this class does: a document is as deep as
    ///          whatever wrote it, and the call stack is not.
    LayoutId addSubtree(Side side, NodeId node, LayoutId parent, const std::stop_token& token) {
        const Tree& tree = side == Side::Left ? left_ : right_;
        const LayoutId root = addCard(side, node, parent);

        std::vector<PendingCard> pending;
        queueChildren(tree, node, root, pending);
        while (!pending.empty() && !token.stop_requested()) {
            const PendingCard next = pending.back();
            pending.pop_back();
            const LayoutId id = addCard(side, next.node, next.parent);
            queueChildren(tree, next.node, id, pending);
        }
        return root;
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

    /// \brief One matched pair the walk is inside, and how far along its
    ///        children it has got.
    struct WalkFrame {
        NodeId left;                 ///< The left node.
        NodeId right;                ///< The right node it matched.
        LayoutId card;               ///< The card standing for the pair.
        std::size_t rightIndex = 0;  ///< The next right child to look at.
        std::size_t leftCursor = 0;  ///< The first left child not yet passed over.
    };

    /// \brief Walks a matched pair, interleaving the nodes the left side lost.
    ///
    /// \param leftRoot The left node.
    /// \param rightRoot The right node it matched.
    /// \param rootCard The card standing for the pair.
    /// \param token Checked as the walk descends.
    ///
    /// \remarks Mirrors how the classifier merges the two child lists, so a
    ///          deletion appears where it happened rather than in a block at the
    ///          end. The frames are the call stack this used to have, kept on
    ///          the heap, for the same reason addSubtree() keeps its own.
    void walkPair(NodeId leftRoot, NodeId rightRoot, LayoutId rootCard,
                  const std::stop_token& token) {
        std::vector<WalkFrame> frames;
        frames.push_back(WalkFrame{leftRoot, rightRoot, rootCard});

        while (!frames.empty()) {
            if (token.stop_requested()) {
                return;
            }
            WalkFrame& frame = frames.back();
            const auto& leftChildren = left_.node(frame.left).children;
            const auto& rightChildren = right_.node(frame.right).children;

            if (frame.rightIndex == rightChildren.size()) {
                while (frame.leftCursor < leftChildren.size()) {
                    const NodeId skipped = leftChildren[frame.leftCursor++];
                    if (!model_.matching.leftMatched(skipped)) {
                        addSubtree(Side::Left, skipped, frame.card, token);
                    }
                }
                frames.pop_back();
                continue;
            }

            const NodeId rightChild = rightChildren[frame.rightIndex++];
            const NodeId partner = model_.matching.toLeft(rightChild);
            if (partner == kInvalidNode) {
                addSubtree(Side::Right, rightChild, frame.card, token);
                continue;
            }
            if (left_.node(partner).parent != frame.left) {
                const LayoutId childCard = addCard(Side::Right, rightChild, frame.card);
                frames.push_back(WalkFrame{partner, rightChild, childCard});
                continue;
            }

            while (frame.leftCursor < leftChildren.size() &&
                   leftChildren[frame.leftCursor] != partner) {
                const NodeId skipped = leftChildren[frame.leftCursor++];
                if (!model_.matching.leftMatched(skipped)) {
                    addSubtree(Side::Left, skipped, frame.card, token);
                }
            }
            if (frame.leftCursor < leftChildren.size()) {
                ++frame.leftCursor;
            }

            const LayoutId childCard = addCard(Side::Right, rightChild, frame.card);
            frames.push_back(WalkFrame{partner, rightChild, childCard});
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

    /// \brief Reports whether a stacked card sits further along the depth
    ///        axis than its parent, rather than further along the breadth axis.
    ///
    /// \returns `true` when stacks run along depth in this layout.
    ///
    /// \remarks Vertical is the depth axis top-down and the breadth axis
    ///          left-to-right; the graph's direction decides which. The rest
    ///          of the stacking code asks this and never asks about
    ///          "vertical", which is what leaves the other direction open.
    [[nodiscard]] bool stacksAlongDepth() const {
        if (stacking_ == StackDirection::AlongDepth) {
            return true;
        }
        return !horizontal();
    }

    /// \brief Decides which cards draw as part of the card above them.
    ///
    /// \remarks A card stacks on its parent when the format flagged the
    ///          parent and the parent has exactly one card under it in the
    ///          union. Two cards under it, a deleted child beside an added
    ///          one, are the change, and drawing either as the block's own
    ///          would hide it. The members of a chain then share one width,
    ///          the widest, so the chain reads as one block; a card is
    ///          measured in its own width before this and drawn in the
    ///          shared one after, and the elision it was given still fits.
    void markStacks() {
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            LayoutNode& card = layout_.nodes[i];
            card.stackBottom = static_cast<LayoutId>(i);
            if (card.parent == kInvalidLayout) {
                continue;
            }
            const LayoutNode& parent = layout_.nodes[card.parent];
            card.stackedOnParent = parent.stackable && parent.children.size() == 1;
        }

        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            const auto head = static_cast<LayoutId>(i);
            if (layout_.nodes[i].stackedOnParent || stackedChild(head) == kInvalidLayout) {
                continue;
            }
            const LayoutId bottom = stackBottom(head);
            float widest = 0.0f;
            for (LayoutId m = head; m != kInvalidLayout; m = stackedChild(m)) {
                widest = std::max(widest, layout_.nodes[m].width);
            }
            for (LayoutId m = head; m != kInvalidLayout; m = stackedChild(m)) {
                layout_.nodes[m].width = widest;
                layout_.nodes[m].stackBottom = bottom;
            }
        }
    }

    /// \brief Returns the card stacked directly under one card.
    ///
    /// \param id The card to look under.
    ///
    /// \returns The stacked child, or kInvalidLayout when the card ends its
    ///          stack, which is the usual case.
    [[nodiscard]] LayoutId stackedChild(LayoutId id) const {
        const auto& children = layout_.nodes[id].children;
        if (children.size() == 1 && layout_.nodes[children[0]].stackedOnParent) {
            return children[0];
        }
        return kInvalidLayout;
    }

    /// \brief Returns the last card of the stack one card heads.
    ///
    /// \param head The first card of the stack.
    ///
    /// \returns The bottom member, or \p head itself when nothing stacks on
    ///          it. Its children are the stack's children.
    [[nodiscard]] LayoutId stackBottom(LayoutId head) const {
        LayoutId bottom = head;
        for (LayoutId m = stackedChild(head); m != kInvalidLayout; m = stackedChild(m)) {
            bottom = m;
        }
        return bottom;
    }

    /// \brief The size of a stack taken as one card.
    struct Composite {
        float breadth = 0.0f;  ///< Extent along the sibling axis.
        float depth = 0.0f;    ///< Extent along the axis that grows with depth.
    };

    /// \brief Measures a stack as the one card the positioning pass sees.
    ///
    /// \param head The first card of the stack, or any card that heads none.
    ///
    /// \returns The members' extents summed along the stacking axis and the
    ///          widest of them across it. For a card that heads no stack,
    ///          its own size.
    [[nodiscard]] Composite composite(LayoutId head) const {
        Composite block;
        for (LayoutId m = head; m != kInvalidLayout; m = stackedChild(m)) {
            const LayoutNode& card = layout_.nodes[m];
            if (stacksAlongDepth()) {
                block.depth += depthOf(card);
                block.breadth = std::max(block.breadth, breadthOf(card));
            } else {
                block.breadth += breadthOf(card);
                block.depth = std::max(block.depth, depthOf(card));
            }
        }
        return block;
    }

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
    /// \returns The subtree breadth of each card, indexed by card id. A card
    ///          stacked on its parent is measured as part of its stack's head
    ///          and has no entry of its own.
    ///
    /// \remarks Runs backwards, so a card's children are already measured
    ///          when it is reached. Parents before children in the card order is
    ///          what makes that true, and it holds for a stack's bottom member
    ///          and its children too.
    [[nodiscard]] std::vector<float> measureSubtrees() const {
        std::vector<float> breadth(layout_.nodes.size(), 0.0f);
        for (std::size_t i = layout_.nodes.size(); i-- > 0;) {
            if (layout_.nodes[i].stackedOnParent) {
                continue;
            }
            const auto head = static_cast<LayoutId>(i);
            breadth[i] = std::max(composite(head).breadth,
                                  childrenBreadth(stackBottom(head), breadth));
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
    /// \returns The level of each card, indexed by card id. A root is zero,
    ///          and a card stacked on its parent shares the parent's level.
    [[nodiscard]] std::vector<std::uint32_t> measureLevels() const {
        std::vector<std::uint32_t> level(layout_.nodes.size(), 0);
        for (std::size_t i = 0; i < layout_.nodes.size(); ++i) {
            const LayoutNode& card = layout_.nodes[i];
            if (card.parent != kInvalidLayout) {
                level[i] = level[card.parent] + (card.stackedOnParent ? 0 : 1);
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
            if (layout_.nodes[i].stackedOnParent) {
                continue;  // counted in its head's composite
            }
            extent[level[i]] =
                std::max(extent[level[i]], composite(static_cast<LayoutId>(i)).depth);
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
            place(static_cast<LayoutId>(i), rootCursor, subtreeBreadth, levelOffset);
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

    /// \brief A card waiting to be placed, and the span it was allotted.
    struct Placement {
        LayoutId id;          ///< The card to place.
        float breadth;        ///< Start of the span allotted to its subtree.
        std::uint32_t depth;  ///< The level it sits at.
    };

    /// \brief Places one root card and everything below it.
    ///
    /// \param root The root card to place.
    /// \param breadth Start of the span allotted to the whole subtree.
    /// \param subtreeBreadth Breadth of every subtree, indexed by card id.
    /// \param levelOffset Where each level begins, indexed by level.
    ///
    /// \remarks Each card's position follows from its parent's allotment
    ///          alone, so the order cards are placed in does not matter and a
    ///          plain stack serves in place of recursion. A stack is placed as
    ///          one card, its members inside it, and the bottom member's
    ///          children are the stack's children.
    void place(LayoutId root, float breadth, const std::vector<float>& subtreeBreadth,
               const std::vector<float>& levelOffset) {
        std::vector<Placement> pending;
        pending.push_back(Placement{root, breadth, 0});

        while (!pending.empty()) {
            const Placement next = pending.back();
            pending.pop_back();

            // The card, or the stack it heads, sits centred in the span its
            // parent allotted it.
            const Composite block = composite(next.id);
            placeStack(next.id, next.breadth + (subtreeBreadth[next.id] - block.breadth) * 0.5f,
                       levelOffset[next.depth], block);

            // Children fill that same span from its start, so a parent with
            // one child lines up exactly with it.
            const LayoutId bottom = stackBottom(next.id);
            const float total = childrenBreadth(bottom, subtreeBreadth);
            float cursor = next.breadth + (subtreeBreadth[next.id] - total) * 0.5f;
            for (const LayoutId child : layout_.nodes[bottom].children) {
                pending.push_back(Placement{child, cursor, next.depth + 1});
                cursor += subtreeBreadth[child] + metrics_.siblingGap;
            }
        }
    }

    /// \brief Places the members of one stack inside the block allotted to it.
    ///
    /// \param head The first card of the stack, or a card that heads none.
    /// \param breadth Where the block begins along the sibling axis.
    /// \param depth Where the block begins along the depth axis.
    /// \param block The block's size, from composite().
    ///
    /// \remarks Members follow one another along the stacking axis with no
    ///          gap, each centred across it. A card that heads no stack is
    ///          its own block, so this places it exactly where the plain
    ///          card placement did.
    void placeStack(LayoutId head, float breadth, float depth, const Composite& block) {
        float along = 0.0f;
        for (LayoutId m = head; m != kInvalidLayout; m = stackedChild(m)) {
            LayoutNode& card = layout_.nodes[m];
            if (stacksAlongDepth()) {
                setPosition(card, breadth + (block.breadth - breadthOf(card)) * 0.5f,
                            depth + along);
                along += depthOf(card);
            } else {
                setPosition(card, breadth + along, depth + (block.depth - depthOf(card)) * 0.5f);
                along += breadthOf(card);
            }
        }
    }

    const Tree& left_;
    const Tree& right_;
    const DiffModel& model_;
    const IFormatProvider& provider_;
    LayoutMetrics metrics_;
    GraphDirection direction_;
    StackDirection stacking_;
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
                       LayoutMetrics metrics, GraphDirection direction, StackDirection stacking,
                       StackEntryPin entryPin) {
    LayoutBuilder builder(left, right, model, provider, metrics, direction, stacking);
    TreeLayout layout = builder.build(token);
    layout.metrics = metrics;
    layout.direction = direction;
    layout.stacking = stacking;
    layout.entryPin = entryPin;
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

}  // namespace nmtreediff
