#pragma once

/// \file
/// \brief The look of the interface: its palette and the Dear ImGui style built from it.

#include <imgui.h>

namespace nmtreediff {

/// \brief The surfaces and inks the interface is drawn with.
///
/// \remarks A flat theme is a handful of neutral surfaces stacked from
///          darkest to lightest, one accent, and no shadows or bevels. The
///          canvases that draw themselves rather than through widgets take
///          their colours from here too, so the whole window reads as one
///          material rather than a widget toolkit with pictures let into it.
namespace theme {

/// \brief The deepest surface: the viewport behind the panels, the menu bar,
///        and the canvas the node view draws on.
constexpr ImU32 kSurfaceDeep = IM_COL32(19, 21, 26, 255);

/// \brief A panel's own background.
constexpr ImU32 kSurfacePanel = IM_COL32(27, 30, 36, 255);

/// \brief A surface that sits on a panel: a field, a popup, a table header,
///        the fill of a quiet card.
constexpr ImU32 kSurfaceRaised = IM_COL32(36, 40, 48, 255);

/// \brief A surface under the pointer, or a resting button.
constexpr ImU32 kSurfaceHover = IM_COL32(46, 51, 61, 255);

/// \brief A surface being pressed, or a button under the pointer.
constexpr ImU32 kSurfaceActive = IM_COL32(57, 63, 75, 255);

/// \brief The one line colour: separators, table borders, quiet card outlines.
constexpr ImU32 kOutline = IM_COL32(52, 58, 69, 255);

/// \brief Lines that carry structure rather than divide it, such as the edges
///        joining a parent card to its children.
constexpr ImU32 kOutlineStrong = IM_COL32(98, 107, 122, 255);

/// \brief Text.
constexpr ImU32 kInk = IM_COL32(223, 228, 236, 255);

/// \brief Text that is present but secondary: hints, line numbers, counts.
constexpr ImU32 kInkMuted = IM_COL32(130, 139, 153, 255);

/// \brief The accent: selection, focus, the active tab's overline.
constexpr ImU32 kAccent = IM_COL32(86, 141, 255, 255);

}  // namespace theme

/// \brief Installs the interface theme on the current Dear ImGui context.
///
/// \remarks Called once after the context exists and before the first frame.
///          Replaces the built-in dark style rather than adjusting it, so what
///          the interface looks like is stated in one place rather than as a
///          diff against a default that may change.
void applyTheme();

}  // namespace nmtreediff
