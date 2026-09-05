#pragma once

/// \file
/// \brief The text view: both files side by side, aligned line by line.

#include <cstdint>

#include "core/snapshot.h"

namespace nmxd {

/// \brief Draws the line diff, and remembers where the reader is in it.
///
/// \remarks Both sides live in one table, so the panes cannot drift out of
///          step: there is no scroll ratio to maintain, only a shared row index.
class TextView {
public:
    /// \brief Draws the view into the current ImGui window.
    ///
    /// \param snapshot What to draw. Earlier stages render an explanatory state
    ///        rather than an empty pane.
    void draw(const DiffSnapshot& snapshot);

    /// \brief Scrolls to the next block of changed rows.
    ///
    /// \param snapshot The snapshot being displayed.
    void goToNextChange(const DiffSnapshot& snapshot);

    /// \brief Scrolls to the previous block of changed rows.
    ///
    /// \param snapshot The snapshot being displayed.
    void goToPreviousChange(const DiffSnapshot& snapshot);

    /// \brief Returns the row the reader was last taken to.
    ///
    /// \returns A row index into TextDiff::rows.
    [[nodiscard]] std::uint32_t selectedRow() const noexcept { return selectedRow_; }

private:
    void drawRows(const SourceFile& left, const SourceFile& right, const TextDiff& diff);
    void drawOverview(const TextDiff& diff, float height);
    void drawRawText(const SourceFile& left, const SourceFile& right);

    std::uint32_t selectedRow_ = 0;
    std::int64_t scrollToRow_ = -1;   // -1 when no scroll is pending
    std::int64_t currentBlock_ = -1;  // index into TextDiff::changeBlocks
    float rowHeight_ = 0.0f;
    std::uint32_t visibleRows_ = 0;
};

}  // namespace nmxd
