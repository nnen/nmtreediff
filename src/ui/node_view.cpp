/// \file
/// \brief Implementation of the node view canvas.

#include "ui/node_view.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <imgui.h>

namespace nmxd {

namespace {

/// \brief Card outline and text colour for an added node.
constexpr ImU32 kAddedInk = IM_COL32(96, 200, 140, 255);
/// \brief Card outline and text colour for a deleted node.
constexpr ImU32 kDeletedInk = IM_COL32(226, 110, 105, 255);
/// \brief Card outline and text colour for a modified node.
constexpr ImU32 kModifiedInk = IM_COL32(224, 176, 82, 255);
/// \brief Card outline and text colour for a moved node.
constexpr ImU32 kMovedInk = IM_COL32(168, 143, 224, 255);

/// \brief Card fill behind an unchanged node.
constexpr ImU32 kUnchangedFill = IM_COL32(38, 42, 50, 255);
/// \brief Card outline for an unchanged node.
constexpr ImU32 kUnchangedEdge = IM_COL32(78, 86, 98, 255);
/// \brief Colour of the lines joining a parent to its children.
constexpr ImU32 kEdgeColour = IM_COL32(110, 120, 134, 190);
/// \brief Colour of the dashed line back to where a moved node used to sit.
constexpr ImU32 kGhostColour = IM_COL32(168, 143, 224, 120);
/// \brief Outline drawn around the selected card.
constexpr ImU32 kSelectionColour = IM_COL32(240, 244, 250, 255);
/// \brief Text colour of a title on an unchanged card.
constexpr ImU32 kTitleInk = IM_COL32(220, 226, 234, 255);
/// \brief Text colour of a subtitle, and of a collapsed card's chip.
constexpr ImU32 kSubtitleInk = IM_COL32(150, 158, 170, 255);

/// \brief How opaque a changed card's fill is, out of 255.
constexpr int kChangedFillAlpha = 46;
/// \brief How opaque the outline drawn around a hovered card is.
constexpr int kHoverAlpha = 120;

/// \brief Corner radius of a card, in pixels.
constexpr float kCardRounding = 3.0f;
/// \brief Outline thickness of an unchanged card.
constexpr float kQuietEdgeWidth = 1.0f;
/// \brief Outline thickness of a card that changed, so it reads first.
constexpr float kLoudEdgeWidth = 1.8f;
/// \brief Width of the stripe carrying the provider's own colour.
constexpr float kAccentStripeWidth = 3.0f;
/// \brief Outline thickness drawn around a hovered card.
constexpr float kHoverEdgeWidth = 1.5f;
/// \brief Outline thickness drawn around the selected card.
constexpr float kSelectionEdgeWidth = 2.0f;
/// \brief How far outside a card its selection outline sits, in pixels.
constexpr float kSelectionInset = 2.0f;
/// \brief Corner radius of that selection outline.
constexpr float kSelectionRounding = 4.0f;
/// \brief Half the width of a collapsed card's chip, used to centre it.
constexpr float kChipHalfWidth = 10.0f;
/// \brief Gap between a card and the chip below it.
constexpr float kChipGap = 3.0f;

/// \brief Background of the canvas.
constexpr ImU32 kCanvasColour = IM_COL32(22, 25, 31, 255);

/// \brief Reports whether the left button was released without dragging.
///
/// \returns `true` on the frame a real click finishes.
///
/// \remarks Panning is a left drag on the same canvas, so a press is not
///          enough to know a click was meant: pressing on empty space is how a
///          pan begins. Waiting for the release and asking how far the pointer
///          travelled tells the two apart, and it uses the threshold ImGui
///          itself uses to start a drag, so the two can never disagree about
///          where a click stops and a pan starts.
[[nodiscard]] bool clickedWithoutDragging() {
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        return false;
    }
    const float threshold = io.MouseDragThreshold * io.MouseDragThreshold;
    return io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <= threshold;
}

/// \brief What every card in one pass is drawn against.
///
/// \remarks Gathered into one value because it is the same for every card,
///          and passing seven arguments per card would say nothing about which
///          of them belong together.
struct CardPaint {
    ImVec2 origin;           ///< Top left of the canvas, in screen pixels.
    ImVec2 size;             ///< Size of the canvas, in screen pixels.
    float zoom = 1.0f;       ///< Screen pixels per layout unit.
    bool selected = false;   ///< Whether this card is the selected one.
    bool collapsed = false;  ///< Whether this card is standing in for its subtree.
    bool hoverable = false;  ///< Whether the canvas has the mouse.
    bool withText = false;   ///< Whether the zoom is close enough for text.
};

/// \brief Reports whether a graph direction runs left to right.
///
/// \param direction The direction the layout was built in.
///
/// \returns `true` when depth grows along x.
[[nodiscard]] bool horizontal(GraphDirection direction) {
    return direction == GraphDirection::LeftToRight;
}

/// \brief Returns the point on a card where an edge to a child leaves.
///
/// \param card The parent card.
/// \param direction The direction the layout was built in.
///
/// \returns A point in layout units, centred on the face that looks towards
///          the next level.
[[nodiscard]] ImVec2 exitPoint(const LayoutNode& card, GraphDirection direction) {
    if (horizontal(direction)) {
        return ImVec2(card.x + card.width, card.y + card.height * 0.5f);
    }
    return ImVec2(card.x + card.width * 0.5f, card.y + card.height);
}

/// \brief Returns the point on a card where an edge from its parent
///        arrives.
///
/// \param card The child card.
/// \param direction The direction the layout was built in.
///
/// \returns A point in layout units, centred on the face that looks back
///          towards the previous level.
[[nodiscard]] ImVec2 entryPoint(const LayoutNode& card, GraphDirection direction) {
    if (horizontal(direction)) {
        return ImVec2(card.x, card.y + card.height * 0.5f);
    }
    return ImVec2(card.x + card.width * 0.5f, card.y);
}

/// \brief Below this zoom, cards are drawn as plain boxes with no text.
///
/// \remarks Text is the expensive part of drawing a card, and below this scale
///          it is illegible anyway.
constexpr float kTextZoomThreshold = 0.55f;

/// \brief Size of the minimap along its longest edge, in pixels.
constexpr float kMinimapSize = 150.0f;

/// \brief Smallest a minimap mark may be on either axis, in pixels.
///
/// \remarks Applied to both axes equally, so a change stays visible in a large
///          tree without the mark being stretched out of shape.
constexpr float kMinimapMark = 2.0f;

/// \brief Chooses the ink colour for a change status.
///
/// \param status The node's status.
///
/// \returns The colour to outline and letter the card in.
ImU32 inkFor(NodeStatus status) {
    switch (status) {
        case NodeStatus::Added:
            return kAddedInk;
        case NodeStatus::Deleted:
            return kDeletedInk;
        case NodeStatus::Modified:
            return kModifiedInk;
        case NodeStatus::Moved:
            return kMovedInk;
        case NodeStatus::Unchanged:
            break;
    }
    return kUnchangedEdge;
}

/// \brief Blends a colour towards transparency.
///
/// \param colour The colour to fade.
/// \param alpha The alpha to apply, from 0 to 255.
///
/// \returns The faded colour.
ImU32 withAlpha(ImU32 colour, int alpha) {
    return (colour & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << 24);
}

/// \brief Draws one card's title and subtitle.
///
/// \param draw The draw list to add to.
/// \param layout The layout being drawn, for the sizes it was built in.
/// \param card The card whose text to draw.
/// \param topLeft The card's top left corner, in screen pixels.
/// \param ink The colour the card's status calls for.
/// \param zoom Screen pixels per layout unit.
void drawCardText(ImDrawList* draw, const TreeLayout& layout, const LayoutNode& card,
                  const ImVec2& topLeft, ImU32 ink, float zoom) {
    // The padding and line height come from the layout rather than from a
    // literal here, so a card's text sits where the card was measured for it
    // whatever size the interface is drawing at.
    const float pad = layout.metrics.padding * zoom;
    const float line = layout.metrics.lineHeight * zoom;
    const float textLeft = topLeft.x + pad + kAccentStripeWidth * zoom;

    draw->AddText(ImVec2(textLeft, topLeft.y + pad),
                  card.status == NodeStatus::Unchanged ? kTitleInk : ink, card.title.c_str());
    if (!card.subtitle.empty()) {
        draw->AddText(ImVec2(textLeft, topLeft.y + pad + line), kSubtitleInk,
                      card.subtitle.c_str());
    }
}

/// \brief Draws one card, unless it is off screen.
///
/// \param draw The draw list to add to.
/// \param layout The layout being drawn.
/// \param card The card to draw.
/// \param topLeft The card's top left corner, in screen pixels.
/// \param bottomRight The card's bottom right corner, in screen pixels.
/// \param paint What is the same for every card this pass.
///
/// \returns `true` when the mouse is over this card.
///
/// \remarks Reports the hover rather than recording it, so that the drawing
///          stays a function of what it is given and the view keeps its own
///          state.
[[nodiscard]] bool drawOneCard(ImDrawList* draw, const TreeLayout& layout, const LayoutNode& card,
                               const ImVec2& topLeft, const ImVec2& bottomRight,
                               const CardPaint& paint) {
    // Culled by bounding box, which is what keeps a large tree affordable: only
    // what is on screen is ever drawn.
    if (bottomRight.x < paint.origin.x || topLeft.x > paint.origin.x + paint.size.x ||
        bottomRight.y < paint.origin.y || topLeft.y > paint.origin.y + paint.size.y) {
        return false;
    }

    // The card itself: a fill, an outline that thickens when something changed,
    // and a stripe in the provider's own colour so two node kinds stay
    // distinguishable even when both are unchanged.
    const bool quiet = card.status == NodeStatus::Unchanged;
    const ImU32 ink = inkFor(card.status);
    draw->AddRectFilled(topLeft, bottomRight,
                        quiet ? kUnchangedFill : withAlpha(ink, kChangedFillAlpha), kCardRounding);
    draw->AddRect(topLeft, bottomRight, ink, kCardRounding, 0,
                  quiet ? kQuietEdgeWidth : kLoudEdgeWidth);
    draw->AddRectFilled(topLeft, ImVec2(topLeft.x + kAccentStripeWidth * paint.zoom, bottomRight.y),
                        IM_COL32(card.accent.r, card.accent.g, card.accent.b, 255), kCardRounding);

    // Hover and selection, drawn over the card so neither is hidden by it.
    const bool hovered = paint.hoverable && ImGui::IsMouseHoveringRect(topLeft, bottomRight);
    if (hovered) {
        draw->AddRect(topLeft, bottomRight, withAlpha(kSelectionColour, kHoverAlpha), kCardRounding,
                      0, kHoverEdgeWidth);
    }
    if (paint.selected) {
        draw->AddRect(ImVec2(topLeft.x - kSelectionInset, topLeft.y - kSelectionInset),
                      ImVec2(bottomRight.x + kSelectionInset, bottomRight.y + kSelectionInset),
                      kSelectionColour, kSelectionRounding, 0, kSelectionEdgeWidth);
    }

    if (paint.withText) {
        drawCardText(draw, layout, card, topLeft, ink, paint.zoom);
    }

    // A collapsed card says how much it stands for, so nothing is hidden
    // without the reader being told it is there.
    if (paint.collapsed && !card.children.empty()) {
        const std::string chip = "+" + std::to_string(card.hiddenDescendants);
        const ImVec2 at(topLeft.x + (bottomRight.x - topLeft.x) * 0.5f -
                            kChipHalfWidth * paint.zoom,
                        bottomRight.y + kChipGap * paint.zoom);
        draw->AddText(at, kSubtitleInk, chip.c_str());
    }
    return hovered;
}

/// \brief Draws a dashed line, used for the edge back to a former parent.
///
/// \param draw The draw list to add to.
/// \param from Where the line starts.
/// \param to Where the line ends.
/// \param colour The line colour.
/// \param dash Length of one dash and of the gap that follows it.
void addDashedLine(ImDrawList* draw, const ImVec2& from, const ImVec2& to, ImU32 colour,
                   float dash) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f || dash < 0.5f) {
        return;
    }
    const float stepX = dx / length;
    const float stepY = dy / length;
    for (float at = 0.0f; at < length; at += dash * 2.0f) {
        const float end = std::min(at + dash, length);
        draw->AddLine(ImVec2(from.x + stepX * at, from.y + stepY * at),
                      ImVec2(from.x + stepX * end, from.y + stepY * end), colour, 1.2f);
    }
}

}  // namespace

