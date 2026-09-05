#pragma once

/// \file
/// \brief The union of two trees, positioned for drawing.

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "core/diff.h"
#include "core/provider.h"
#include "core/tree.h"

namespace nmxd {

/// \brief Identifies a node in a TreeLayout.
using LayoutId = std::uint32_t;

/// \brief The id that means "no layout node".
inline constexpr LayoutId kInvalidLayout = 0xFFFFFFFFu;

/// \brief One card in the node view.
///
/// \remarks A layout node names the document node it stands for, so a click on
///          a card leads back to a real node, its properties and its source
///          span.
struct LayoutNode {
    /// \brief Which document the node comes from.
    ///
    /// \remarks Deleted nodes come from the left; everything else comes from the
    ///          right, because the right is the version being reviewed.
    Side side = Side::Right;

    /// \brief The node's id within its own tree.
    NodeId node = kInvalidNode;

    /// \brief What happened to the node.
    NodeStatus status = NodeStatus::Unchanged;

    /// \brief The parent card, or kInvalidLayout for the root.
    LayoutId parent = kInvalidLayout;

    /// \brief Child cards, in the order they are drawn.
    std::vector<LayoutId> children;

    /// \brief Where a moved node used to sit, or kInvalidLayout.
    ///
    /// \remarks Drawn as a ghost edge, so a move reads as one node that went
    ///          somewhere rather than as a deletion and an unrelated addition.
    LayoutId movedFrom = kInvalidLayout;

    /// \brief Position of the card's top-left corner, in layout units.
    float x = 0.0f;
    /// \brief Position of the card's top-left corner, in layout units.
    float y = 0.0f;
    /// \brief Card width in layout units.
    float width = 0.0f;
    /// \brief Card height in layout units.
    float height = 0.0f;

    /// \brief The card's main line, from the provider.
    std::string title;
    /// \brief The card's second line, from the provider. May be empty.
    std::string subtitle;
    /// \brief The provider's colour, before diff status is applied.
    Color accent;

    /// \brief Whether anything below this node changed.
    ///
    /// \remarks Drives the default collapse: a subtree with nothing to report is
    ///          worth hiding, and one with a change in it is not.
    bool subtreeChanged = false;

    /// \brief How many nodes this card stands for when collapsed.
    std::uint32_t hiddenDescendants = 0;
};

/// \brief The union of both trees, laid out for drawing.
///
/// \remarks Holds one card per node of the right tree, plus a card for every
///          node the left tree lost. Positions are in abstract layout units
///          rather than pixels, so the layout can be computed on a worker with
///          no access to the font.
struct TreeLayout {
    /// \brief Every card, parents before children.
    std::vector<LayoutNode> nodes;

    /// \brief The root card, or kInvalidLayout when the layout is empty.
    LayoutId root = kInvalidLayout;

    /// \brief Width of the whole drawing in layout units.
    float width = 0.0f;
    /// \brief Height of the whole drawing in layout units.
    float height = 0.0f;

    /// \brief Whether the layout stopped early because it was cancelled.
    bool cancelled = false;

    /// \brief Finds the card standing for one document node.
    ///
    /// \param side Which document the node belongs to.
    /// \param node The node to look for.
    ///
    /// \returns The card's id, or kInvalidLayout when the node has no card.
    [[nodiscard]] LayoutId find(Side side, NodeId node) const;

    /// \brief Returns how many cards the layout holds.
    ///
    /// \returns The card count.
    [[nodiscard]] std::size_t size() const noexcept { return nodes.size(); }

    /// \brief Reports whether the layout holds any cards.
    ///
    /// \returns `true` when there is nothing to draw.
    [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }
};

/// \brief The sizes the layout works in.
///
/// \remarks Layout units are approximate character cells rather than pixels, so
///          the layout stays independent of the font actually in use. The view
///          multiplies by the same character advance when it draws.
struct LayoutMetrics {
    /// \brief Width of one character, in layout units.
    float characterWidth = 7.0f;
    /// \brief Height of one line of text, in layout units.
    float lineHeight = 16.0f;
    /// \brief Padding inside a card.
    float padding = 6.0f;
    /// \brief Smallest card width, so a short title still gives a usable target.
    float minimumWidth = 90.0f;
    /// \brief Largest card width, past which a title is elided.
    float maximumWidth = 260.0f;
    /// \brief Gap between sibling subtrees.
    float siblingGap = 18.0f;
    /// \brief Gap between one depth and the next.
    float levelGap = 46.0f;
};

/// \brief Builds the union of two trees and positions it for drawing.
///
/// \param left The left tree.
/// \param right The right tree.
/// \param model The classified diff between them.
/// \param provider The format provider, consulted for titles and colours.
/// \param token Checked while building; the layout gives up when a stop is
///        requested.
/// \param metrics The sizes to lay out in.
///
/// \returns The positioned union. TreeLayout::cancelled is set when the token
///          stopped the work.
///
/// \remarks Cards are laid out top down: children sit below their parent and a
///          parent is centred over them. Sibling subtrees are placed one after
///          another with a gap, which never overlaps and costs one pass over the
///          tree. A tighter packing that interleaves subtrees of different
///          depths would save horizontal space and is a possible refinement, not
///          a correctness fix.
[[nodiscard]] TreeLayout buildLayout(const Tree& left, const Tree& right, const DiffModel& model,
                                     const IFormatProvider& provider, std::stop_token token = {},
                                     LayoutMetrics metrics = {});

/// \brief Finds the innermost node whose span covers a byte offset.
///
/// \param tree The tree to search.
/// \param offset The byte offset into the source the tree was parsed from.
///
/// \returns The deepest node containing \p offset, or kInvalidNode when no node
///          does.
///
/// \remarks Used to turn a click in the text view into a selection in the node
///          view. Descends from the root rather than scanning, so the cost is
///          the depth of the tree rather than its size.
[[nodiscard]] NodeId findNodeAt(const Tree& tree, std::uint32_t offset);

}  // namespace nmxd
