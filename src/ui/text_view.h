#pragma once

// The text view. Both sides live in one table, so the panes cannot drift out
// of step: there is no scroll ratio to maintain, only a shared row index.

#include <cstdint>

#include "core/snapshot.h"

namespace nmxd {

class TextView {
public:
    // Draws into the current ImGui window.
    void draw(const DiffSnapshot& snapshot);

    void goToNextChange(const DiffSnapshot& snapshot);
    void goToPreviousChange(const DiffSnapshot& snapshot);

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