void NodeView::draw(const DiffSnapshot& snapshot, Selection& selection) {
    if (snapshot.stage == Stage::Failed) {
        ImGui::TextColored(ImVec4(0.88f, 0.45f, 0.43f, 1.0f), "%s", snapshot.message.c_str());
        return;
    }
    if (!snapshot.layout || snapshot.layout->empty()) {
        if (snapshot.stage == Stage::Idle) {
            ImGui::TextDisabled("Nothing open.");
        } else {
            ImGui::TextDisabled("Building the node view...");
        }
        return;
    }

    const TreeLayout& layout = *snapshot.layout;

    // A new layout invalidates a pan and zoom that belonged to the old one.
    const auto stamp = reinterpret_cast<std::uint64_t>(&layout);
    if (stamp != layoutStamp_) {
        layoutStamp_ = stamp;
        framed_ = false;
        collapsed_.clear();
        currentChange_ = -1;
        collapseUnchanged(snapshot);
    }

    if (ImGui::SmallButton("Fit")) {
        framed_ = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Expand all")) {
        expandAll();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Collapse unchanged")) {
        collapseUnchanged(snapshot);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu nodes  |  drag to pan, wheel to zoom", layout.size());

    drawCanvas(layout, snapshot, selection);
}

void NodeView::drawCanvas(const TreeLayout& layout, const DiffSnapshot& snapshot,
                          Selection& selection) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 64.0f);
    size.y = std::max(size.y, 64.0f);

    canvasX_ = origin.x;
    canvasY_ = origin.y;
    canvasWidth_ = size.x;
    canvasHeight_ = size.y;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), kCanvasColour);
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    if (!framed_) {
        // Fit the whole drawing, but never magnify past life size on a small
        // tree, which would look like a mistake rather than a choice.
        const float scaleX = size.x / std::max(layout.width, 1.0f);
        const float scaleY = size.y / std::max(layout.height, 1.0f);
        zoom_ = std::clamp(std::min(scaleX, scaleY) * 0.92f, 0.05f, 1.0f);
        panX_ = (size.x - layout.width * zoom_) * 0.5f;
        // Centred vertically too, so a small tree sits in the canvas rather
        // than clinging to the top of a mostly empty one.
        panY_ = std::max(24.0f, (size.y - layout.height * zoom_) * 0.5f);
        framed_ = true;
    }

    followSelection(layout, selection);

    ImGui::InvisibleButton("canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();

    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        panX_ += io.MouseDelta.x;
        panY_ += io.MouseDelta.y;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        // Zoom about the cursor, so the thing under the pointer stays under it.
        const float previous = zoom_;
        zoom_ = std::clamp(zoom_ * std::pow(1.12f, io.MouseWheel), 0.05f, 3.0f);
        const float localX = io.MousePos.x - origin.x;
        const float localY = io.MousePos.y - origin.y;
        panX_ = localX - (localX - panX_) * (zoom_ / previous);
        panY_ = localY - (localY - panY_) * (zoom_ / previous);
    }

    const auto toScreen = [&](float x, float y) {
        return ImVec2(origin.x + panX_ + x * zoom_, origin.y + panY_ + y * zoom_);
    };

    hovered_ = kInvalidLayout;
    const LayoutId selected =
        selection.active() ? layout.find(selection.side, selection.node) : kInvalidLayout;

    // Edges first, so a card always sits on top of the lines reaching it.
    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        const LayoutNode& card = layout.nodes[i];
        if (card.parent == kInvalidLayout || hiddenByCollapse(layout, static_cast<LayoutId>(i))) {
            continue;
        }
        const LayoutNode& parent = layout.nodes[card.parent];
        const ImVec2 exit = exitPoint(parent, layout.direction);
        const ImVec2 entry = entryPoint(card, layout.direction);
        const ImVec2 from = toScreen(exit.x, exit.y);
        const ImVec2 to = toScreen(entry.x, entry.y);
        if (std::max(from.y, to.y) < origin.y || std::min(from.y, to.y) > origin.y + size.y) {
            continue;
        }
        // The curve leaves and arrives along the axis that grows with depth, so
        // an edge reads as a descent whichever way the graph runs.
        const ImVec2 midFrom = horizontal(layout.direction)
                                   ? ImVec2((from.x + to.x) * 0.5f, from.y)
                                   : ImVec2(from.x, (from.y + to.y) * 0.5f);
        const ImVec2 midTo = horizontal(layout.direction)
                                 ? ImVec2((from.x + to.x) * 0.5f, to.y)
                                 : ImVec2(to.x, (from.y + to.y) * 0.5f);
        draw->AddBezierCubic(from, midFrom, midTo, to, kEdgeColour, 1.4f);
    }

    // A ghost edge shows where a moved node came from, so a move reads as one
    // node that went somewhere rather than two unrelated changes.
    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        const LayoutNode& card = layout.nodes[i];
        if (card.movedFrom == kInvalidLayout || hiddenByCollapse(layout, static_cast<LayoutId>(i))) {
            continue;
        }
        const LayoutNode& from = layout.nodes[card.movedFrom];
        addDashedLine(draw, toScreen(from.x + from.width * 0.5f, from.y + from.height * 0.5f),
                      toScreen(card.x + card.width * 0.5f, card.y + card.height * 0.5f),
                      kGhostColour, 6.0f);
    }

    const bool drawText = zoom_ >= kTextZoomThreshold;

    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        const auto id = static_cast<LayoutId>(i);
        const LayoutNode& card = layout.nodes[i];
        if (hiddenByCollapse(layout, id)) {
            continue;
        }

        CardPaint paint;
        paint.origin = origin;
        paint.size = size;
        paint.zoom = zoom_;
        paint.selected = id == selected;
        paint.collapsed = collapsed_.count(id) != 0;
        paint.hoverable = hovered;
        paint.withText = drawText;

        if (drawOneCard(draw, layout, card, toScreen(card.x, card.y),
                        toScreen(card.x + card.width, card.y + card.height), paint)) {
            hovered_ = id;
        }
    }

    if (hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left) && hovered_ != kInvalidLayout) {
        const LayoutNode& card = layout.nodes[hovered_];
        const Tree& tree = card.side == Side::Left ? *snapshot.leftTree : *snapshot.rightTree;
        selection.select(card.side, card.node, tree.node(card.node).span.begin);
        followedRevision_ = selection.revision;
    }

    // Clicking the canvas itself clears the selection, the way clicking empty
    // space does anywhere else. It waits for the release rather than acting on
    // the press, because a press on empty space is also how a pan begins and
    // losing the selection every time the view is dragged would be worse than
    // not being able to clear it at all.
    if (hovered && hovered_ == kInvalidLayout && selection.active() && clickedWithoutDragging()) {
        selection.clear();
        followedRevision_ = selection.revision;
    }
    if (hovered && ImGui::IsItemClicked(ImGuiMouseButton_Right) && hovered_ != kInvalidLayout) {
        if (collapsed_.count(hovered_) != 0) {
            collapsed_.erase(hovered_);
        } else if (!layout.nodes[hovered_].children.empty()) {
            collapsed_.insert(hovered_);
        }
    }

    draw->PopClipRect();
    drawMinimap(layout);

    if (hovered_ != kInvalidLayout) {
        const LayoutNode& card = layout.nodes[hovered_];
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(card.title.c_str());
        if (!card.subtitle.empty()) {
            ImGui::TextDisabled("%s", card.subtitle.c_str());
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", describe(card.status));
        if (!card.children.empty()) {
            ImGui::TextDisabled("right-click to %s %u below",
                                collapsed_.count(hovered_) != 0 ? "expand" : "collapse",
                                card.hiddenDescendants);
        }
        ImGui::EndTooltip();
    }
}

