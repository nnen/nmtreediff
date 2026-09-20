/// \file
/// \brief Implementation of the About dialog.

#include "ui/about.h"

#include "ui/about_info.h"

#include <imgui.h>

namespace nmtreediff {

namespace {

/// \brief Title of the dialog, which is also how ImGui identifies it.
constexpr const char* kAboutTitle = "About NM Tree Diff";

/// \brief What the program calls itself.
constexpr const char* kProductName = "NM Tree Diff";

/// \brief The licence the program is released under.
constexpr const char* kLicenceLine = "Released under the MIT licence.";

/// \brief Where the notices of the libraries are to be found.
constexpr const char* kNoticesLine =
    "Their notices are in THIRD-PARTY-NOTICES.txt, installed with the program.";

/// \brief How wide the Close button is, in characters.
///
/// \remarks Sized in characters rather than pixels so the dialog holds
///          together at whatever size the interface draws text.
constexpr float kCloseButtonCharacters = 12.0f;

/// \brief A library compiled into the program, and the terms it came under.
struct Component {
    const char* name;     ///< What the library is called.
    const char* licence;  ///< The licence it is used under.
};

/// \brief The libraries compiled into the program, in alphabetical order.
///
/// \remarks Those cmake/Dependencies.cmake fetches, less the test framework,
///          which is not part of the executable. Versions are left out
///          because that file is where they are pinned, and a second list of
///          them here would be one more thing to forget when pinning a new
///          one. simdjson offers a choice of two licences, and this names the
///          one taken. The notices themselves ship in the file that
///          cmake/ThirdPartyNotices.cmake writes, which lists the same
///          libraries; a change to one list is a change to both.
constexpr Component kComponents[] = {
    {"CLI11", "BSD 3-Clause"},
    {"Dear ImGui", "MIT"},
    {"GLFW", "zlib"},
    {"Lua", "MIT"},
    {"Native File Dialog Extended", "zlib"},
    {"pugixml", "MIT"},
    {"simdjson", "MIT"},
    {"sol2", "MIT"},
};

/// \brief Draws what the program is: its name, version, purpose and terms.
void drawProduct() {
    ImGui::TextUnformatted(kProductName);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kAboutVersion);
    ImGui::TextUnformatted(kAboutDescription);

    ImGui::Spacing();
    ImGui::TextUnformatted(kAboutCopyright);
    ImGui::TextUnformatted(kLicenceLine);

    // A link rather than text, so that reporting a problem is one click away
    // from finding out which version has it.
    ImGui::Spacing();
    ImGui::TextLinkOpenURL(kAboutHomepage, kAboutHomepage);
}

/// \brief Draws the list of libraries the program is built with.
void drawComponents() {
    ImGui::TextDisabled("Built with");
    if (!ImGui::BeginTable("components", 2, ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    for (const Component& component : kComponents) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(component.name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s", component.licence);
    }
    ImGui::EndTable();

    ImGui::Spacing();
    ImGui::TextDisabled("%s", kNoticesLine);
}

/// \brief Draws the Close button, centred under the rest.
///
/// \returns `true` when the reader pressed it.
[[nodiscard]] bool drawCloseButton() {
    const float width = ImGui::CalcTextSize("M").x * kCloseButtonCharacters;
    const float free = ImGui::GetContentRegionAvail().x - width;
    if (free > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + free / 2.0f);
    }
    return ImGui::Button("Close", ImVec2(width, 0));
}

}  // namespace

void drawAbout(bool& open) {
    // A popup is opened by one call and drawn by another, and asking to open
    // one that is already open would restart it every frame.
    if (open && !ImGui::IsPopupOpen(kAboutTitle)) {
        ImGui::OpenPopup(kAboutTitle);
    }

    // In the middle of the window when it appears, and wherever the reader
    // drags it after that.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kAboutTitle, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    drawProduct();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    drawComponents();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Dear ImGui closes a menu on Escape and leaves a modal dialog open, so
    // the key is handled here to make it close whatever is on top.
    if (drawCloseButton() || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

}  // namespace nmtreediff
