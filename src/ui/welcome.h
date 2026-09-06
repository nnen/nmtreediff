#pragma once

/// \file
/// \brief What the window shows before there is anything to compare.

#include <filesystem>
#include <string>

namespace nmxd {

/// \brief Which side the reader asked to choose, if any.
enum class WelcomeChoice {
    None,   ///< Nothing was clicked this frame.
    Left,   ///< Choose the older file.
    Right,  ///< Choose the newer file.
};

/// \brief What the welcome pane needs to know to draw itself.
struct WelcomeState {
    /// \brief The older file, or empty when it has not been chosen.
    std::filesystem::path leftPath;
    /// \brief The newer file, or empty when it has not been chosen.
    std::filesystem::path rightPath;
    /// \brief Whether a dialog is open, so the buttons can say so.
    bool picking = false;
    /// \brief Why the last dialog failed, or empty.
    std::string error;
};

/// \brief Draws the pane shown until both files are chosen.
///
/// \param state What has been chosen so far.
///
/// \returns Which side the reader asked to choose, if any.
///
/// \remarks Deliberately a plain function over a plain struct rather than a
///          class: it holds nothing between frames, so there is nothing for it
///          to own. The window keeps the paths, because the window is what
///          opens a comparison once both are there.
[[nodiscard]] WelcomeChoice drawWelcome(const WelcomeState& state);

}  // namespace nmxd