void NodeView::drawMinimap(const TreeLayout& layout) {
    if (layout.width <= 0.0f || layout.height <= 0.0f) {
        return;
    }

    const float scale = kMinimapSize / std::max(layout.width, layout.height);
    const float mapWidth = layout.width * scale;
    const float mapHeight = layout.height * scale;

    const ImVec2 at(canvasX_ + canvasWidth_ - mapWidth - 12.0f,
                    canvasY_ + canvasHeight_ - mapHeight - 12.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + mapWidth, at.y + mapHeight), IM_COL32(16, 18, 23, 210),
                        3.0f);
    draw->AddRect(at, ImVec2(at.x + mapWidth, at.y + mapHeight), IM_COL32(90, 98, 112, 200), 3.0f);

    // Only changed nodes are plotted: the minimap answers "where are the
    // changes", and plotting everything would answer nothing.
    for (const LayoutNode& card : layout.nodes) {
        if (card.status == NodeStatus::Unchanged) {
            continue;
        }
        // Both axes are scaled and both have the same floor. Scaling only the
        // width, as this once did, stretched a card that is roughly twice as
        // wide as it is tall into a mark ten times as wide as it was tall.
        const ImVec2 dot(at.x + card.x * scale, at.y + card.y * scale);
        draw->AddRectFilled(dot,
                            ImVec2(dot.x + std::max(kMinimapMark, card.width * scale),
                                   dot.y + std::max(kMinimapMark, card.height * scale)),
                            inkFor(card.status));
    }

    // The viewport rectangle, so the reader can see where they are looking. It
    // is clamped to the minimap: when the view is larger than the drawing, an
    // unclamped rectangle sprawls across the canvas and reads as a bug.
    const float viewX = -panX_ / std::max(zoom_, 0.001f);
    const float viewY = -panY_ / std::max(zoom_, 0.001f);
    const float viewW = canvasWidth_ / std::max(zoom_, 0.001f);
    const float viewH = canvasHeight_ / std::max(zoom_, 0.001f);

    const ImVec2 viewTopLeft(std::clamp(at.x + viewX * scale, at.x, at.x + mapWidth),
                             std::clamp(at.y + viewY * scale, at.y, at.y + mapHeight));
    const ImVec2 viewBottomRight(
        std::clamp(at.x + (viewX + viewW) * scale, at.x, at.x + mapWidth),
        std::clamp(at.y + (viewY + viewH) * scale, at.y, at.y + mapHeight));

    if (viewBottomRight.x - viewTopLeft.x > 2.0f && viewBottomRight.y - viewTopLeft.y > 2.0f) {
        draw->AddRect(viewTopLeft, viewBottomRight, IM_COL32(240, 244, 250, 150), 2.0f);
    }
}

