#pragma once

/// \file
/// \brief The node view: both trees drawn as one graph, coloured by change.

#include <cstdint>
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

private:
    void drawCanvas(const TreeLayout& layout, const DiffSnapshot& snapshot,
                    Selection& selection);
    void drawMinimap(const TreeLayout& layout);
    void centreOn(const TreeLayout& layout, LayoutId id);
    void followSelection(const TreeLayout& layout, const Selection& selection);
    [[nodiscard]] bool hiddenByCollapse(const TreeLayout& layout, LayoutId id) const;

    float zoom_ = 1.0f;
    float panX_ = 0.0f;
    float panY_ = 0.0f;
    bool framed_ = false;         // whether the first fit-to-view has happened
    std::uint64_t layoutStamp_ = 0;  // which layout the pan and zoom belong to

    LayoutId hovered_ = kInvalidLayout;
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
