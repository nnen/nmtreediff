/// \file
/// \brief Implementation of the node view canvas.

#include "ui/node_view.h"

#include <algorithm>
#include <cmath>
#include <string>
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

/// \brief How much thicker a changed card's outline is than a quiet one's.
constexpr float kLoudEdgeFactor = 5.0f;

/// \brief Outline thickness of a card that changed, so it reads first.
///
/// \remarks Derived rather than written out, so the two cannot drift apart
///          and the ratio between them is the thing stated.
constexpr float kLoudEdgeWidth = kQuietEdgeWidth * kLoudEdgeFactor;
/// \brief Width of the stripe carrying the provider's own colour.
constexpr float kAccentStripeWidth = 3.0f;
/// \brief Outline thickness drawn around a hovered card.
constexpr float kHoverEdgeWidth = 1.5f;
/// \brief How much thicker the selection halo is than a quiet outline.
constexpr float kSelectionEdgeFactor = 20.0f;

/// \brief Thickness of the halo drawn behind the selected card.
///
/// \remarks Drawn before the card rather than over it, so the card's own
///          border and colour sit on top and the halo shows only as a ring
///          around the outside. A band this heavy over the card would bury the
///          one thing the card exists to say, which is what happened to it.
constexpr float kSelectionEdgeWidth = kQuietEdgeWidth * kSelectionEdgeFactor;
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
    ImDrawFlags corners = 0; ///< Which corners are rounded; a stack member squares the shared ones.
    float textSize = 0.0f;   ///< Pixel size to draw text at, for this zoom.
    int textAlpha = 0;       ///< How visible text is at that size; zero skips it.
    bool failed = false;     ///< Whether a shaping job failed under this node.
};

/// \brief Colour of the mark on a card a shaping job failed under.
///
/// \remarks Violet, which no diff status uses, so a broken script is never
///          read as a change. The same colour the text view marks the
///          element in.
constexpr ImU32 kFailedMark = IM_COL32(190, 90, 210, 255);

/// \brief Side of the failure mark, in layout units before zoom.
constexpr float kFailedMarkSize = 7.0f;

/// \brief Draws the mark that says a shaping job failed under a card.
///
/// \param draw The draw list to add to.
/// \param topLeft The card's top left corner, in screen pixels.
/// \param bottomRight The card's bottom right corner, in screen pixels.
/// \param zoom Screen pixels per layout unit.
///
/// \remarks A small triangle in the top right corner, because a change
///          reported under this node may be an artefact of the failure rather
///          than of the file, and a reader should see that before trusting
///          the card.
void drawFailedMark(ImDrawList* draw, const ImVec2& topLeft, const ImVec2& bottomRight,
                    float zoom) {
    const float side = std::max(4.0f, kFailedMarkSize * zoom);
    const ImVec2 corner(bottomRight.x - 1.0f, topLeft.y + 1.0f);
    draw->AddTriangleFilled(corner, ImVec2(corner.x - side, corner.y),
                            ImVec2(corner.x, corner.y + side), kFailedMark);
}

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

/// \brief Reports whether stacks run along the depth axis in a layout.
///
/// \param layout The layout being drawn.
///
/// \returns `true` when a stacked card sits further along the depth axis than
///          the card it stacks on; `false` when it sits across it.
[[nodiscard]] bool stacksAlongDepth(const TreeLayout& layout) {
    return layout.stacking == nmxd::StackDirection::AlongDepth || !horizontal(layout.direction);
}

/// \brief Returns the card an edge from the parent visually arrives at.
///
/// \param layout The layout being drawn.
/// \param card The card the edge logically reaches.
///
/// \returns \p card, or the bottom member of its stack when the format pinned
///          the entry there and the stack stands across the depth axis.
///
/// \remarks Along the depth axis the head's face is the block's face and
///          there is nothing to choose, so the pin only matters when the
///          block stands across the line of flow, which with vertical stacks
///          means left to right.
[[nodiscard]] const LayoutNode& entryCard(const TreeLayout& layout, const LayoutNode& card) {
    if (layout.entryPin == nmxd::StackEntryPin::Bottom && !stacksAlongDepth(layout) &&
        card.stackBottom != kInvalidLayout) {
        return layout.nodes[card.stackBottom];
    }
    return card;
}