bool NodeView::hiddenByCollapse(const TreeLayout& layout, LayoutId id) const {
    if (collapsed_.empty()) {
        return false;
    }
    // Hidden when any ancestor is collapsed. Walking up is bounded by the depth
    // of the tree rather than its size.
    for (LayoutId at = layout.nodes[id].parent; at != kInvalidLayout;
         at = layout.nodes[at].parent) {
        if (collapsed_.count(at) != 0) {
            return true;
        }
    }
    return false;
}

void NodeView::expandAll() { collapsed_.clear(); }

void NodeView::collapseUnchanged(const DiffSnapshot& snapshot) {
    collapsed_.clear();
    if (!snapshot.layout) {
        return;
    }
    const TreeLayout& layout = *snapshot.layout;
    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        const LayoutNode& card = layout.nodes[i];
        if (card.children.empty() || card.subtreeChanged) {
            continue;
        }
        // Only the topmost unchanged card is collapsed; collapsing one already
        // inside a collapsed subtree would achieve nothing.
        if (!hiddenByCollapse(layout, static_cast<LayoutId>(i))) {
            collapsed_.insert(static_cast<LayoutId>(i));
        }
    }
}

void NodeView::centreOn(const TreeLayout& layout, LayoutId id) {
    if (id == kInvalidLayout || id >= layout.nodes.size()) {
        return;
    }
    const LayoutNode& card = layout.nodes[id];
    panX_ = canvasWidth_ * 0.5f - (card.x + card.width * 0.5f) * zoom_;
    panY_ = canvasHeight_ * 0.5f - (card.y + card.height * 0.5f) * zoom_;
    framed_ = true;
}

