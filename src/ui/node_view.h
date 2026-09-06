#pragma once

/// \file
/// \brief The node view: both trees drawn as one graph, coloured by change.

#include <cstdint>
#include <optional>
#include <unordered_set>

#include "core/snapshot.h"
#include "ui/selection.h"

namespace nmxd {

/// \brief Draws the union of both trees as a pannable, zoomable graph.
///
/// \remarks Drawn through the ImGui draw list rather than with widgets, so
///          panning, zooming and culling stay under our control. Positions come
///          from the layout computed on a worker; this class only maps them to
///          the screen and decides what is worth drawing.
class NodeView {
public:
    /// \brief Draws the view into the current ImGui window.
    ///
    /// \param snapshot What to draw. Earlier stages render an explanatory state
    ///        rather than an empty canvas.
    /// \param selection The selection shared with the text view, read and
    ///        written.
    void draw(const DiffSnapshot& snapshot, Selection& selection);

    /// \brief Centres the view on the next changed node.
    ///
    /// \param snapshot The snapshot being displayed.
    /// \param selection The selection to move.
    void goToNextChange(const DiffSnapshot& snapshot, Selection& selection);

    /// \brief Centres the view on the previous changed node.
    ///
    /// \param snapshot The snapshot being displayed.
    /// \param selection The selection to move.
    void goToPreviousChange(const DiffSnapshot& snapshot, Selection& selection);

    /// \brief Expands every collapsed subtree.
    void expandAll();

    /// \brief Collapses every subtree with nothing changed inside it.
    ///
    /// \param snapshot The snapshot being displayed.
    void collapseUnchanged(const DiffSnapshot& snapshot);

    /// \brief Frames the whole graph on the next drawn frame.
    void fit();

    /// \brief Collects a graph direction asked for from the context menu.
    ///
    /// \returns The direction the reader chose, or nothing when they have not
    ///          chosen one since the last call.
    ///
    /// \remarks Direction belongs to the window, which owns the session the
    ///          layout is rebuilt from, so the view asks rather than acts. Polled
    ///          once a frame, the same way the file dialog's answer is.
    [[nodiscard]] std::optional<GraphDirection> takeDirectionRequest();

private:
    void drawCanvas(const TreeLayout& layout, const DiffSnapshot& snapshot,
                    Selection& selection);
    void drawMinimap(const TreeLayout& layout);

    /// \brief Draws the canvas context menu, if it is open.
    ///
    /// \param layout The layout being drawn.
    /// \param snapshot The comparison being shown.
    void drawContextMenu(const TreeLayout& layout, const DiffSnapshot& snapshot);

    /// \brief Draws the part of the context menu about one card.
    ///
    /// \param layout The layout being drawn.
    void drawCardMenuItems(const TreeLayout& layout);

    /// \brief Draws the graph direction items of the context menu.
    ///
    /// \param layout The layout being drawn, for the direction in force.
    /// \param snapshot The comparison, for whether the format decides.
    void drawDirectionMenuItems(const TreeLayout& layout, const DiffSnapshot& snapshot);

    /// \brief Hides or reveals one card's children.
    ///
    /// \param layout The layout being drawn.
    /// \param id The card to toggle.
    ///
    /// \remarks A card with no children is left alone: collapsing it would
    ///          hide nothing and leave a chip claiming otherwise.
    void toggleCollapse(const TreeLayout& layout, LayoutId id);
    void centreOn(const TreeLayout& layout, LayoutId id);
    void followSelection(const TreeLayout& layout, const Selection& selection);
    [[nodiscard]] bool hiddenByCollapse(const TreeLayout& layout, LayoutId id) const;

    float zoom_ = 1.0f;
    float panX_ = 0.0f;
    float panY_ = 0.0f;
    bool framed_ = false;         // whether the first fit-to-view has happened
    std::uint64_t layoutStamp_ = 0;  // which layout the pan and zoom belong to

    LayoutId hovered_ = kInvalidLayout;

    /// \brief The card the context menu was opened on, or kInvalidLayout.
    ///
    /// \remarks Remembered at the click rather than read while the menu is
    ///          drawn, because by then the pointer has moved onto the menu and
    ///          no card is under it.
    LayoutId menuTarget_ = kInvalidLayout;

    /// \brief A direction asked for from the menu, until it is collected.
    std::optional<GraphDirection> directionRequest_;
    std::int64_t currentChange_ = -1;
    std::uint64_t followedRevision_ = 0;

    /// \brief Cards whose children are hidden.
    std::unordered_set<LayoutId> collapsed_;

    /// \brief Canvas size and origin from the last frame, for hit testing.
    float canvasX_ = 0.0f;
    float canvasY_ = 0.0f;
    float canvasWidth_ = 0.0f;
    float canvasHeight_ = 0.0f;
};

}  // namespace nmxd
