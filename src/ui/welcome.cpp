/// \file
/// \brief Implementation of the pane shown before a comparison is open.

#include "ui/welcome.h"

#include <imgui.h>

namespace nmtreediff {

namespace {

/// \brief Colour of the line explaining what a side is for.
constexpr ImVec4 kHintColour{0.55f, 0.59f, 0.65f, 1.0f};

/// \brief Colour of a failure message.
constexpr ImVec4 kErrorColour{0.89f, 0.43f, 0.41f, 1.0f};

/// \brief Colour of a path that has been chosen.
constexpr ImVec4 kChosenColour{0.38f, 0.78f, 0.55f, 1.0f};

/// \brief How wide the choose buttons are, in characters.
///
/// \remarks Sized in characters rather than pixels so the pane holds together
///          at whatever size the interface draws text.
constexpr float kButtonCharacters = 16.0f;

/// \brief Draws one side's row: its title, what is chosen, and its button.
///
/// \param title What this side is called.
/// \param hint What this side means, for a reader who has not used the tool.
/// \param path What has been chosen, or an empty path.
/// \param buttonId A label unique to this row, used as the button's identity.
/// \param enabled Whether the button can be pressed.
///
/// \returns `true` when the reader asked to choose a file for this side.
[[nodiscard]] bool drawSide(const char* title, const char* hint,
                            const std::filesystem::path& path, const char* buttonId,
                            bool enabled) {
    ImGui::PushID(buttonId);

    ImGui::TextUnformatted(title);
    ImGui::TextColored(kHintColour, "%s", hint);
    ImGui::Spacing();

    // A chosen file is shown by name, with the whole path on hover: the name is
    // what tells two versions apart, and the path is what tells you which
    // branch you are looking at.
    if (path.empty()) {
        ImGui::TextColored(kHintColour, "nothing chosen");
    } else {
        ImGui::TextColored(kChosenColour, "%s", path.filename().string().c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", path.string().c_str());
        }
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!enabled);
    const float width = ImGui::CalcTextSize("M").x * kButtonCharacters;
    const bool pressed = ImGui::Button(path.empty() ? "Choose..." : "Change...", ImVec2(width, 0));
    ImGui::EndDisabled();

    ImGui::PopID();
    return pressed;
}

}  // namespace

WelcomeChoice drawWelcome(const WelcomeState& state) {
    WelcomeChoice choice = WelcomeChoice::None;

    ImGui::Spacing();
    ImGui::TextUnformatted("Choose two files to compare.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Two columns, so the older and newer sides sit where they will sit in the
    // comparison itself.
    if (ImGui::BeginTable("welcome", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (drawSide("Left", "the older version", state.leftPath, "left", !state.picking)) {
            choice = WelcomeChoice::Left;
        }

        ImGui::TableSetColumnIndex(1);
        if (drawSide("Right", "the newer version", state.rightPath, "right", !state.picking)) {
            choice = WelcomeChoice::Right;
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (state.picking) {
        ImGui::TextColored(kHintColour, "Waiting for the file dialog...");
    } else if (!state.error.empty()) {
        ImGui::TextColored(kErrorColour, "%s", state.error.c_str());
    } else if (state.leftPath.empty() || state.rightPath.empty()) {
        ImGui::TextColored(kHintColour, "The comparison starts once both are chosen.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(kHintColour,
                       "Two paths on the command line do the same thing, which is how a version "
                       "control system opens this tool.");

    return choice;
}

}  // namespace nmtreediff