void NodeView::followSelection(const TreeLayout& layout, const Selection& selection) {
    if (!selection.active() || selection.revision == followedRevision_) {
        return;
    }
    followedRevision_ = selection.revision;

    const LayoutId id = layout.find(selection.side, selection.node);
    if (id == kInvalidLayout) {
        return;
    }
    // Reveal the node by expanding whatever was hiding it, then centre on it.
    for (LayoutId at = layout.nodes[id].parent; at != kInvalidLayout;
         at = layout.nodes[at].parent) {
        collapsed_.erase(at);
    }
    centreOn(layout, id);
}

void NodeView::goToNextChange(const DiffSnapshot& snapshot, Selection& selection) {
    if (!snapshot.layout || !snapshot.treeDiff || snapshot.treeDiff->changes.empty()) {
        return;
    }
    const auto& changes = snapshot.treeDiff->changes;
    currentChange_ =
        std::min<std::int64_t>(currentChange_ + 1, static_cast<std::int64_t>(changes.size()) - 1);

    const Change& change = changes[static_cast<std::size_t>(currentChange_)];
    const bool onRight = change.right != kInvalidNode;
    const Side side = onRight ? Side::Right : Side::Left;
    const NodeId node = onRight ? change.right : change.left;
    const Tree& tree = onRight ? *snapshot.rightTree : *snapshot.leftTree;

    selection.select(side, node, tree.node(node).span.begin);
}

void NodeView::goToPreviousChange(const DiffSnapshot& snapshot, Selection& selection) {
    if (!snapshot.layout || !snapshot.treeDiff || snapshot.treeDiff->changes.empty()) {
        return;
    }
    currentChange_ = std::max<std::int64_t>(currentChange_ - 1, 0);

    const Change& change = snapshot.treeDiff->changes[static_cast<std::size_t>(currentChange_)];
    const bool onRight = change.right != kInvalidNode;
    const Side side = onRight ? Side::Right : Side::Left;
    const NodeId node = onRight ? change.right : change.left;
    const Tree& tree = onRight ? *snapshot.rightTree : *snapshot.leftTree;

    selection.select(side, node, tree.node(node).span.begin);
}

}  // namespace nmxd
