#pragma once

/// \file
/// \brief The dialog that says what the program is.

namespace nmtreediff {

/// \brief Draws the About dialog while it is open.
///
/// \param open Whether the dialog is shown. Set it to `true` to open the
///        dialog; it is set back to `false` when the reader closes it.
///
/// \remarks Call once a frame, outside any other window, whether or not the
///          dialog is open. It is modal, so nothing behind it takes input
///          until it is closed, which the Close button, the cross in its title
///          bar and Escape all do.
///
///          A plain function over a flag the caller keeps, for the reason
///          drawWelcome() is one: it holds nothing between frames, and the
///          window is what knows when Help, About was chosen.
void drawAbout(bool& open);

}  // namespace nmtreediff
