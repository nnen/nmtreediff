#pragma once

/// \file
/// \brief What the two views agree is currently selected.

#include <cstdint>

#include "core/diff.h"
#include "core/tree.h"

namespace nmxd {

/// \brief The node both views are pointing at.
///
/// \remarks One object shared by the text view and the node view, so selecting
///          in either is selecting in both. Whichever view did not make the
///          change notices the raised revision and scrolls to follow.
struct Selection {
    /// \brief Which document the selected node belongs to.
    Side side = Side::Right;

    /// \brief The selected node, or kInvalidNode when nothing is selected.
    NodeId node = kInvalidNode;

    /// \brief Byte offset the selection was made at.
    ///
    /// \remarks Held separately from the node because a click in the text view
    ///          lands on a position first and only then resolves to a node.
    std::uint32_t offset = 0;

    /// \brief Raised every time the selection changes.
    ///
    /// \remarks A view compares this against the revision it last acted on, so
    ///          each view follows a selection it did not make without fighting
    ///          the one that did.
    std::uint64_t revision = 0;

    /// \brief Reports whether anything is selected.
    ///
    /// \returns `true` when a node is selected.
    [[nodiscard]] bool active() const noexcept { return node != kInvalidNode; }

    /// \brief Selects a node and raises the revision.
    ///
    /// \param newSide Which document the node belongs to.
    /// \param newNode The node to select.
    /// \param newOffset The byte offset to remember.
    void select(Side newSide, NodeId newNode, std::uint32_t newOffset) {
        side = newSide;
        node = newNode;
        offset = newOffset;
        ++revision;
    }

    /// \brief Clears the selection and raises the revision.
    void clear() {
        node = kInvalidNode;
        offset = 0;
        ++revision;
    }
};

}  // namespace nmxd
