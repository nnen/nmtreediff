#pragma once

// The line diff behind the text view.
//
// The output is a single list of aligned rows rather than two independent
// lists. That is what makes the two panes scroll together: they are one table,
// so there is no scroll ratio to keep in step and no drift.

#include <cstdint>
#include <stop_token>
#include <string_view>
#include <vector>

#include "core/source.h"

namespace nmxd {

inline constexpr std::uint32_t kNoLine = 0xFFFFFFFFu;

enum class RowStatus : std::uint8_t {
    Equal,
    Added,
    Deleted,
    Modified,  // a deleted line paired with the added line that replaced it
};

// A run of characters within one line, flagged as changed or carried over.
struct WordSegment {
    std::uint32_t begin = 0;  // byte offset within the line's text
    std::uint32_t end = 0;
    bool changed = false;
};

// Word-level detail for one modified row. Empty when the pair was too
// dissimilar or too long to be worth splitting.
struct WordRun {
    std::vector<WordSegment> left;
    std::vector<WordSegment> right;
};

struct DiffRow {
    RowStatus status = RowStatus::Equal;
    std::uint32_t leftLine = kNoLine;   // zero-based index into the left file
    std::uint32_t rightLine = kNoLine;
    std::uint32_t words = kNoLine;      // index into TextDiff::wordRuns
};

// What the result is worth. Anything but Full is shown to the user, because a
// diff tool that quietly under-reports differences is not trusted twice.
enum class TextDiffQuality : std::uint8_t {
    Full,
    LineCapped,    // the edit-distance ceiling stopped the line alignment
    WordsSkipped,  // too many changed rows to split them into words
};

[[nodiscard]] const char* describe(TextDiffQuality quality) noexcept;

struct TextDiffLimits {
    // Ceiling on the Myers edit distance. Beyond this the affected region is
    // reported as a wholesale replacement rather than being aligned.
    std::uint32_t maxEditDistance = 65536;

    // The guard that actually bounds time. Alignment costs roughly the edit
    // distance squared, so capping the distance alone still allows billions of
    // steps on two files that share nothing. This counts the steps taken and
    // gives up when they run out, which keeps the worst case in the same order
    // as the ordinary case instead of thousands of times slower.
    std::uint64_t maxAlignmentSteps = 24'000'000;

    // Above this many changed rows, word detail is skipped entirely.
    std::uint32_t maxWordRows = 20000;

    // Lines longer than this are never split into words. Minified assets put
    // a whole document on one line, and that is not worth tokenising.
    std::uint32_t maxWordLineLength = 2000;
};

struct TextDiff {
    std::vector<DiffRow> rows;
    std::vector<WordRun> wordRuns;

    std::uint32_t addedRows = 0;
    std::uint32_t deletedRows = 0;
    std::uint32_t modifiedRows = 0;
    std::uint32_t equalRows = 0;

    // Row index where each run of consecutive changed rows begins, for
    // next-change and previous-change navigation and for the overview strip.
    std::vector<std::uint32_t> changeBlocks;

    TextDiffQuality quality = TextDiffQuality::Full;
    double elapsedMillis = 0.0;
    bool cancelled = false;

    [[nodiscard]] bool identical() const noexcept {
        return addedRows == 0 && deletedRows == 0 && modifiedRows == 0;
    }
    [[nodiscard]] std::uint32_t changedRows() const noexcept {
        return addedRows + deletedRows + modifiedRows;
    }
};

// Runs on a worker. Checks the token often enough that a cancel is noticed
// promptly even on a large pair.
[[nodiscard]] TextDiff diffText(const SourceFile& left, const SourceFile& right,
                                std::stop_token token, TextDiffLimits limits = {});

// Exposed for tests: splits a line the way the word diff does.
[[nodiscard]] std::vector<std::string_view> tokenizeLine(std::string_view line);

}  // namespace nmxd