/// \brief Returns the card a collapse asked of one card acts on.
///
/// \param layout The layout being drawn.
/// \param id The card the reader pointed at.
///
/// \returns The bottom member of the card's stack, or the card itself when
///          it is in none.
///
/// \remarks A stack collapses as a unit: what folds away is the block's
///          subtree, never part of the block. Collapsing a member in the
///          middle would cut the block and leave a chip hanging inside it.
[[nodiscard]] LayoutId collapseAnchor(const TreeLayout& layout, LayoutId id) {
    const LayoutId bottom = layout.nodes[id].stackBottom;
    return bottom == kInvalidLayout ? id : bottom;
}

/// \brief Says which of a card's corners are rounded.
///
/// \param layout The layout being drawn, for the stacking direction and the
///        card's children.
/// \param card The card to draw.
///
/// \returns Draw flags naming the rounded corners.
///
/// \remarks A card in a stack squares the corners it shares with the member
///          above or below it, so the stack reads as one block with a line
///          across it rather than as cards resting on one another. The shared
///          edge is the bottom one for a vertical stack, and the right one
///          when stacks follow a left-to-right graph instead.
[[nodiscard]] ImDrawFlags cornersFor(const TreeLayout& layout, const LayoutNode& card) {
    const bool joinedAbove = card.stackedOnParent;
    const bool joinedBelow =
        card.children.size() == 1 && layout.nodes[card.children[0]].stackedOnParent;
    const bool sideways = layout.stacking == nmxd::StackDirection::AlongDepth &&
                          horizontal(layout.direction);
    if (joinedAbove && joinedBelow) {
        return ImDrawFlags_RoundCornersNone;
    }
    if (joinedAbove) {
        return sideways ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersBottom;
    }
    if (joinedBelow) {
        return sideways ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersTop;
    }
    return ImDrawFlags_RoundCornersAll;
}

/// \brief Keeps only the left-hand corners of a set of rounding flags.
///
/// \param corners The card's rounding flags.
///
/// \returns Flags rounding the card's left corners that \p corners rounds,
///          or none.
///
/// \remarks For the accent stripe, which sits against the card's left edge
///          and follows the card's corners there while staying square where
///          it meets the fill.
[[nodiscard]] ImDrawFlags leftCornersOf(ImDrawFlags corners) {
    const ImDrawFlags left =
        corners & (ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft);
    return left == 0 ? ImDrawFlags_RoundCornersNone : left;
}

/// \brief The text size, in pixels, below which card text is not drawn.
///
/// \remarks Text scales with the zoom, so what decides whether it is worth
///          drawing is the size it would come out at rather than the zoom
///          itself. Under five pixels a glyph is a smudge, and text is the
///          expensive part of drawing a card.
constexpr float kTextFadeStartPixels = 5.0f;

/// \brief The text size, in pixels, at which card text is fully opaque.
///
/// \remarks Fading between the two sizes rather than switching at one means
///          a wheel zoom never pops the labels in and out.
constexpr float kTextFadeEndPixels = 8.0f;

/// \brief The step card text sizes are rounded to, in pixels.
///
/// \remarks The font atlas rasterises each distinct size it is asked for,
///          and a wheel zoom passes through hundreds of sizes on its way
///          anywhere. Half a pixel is well under what the eye picks up and
///          keeps the atlas to a few dozen sizes over a session.
constexpr float kTextSizeStep = 0.5f;

/// \brief Chooses the pixel size card text is drawn at for one zoom.
///
/// \param zoom Screen pixels per layout unit.
///
/// \returns The interface font size scaled by the zoom, rounded to the step.
///
/// \remarks The layout measured every card in character cells of the
///          interface font at zoom one, so text at the font's size times the
///          zoom fits its card at every zoom exactly as it does at one.
[[nodiscard]] float textSizeFor(float zoom) {
    return std::round(ImGui::GetFontSize() * zoom / kTextSizeStep) * kTextSizeStep;
}

