/// \file
/// \brief Implementation of the interface theme.

#include "ui/theme.h"

namespace nmxd {

namespace {

/// \brief Corner radius of a field, a button, a tab or a grab, in pixels.
///
/// \remarks Small enough that a control still reads as a rectangle. Windows
///          and child regions keep square corners: docked panels meet edge to
///          edge, and a radius there opens a gap between them.
constexpr float kControlRounding = 4.0f;

/// \brief Corner radius of a popup or a tooltip, in pixels.
constexpr float kPopupRounding = 6.0f;

/// \brief Thickness of the coloured line over the selected tab.
constexpr float kTabOverline = 2.0f;

/// \brief Turns a packed colour into the float form the style table holds.
///
/// \param colour The packed colour.
/// \param alpha The opacity to give it, in place of the one it carries.
///
/// \returns The colour as the style wants it.
[[nodiscard]] ImVec4 tone(ImU32 colour, float alpha = 1.0f) {
    ImVec4 out = ImGui::ColorConvertU32ToFloat4(colour);
    out.w = alpha;
    return out;
}

/// \brief Sets the sizes, paddings and radii.
///
/// \param style The style to fill in.
///
/// \remarks No borders anywhere but on popups, which float and need an edge
///          to end at. Everything else is told apart by its surface colour,
///          which is what flat means.
void applyMetrics(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(10.0f, 5.0f);
    style.CellPadding = ImVec2(6.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = kTabOverline;
    style.DockingSeparatorSize = 1.0f;
    style.SeparatorTextBorderSize = 1.0f;

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = kControlRounding;
    style.PopupRounding = kPopupRounding;
    style.ScrollbarRounding = kControlRounding;
    style.GrabRounding = kControlRounding;
    style.TabRounding = kControlRounding;

    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
}

/// \brief Sets the colour table.
///
/// \param style The style to fill in.
///
/// \remarks A selected tab takes the colour of the panel under it, so the tab
///          and its content join into one shape and the accent overline is
///          what marks it. Hover and press are one step up the surface
///          ladder each, and the accent is kept for things that are chosen or
///          focused, so it stays meaningful.
void applyColours(ImGuiStyle& style) {
    using namespace theme;
    ImVec4* colours = style.Colors;

    // Text.
    colours[ImGuiCol_Text] = tone(kInk);
    colours[ImGuiCol_TextDisabled] = tone(kInkMuted);
    colours[ImGuiCol_TextLink] = tone(kAccent);
    colours[ImGuiCol_TextSelectedBg] = tone(kAccent, 0.35f);
    colours[ImGuiCol_InputTextCursor] = tone(kInk);

    // Surfaces.
    colours[ImGuiCol_WindowBg] = tone(kSurfacePanel);
    colours[ImGuiCol_ChildBg] = tone(kSurfacePanel, 0.0f);
    colours[ImGuiCol_PopupBg] = tone(kSurfaceRaised, 0.98f);
    colours[ImGuiCol_MenuBarBg] = tone(kSurfaceDeep);
    colours[ImGuiCol_DockingEmptyBg] = tone(kSurfaceDeep);
    colours[ImGuiCol_Border] = tone(kOutline);
    colours[ImGuiCol_BorderShadow] = tone(kSurfaceDeep, 0.0f);

    // Title bars, seen only on a window that has been floated.
    colours[ImGuiCol_TitleBg] = tone(kSurfaceDeep);
    colours[ImGuiCol_TitleBgActive] = tone(kSurfaceDeep);
    colours[ImGuiCol_TitleBgCollapsed] = tone(kSurfaceDeep);

    // Fields.
    colours[ImGuiCol_FrameBg] = tone(kSurfaceRaised);
    colours[ImGuiCol_FrameBgHovered] = tone(kSurfaceHover);
    colours[ImGuiCol_FrameBgActive] = tone(kSurfaceActive);
    colours[ImGuiCol_CheckMark] = tone(kAccent);
    colours[ImGuiCol_SliderGrab] = tone(kAccent);
    colours[ImGuiCol_SliderGrabActive] = tone(kAccent, 0.85f);

    // Buttons.
    colours[ImGuiCol_Button] = tone(kSurfaceHover);
    colours[ImGuiCol_ButtonHovered] = tone(kSurfaceActive);
    colours[ImGuiCol_ButtonActive] = tone(kAccent);

    // Headers: tree nodes, selectables, menu items.
    colours[ImGuiCol_Header] = tone(kAccent, 0.28f);
    colours[ImGuiCol_HeaderHovered] = tone(kAccent, 0.40f);
    colours[ImGuiCol_HeaderActive] = tone(kAccent, 0.55f);

    // Dividers.
    colours[ImGuiCol_Separator] = tone(kOutline);
    colours[ImGuiCol_SeparatorHovered] = tone(kAccent, 0.7f);
    colours[ImGuiCol_SeparatorActive] = tone(kAccent);
    colours[ImGuiCol_ResizeGrip] = tone(kInk, 0.0f);
    colours[ImGuiCol_ResizeGripHovered] = tone(kAccent, 0.6f);
    colours[ImGuiCol_ResizeGripActive] = tone(kAccent);

    // Scrollbars: no trough, a grab that is a shade of the text.
    colours[ImGuiCol_ScrollbarBg] = tone(kSurfaceDeep, 0.0f);
    colours[ImGuiCol_ScrollbarGrab] = tone(kInk, 0.14f);
    colours[ImGuiCol_ScrollbarGrabHovered] = tone(kInk, 0.24f);
    colours[ImGuiCol_ScrollbarGrabActive] = tone(kInk, 0.34f);

    // Tabs.
    colours[ImGuiCol_Tab] = tone(kSurfaceDeep);
    colours[ImGuiCol_TabHovered] = tone(kSurfaceRaised);
    colours[ImGuiCol_TabSelected] = tone(kSurfacePanel);
    colours[ImGuiCol_TabSelectedOverline] = tone(kAccent);
    colours[ImGuiCol_TabDimmed] = tone(kSurfaceDeep);
    colours[ImGuiCol_TabDimmedSelected] = tone(kSurfacePanel);
    colours[ImGuiCol_TabDimmedSelectedOverline] = tone(kOutlineStrong);
    colours[ImGuiCol_DockingPreview] = tone(kAccent, 0.5f);

    // Tables.
    colours[ImGuiCol_TableHeaderBg] = tone(kSurfaceRaised);
    colours[ImGuiCol_TableBorderStrong] = tone(kOutline);
    colours[ImGuiCol_TableBorderLight] = tone(kOutline);
    colours[ImGuiCol_TableRowBg] = tone(kInk, 0.0f);
    colours[ImGuiCol_TableRowBgAlt] = tone(kInk, 0.02f);
    colours[ImGuiCol_TreeLines] = tone(kOutline);

    // Plots, drag and drop, navigation.
    colours[ImGuiCol_PlotLines] = tone(kAccent);
    colours[ImGuiCol_PlotLinesHovered] = tone(kInk);
    colours[ImGuiCol_PlotHistogram] = tone(kAccent);
    colours[ImGuiCol_PlotHistogramHovered] = tone(kInk);
    colours[ImGuiCol_DragDropTarget] = tone(kAccent);
    colours[ImGuiCol_NavCursor] = tone(kAccent);
    colours[ImGuiCol_NavWindowingHighlight] = tone(kInk, 0.7f);
    colours[ImGuiCol_NavWindowingDimBg] = tone(kSurfaceDeep, 0.6f);
    colours[ImGuiCol_ModalWindowDimBg] = tone(kSurfaceDeep, 0.6f);
}

}  // namespace

void applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    applyMetrics(style);
    applyColours(style);
}

}  // namespace nmxd