/// \brief Says how visible card text is at one pixel size.
///
/// \param size The text size in pixels.
///
/// \returns An alpha from 0, not drawn, to 255, fully opaque.
[[nodiscard]] int textAlphaFor(float size) {
    if (size <= kTextFadeStartPixels) {
        return 0;
    }
    if (size >= kTextFadeEndPixels) {
        return 255;
    }
    return static_cast<int>(255.0f * (size - kTextFadeStartPixels) /
                            (kTextFadeEndPixels - kTextFadeStartPixels));
}

/// \brief How sharply the view settles when it glides, per second.
///
/// \remarks Used as an exponential decay rather than a fixed step, so the
///          glide takes the same time whatever the frame rate: vertical sync is
///          off during a timing run and on the rest of the time, and a movement
///          that ran at frame speed would be a blur in one and a crawl in the
///          other.
constexpr float kGlideRate = 14.0f;

/// \brief How close in pixels counts as arrived.
///
/// \remarks Exponential decay never quite reaches its target, so without a
///          floor the view would creep for ever and never be still.
constexpr float kGlideSettled = 0.5f;

/// \brief Identifier of the canvas context menu popup.
constexpr const char* kContextMenuId = "nodeContextMenu";

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

/// \brief Scales a colour's own alpha by a factor.
///
/// \param colour The colour, in ImGui's packed form.
/// \param alpha The factor, from 0 to 255 for none to unchanged.
///
/// \returns The colour with its alpha multiplied, so a translucent ink stays
///          translucent rather than being made opaque.
ImU32 faded(ImU32 colour, int alpha) {
    const int own = static_cast<int>(colour >> 24);
    return withAlpha(colour, own * std::clamp(alpha, 0, 255) / 255);
}

/// \brief Draws one card's title and subtitle.
///
/// \param draw The draw list to add to.
/// \param layout The layout being drawn, for the sizes it was built in.
/// \param card The card whose text to draw.
/// \param topLeft The card's top left corner, in screen pixels.
/// \param ink The colour the card's status calls for.
/// \param paint What is the same for every card this pass, for the zoom and
///        the text size and visibility that follow from it.
///
/// \remarks The text is drawn at the size the zoom asks for rather than at
///          the interface size. The font atlas rasterises any size on demand,
///          so it is crisp at every zoom rather than a stretched copy of one.
void drawCardText(ImDrawList* draw, const TreeLayout& layout, const LayoutNode& card,
                  const ImVec2& topLeft, ImU32 ink, const CardPaint& paint) {
    // The padding and line height come from the layout rather than from a
    // literal here, so a card's text sits where the card was measured for it
    // whatever size the interface is drawing at.
    const float pad = layout.metrics.padding * paint.zoom;
    const float line = layout.metrics.lineHeight * paint.zoom;
    const float textLeft = topLeft.x + pad + kAccentStripeWidth * paint.zoom;
    ImFont* font = ImGui::GetFont();

    const ImU32 titleInk = card.status == NodeStatus::Unchanged ? kTitleInk : ink;
    draw->AddText(font, paint.textSize, ImVec2(textLeft, topLeft.y + pad),
                  faded(titleInk, paint.textAlpha), card.title.c_str());
    if (!card.subtitle.empty()) {
        draw->AddText(font, paint.textSize, ImVec2(textLeft, topLeft.y + pad + line),
                      faded(kSubtitleInk, paint.textAlpha), card.subtitle.c_str());
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

    // The selection halo first, so everything the card draws for itself lands on
    // top of it. Half its width falls inside the card and is covered by the
    // fill, which is what leaves a clean ring outside.
    if (paint.selected) {
        draw->AddRect(topLeft, bottomRight, kSelectionColour, kCardRounding, paint.corners,
                      kSelectionEdgeWidth);
    }

    // The card itself: a fill, an outline that thickens when something changed,
    // and a stripe in the provider's own colour so two node kinds stay
    // distinguishable even when both are unchanged.
    const bool quiet = card.status == NodeStatus::Unchanged;
    const ImU32 ink = inkFor(card.status);
    draw->AddRectFilled(topLeft, bottomRight,
                        quiet ? kUnchangedFill : withAlpha(ink, kChangedFillAlpha), kCardRounding,
                        paint.corners);
    draw->AddRect(topLeft, bottomRight, ink, kCardRounding, paint.corners,
                  quiet ? kQuietEdgeWidth : kLoudEdgeWidth);
    draw->AddRectFilled(topLeft, ImVec2(topLeft.x + kAccentStripeWidth * paint.zoom, bottomRight.y),
                        IM_COL32(card.accent.r, card.accent.g, card.accent.b, 255), kCardRounding,
                        leftCornersOf(paint.corners));

    // Hover is drawn over the card, unlike the selection halo underneath it,
    // because it is a light hint rather than a standing mark.
    const bool hovered = paint.hoverable && ImGui::IsMouseHoveringRect(topLeft, bottomRight);
    if (hovered) {
        draw->AddRect(topLeft, bottomRight, withAlpha(kSelectionColour, kHoverAlpha), kCardRounding,
                      paint.corners, kHoverEdgeWidth);
    }

    if (paint.textAlpha > 0) {
        drawCardText(draw, layout, card, topLeft, ink, paint);
    }
    if (paint.failed) {
        drawFailedMark(draw, topLeft, bottomRight, paint.zoom);
    }

    // A collapsed card says how much it stands for, so nothing is hidden
    // without the reader being told it is there.
    if (paint.collapsed && !card.children.empty() && paint.textAlpha > 0) {
        const std::string chip = "+" + std::to_string(card.hiddenDescendants);
        const ImVec2 at(topLeft.x + (bottomRight.x - topLeft.x) * 0.5f -
                            kChipHalfWidth * paint.zoom,
                        bottomRight.y + kChipGap * paint.zoom);
        draw->AddText(ImGui::GetFont(), paint.textSize, at, faded(kSubtitleInk, paint.textAlpha),
                      chip.c_str());
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
        touched_ = false;
        collapsed_.clear();
        currentChange_ = -1;
        // A card id from the old layout names a different card in this one.
        menuTarget_ = kInvalidLayout;
        collapseUnchanged(snapshot);
        noteFailures(snapshot);
    }

    if (ImGui::SmallButton("Fit")) {
        fit();
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
    ImGui::TextDisabled("%zu nodes  |  drag to pan, wheel to zoom, right-click for more",
                        layout.size());

    handleKeys(snapshot, selection);
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

    // Fitted once, and again whenever the canvas changes size before the
    // reader has taken hold: the size a docked window reports on its first
    // frame is a stand-in, and a layout fitted to it would sit in a corner.
    const bool resized = std::abs(size.x - framedWidth_) > 0.5f ||
                         std::abs(size.y - framedHeight_) > 0.5f;
    if (!framed_ || (!touched_ && resized)) {
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
        framedWidth_ = size.x;
        framedHeight_ = size.y;
        cancelGlide();
    }

    advanceGlide();
    followSelection(layout, snapshot, selection);

    ImGui::InvisibleButton("canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();

    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        cancelGlide();
        touched_ = true;
        panX_ += io.MouseDelta.x;
        panY_ += io.MouseDelta.y;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        // Zoom about the cursor, so the thing under the pointer stays under it.
        cancelGlide();
        touched_ = true;
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

    // Edges first, so a card always sits on top of the lines reaching it. A
    // card stacked on its parent touches it, and the touching outlines are
    // the divider: an edge there would say the two are apart.
    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        const LayoutNode& card = layout.nodes[i];
        if (card.parent == kInvalidLayout || card.stackedOnParent ||
            hiddenByCollapse(layout, static_cast<LayoutId>(i))) {
            continue;
        }
        const LayoutNode& parent = layout.nodes[card.parent];
        const ImVec2 exit = exitPoint(parent, layout.direction);
        const ImVec2 entry = entryPoint(entryCard(layout, card), layout.direction);
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

    const float textSize = textSizeFor(zoom_);
    const int textAlpha = textAlphaFor(textSize);

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
        paint.corners = cornersFor(layout, card);
        paint.textSize = textSize;
        paint.textAlpha = textAlpha;
        paint.failed = failedUnder(card);

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
    // Double click is the usual way to open and close a node in a tree, and it
    // costs nothing: the first click of the pair has already selected the card.
    if (hovered && hovered_ != kInvalidLayout &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        toggleCollapse(layout, hovered_);
    }

    // The card under the pointer is remembered now, while there still is one.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        menuTarget_ = hovered_;
        ImGui::OpenPopup(kContextMenuId);
    }

    draw->PopClipRect();
    drawContextMenu(layout, snapshot);
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
        // A collapse acts at the bottom of the card's stack, so the tooltip
        // counts what that would hide.
        const LayoutId anchor = collapseAnchor(layout, hovered_);
        const LayoutNode& anchored = layout.nodes[anchor];
        if (!anchored.children.empty()) {
            ImGui::TextDisabled("right-click to %s %u below",
                                collapsed_.count(anchor) != 0 ? "expand" : "collapse",
                                anchored.hiddenDescendants);
        }
        ImGui::EndTooltip();
    }
}

void NodeView::toggleCollapse(const TreeLayout& layout, LayoutId id) {
    if (id == kInvalidLayout) {
        return;
    }
    id = collapseAnchor(layout, id);
    if (collapsed_.count(id) != 0) {
        collapsed_.erase(id);
    } else if (!layout.nodes[id].children.empty()) {
        collapsed_.insert(id);
    }
}

void NodeView::fit() {
    framed_ = false;
    touched_ = false;
}

std::optional<GraphDirection> NodeView::takeDirectionRequest() {
    const std::optional<GraphDirection> request = directionRequest_;
    directionRequest_.reset();
    return request;
}

void NodeView::drawCardMenuItems(const TreeLayout& layout) {
    if (menuTarget_ == kInvalidLayout || menuTarget_ >= layout.size()) {
        return;
    }

    const LayoutNode& card = layout.nodes[menuTarget_];
    const LayoutId anchor = collapseAnchor(layout, menuTarget_);
    const bool collapsed = collapsed_.count(anchor) != 0;

    // Named after the card, so the menu says which node it is about rather than
    // leaving the reader to remember what was under the pointer. The collapse
    // itself acts at the bottom of the card's stack.
    const std::string label =
        (collapsed ? "Expand " : "Collapse ") + (card.title.empty() ? "node" : card.title);
    if (ImGui::MenuItem(label.c_str(), nullptr, false,
                        collapsed || !layout.nodes[anchor].children.empty())) {
        toggleCollapse(layout, menuTarget_);
    }
    ImGui::Separator();
}

void NodeView::drawDirectionMenuItems(const TreeLayout& layout, const DiffSnapshot& snapshot) {
    if (!ImGui::BeginMenu("Graph direction")) {
        return;
    }

    // A format that names a direction of its own wins over the reader's choice,
    // so offering the choice here would be offering something that does not
    // happen. The items are shown disabled with the reason, rather than hidden,
    // because a missing menu entry explains nothing.
    const bool formatDecides =
        snapshot.provider != nullptr && snapshot.provider->graphDirection() != GraphDirection::Inherit;
    const bool topDown = layout.direction == GraphDirection::TopDown;

    ImGui::BeginDisabled(formatDecides);
    if (ImGui::MenuItem("Top down", nullptr, topDown) && !topDown) {
        directionRequest_ = GraphDirection::TopDown;
    }
    if (ImGui::MenuItem("Left to right", nullptr, !topDown) && topDown) {
        directionRequest_ = GraphDirection::LeftToRight;
    }
    ImGui::EndDisabled();

    if (formatDecides) {
        ImGui::TextDisabled("Chosen by %.*s.",
                            static_cast<int>(snapshot.provider->displayName().size()),
                            snapshot.provider->displayName().data());
    }
    ImGui::EndMenu();
}

void NodeView::drawContextMenu(const TreeLayout& layout, const DiffSnapshot& snapshot) {
    if (!ImGui::BeginPopup(kContextMenuId)) {
        return;
    }

    drawCardMenuItems(layout);

    if (ImGui::MenuItem("Collapse unchanged")) {
        collapseUnchanged(snapshot);
    }
    if (ImGui::MenuItem("Expand all")) {
        expandAll();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Fit to view")) {
        fit();
    }
    drawDirectionMenuItems(layout, snapshot);

    ImGui::EndPopup();
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

void NodeView::noteFailures(const DiffSnapshot& snapshot) {
    // One bit per node per side, filled once per layout, so a card asks a
    // lookup rather than a search of the failure list per frame.
    leftFailed_.clear();
    rightFailed_.clear();
    const auto mark = [](std::vector<bool>& flags, const Tree* tree) {
        if (tree == nullptr) {
            return;
        }
        flags.assign(tree->size(), false);
        for (const ShapeFailure& failure : tree->failures()) {
            if (failure.owner != kInvalidNode && failure.owner < flags.size()) {
                flags[failure.owner] = true;
            }
        }
    };
    mark(leftFailed_, snapshot.leftTree.get());
    mark(rightFailed_, snapshot.rightTree.get());
}

bool NodeView::failedUnder(const LayoutNode& card) const {
    const std::vector<bool>& flags = card.side == Side::Left ? leftFailed_ : rightFailed_;
    return card.node < flags.size() && flags[card.node];
}

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
        // A stack folds only at its bottom member, so a block is never cut
        // in the middle; the bottom is reached in its turn.
        if (collapseAnchor(layout, static_cast<LayoutId>(i)) != static_cast<LayoutId>(i)) {
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
    glideX_ = canvasWidth_ * 0.5f - (card.x + card.width * 0.5f) * zoom_;
    glideY_ = canvasHeight_ * 0.5f - (card.y + card.height * 0.5f) * zoom_;

    // Heading somewhere on purpose is taking hold of the view: the pan is now
    // the reader's and a resize must not snatch it back to a fit. Before the
    // first fit there is nothing to hold yet.
    if (framed_) {
        touched_ = true;
    }

    // The first framing of a layout has nowhere to travel from, so it arrives
    // rather than glides. Animating it would look like the tool was still
    // loading.
    if (!framed_) {
        panX_ = glideX_;
        panY_ = glideY_;
        framed_ = true;
        gliding_ = false;
        return;
    }
    gliding_ = true;
}

void NodeView::cancelGlide() {
    gliding_ = false;
    glideX_ = panX_;
    glideY_ = panY_;
}

void NodeView::advanceGlide() {
    if (!gliding_) {
        return;
    }

    const float dx = glideX_ - panX_;
    const float dy = glideY_ - panY_;
    if (std::abs(dx) < kGlideSettled && std::abs(dy) < kGlideSettled) {
        panX_ = glideX_;
        panY_ = glideY_;
        gliding_ = false;
        return;
    }

    // Exponential decay towards the destination: fast while there is a long way
    // to go, slow as it arrives, and the same duration whatever the frame rate.
    const float step = 1.0f - std::exp(-kGlideRate * ImGui::GetIO().DeltaTime);
    panX_ += dx * step;
    panY_ += dy * step;
}

void NodeView::followSelection(const TreeLayout& layout, const DiffSnapshot& snapshot,
                               const Selection& selection) {
    if (!selection.active() || selection.revision == followedRevision_) {
        return;
    }
    followedRevision_ = selection.revision;

    // The selection moved from somewhere else, so the change cursor has to
    // catch up or the next step would carry on from where it last was.
    syncChangeCursor(snapshot, selection);

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

void NodeView::syncChangeCursor(const DiffSnapshot& snapshot, const Selection& selection) {
    if (!snapshot.treeDiff || !selection.active()) {
        return;
    }

    const auto& changes = snapshot.treeDiff->changes;
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const Change& change = changes[i];
        const NodeId at = selection.side == Side::Left ? change.left : change.right;
        if (at == selection.node) {
            currentChange_ = static_cast<std::int64_t>(i);
            return;
        }
    }

    // An unchanged node is not in the list at all. Stepping on from the change
    // before it is closer to what the reader means by "next" than carrying on
    // from wherever the cursor was left.
    currentChange_ = -1;
}

void NodeView::handleKeys(const DiffSnapshot& snapshot, Selection& selection) {
    // Only when this view has the keyboard. The text view walks its own list of
    // changed lines, and one letter meaning two things at once would be worse
    // than it meaning nothing here.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        goToNextChange(snapshot, selection);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        goToPreviousChange(snapshot, selection);
    }
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
