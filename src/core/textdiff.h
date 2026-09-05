#pragma once

/// \file
/// \brief The line diff behind the text view.

#include <cstdint>
#include <stop_token>
#include <string_view>
#include <vector>

#include "core/source.h"

namespace nmxd {

/// \brief The line index that means "this row has no line on this side".
inline constexpr std::uint32_t kNoLine = 0xFFFFFFFFu;

/// \brief What happened to one row of the text view.
enum class RowStatus : std::uint8_t {
    Equal,     ///< The same line on both sides.
    Added,     ///< Present on the right only.
    Deleted,   ///< Present on the left only.
    Modified,  ///< A deleted line paired with the added line that replaced it.
};

/// \brief A run of characters within one line, flagged as changed or carried
///        over.
struct WordSegment {
    /// \brief Byte offset of the run within its line.
    std::uint32_t begin = 0;
    /// \brief Byte offset one past the run within its line.
    std::uint32_t end = 0;
    /// \brief Whether this run differs from the other side.
    bool changed = false;
};

/// \brief Word-level detail for one modified row.
///
/// \remarks Empty when the pair was too dissimilar or too long to be worth
///          splitting.
struct WordRun {
    /// \brief Segments covering the left line, in order and without gaps.
    std::vector<WordSegment> left;
    /// \brief Segments covering the right line, in order and without gaps.
    std::vector<WordSegment> right;
};

/// \brief One row of the text view, pairing a left line with a right line.
struct DiffRow {
    /// \brief What happened to this row.
    RowStatus status = RowStatus::Equal;
    /// \brief Zero-based line index in the left file, or kNoLine.
    std::uint32_t leftLine = kNoLine;
    /// \brief Zero-based line index in the right file, or kNoLine.
    std::uint32_t rightLine = kNoLine;
    /// \brief Index into TextDiff::wordRuns, or kNoLine when there is no word
    ///        detail.
    std::uint32_t words = kNoLine;
};

/// \brief How complete a text diff is.
///
/// \remarks Anything but TextDiffQuality::Full is shown to the user. A diff tool
///          that quietly under-reports differences is not trusted twice.
enum class TextDiffQuality : std::uint8_t {
    Full,          ///< Nothing was trimmed.
    LineCapped,    ///< A guard stopped the line alignment short.
    WordsSkipped,  ///< Too many changed rows to split them into words.
};

/// \brief Converts a quality value into a phrase suitable for a message.
///
/// \param quality The value to describe.
///
/// \returns A short sentence fragment, never null.
[[nodiscard]] const char* describe(TextDiffQuality quality) noexcept;

/// \brief The guards that bound how much work a text diff may do.
struct TextDiffLimits {
    /// \brief Ceiling on the Myers edit distance.
    ///
    /// \remarks Beyond this the affected region is reported as a wholesale
    ///          replacement rather than being aligned.
    std::uint32_t maxEditDistance = 65536;

    /// \brief Ceiling on alignment steps, which is what actually bounds time.
    ///
    /// \remarks Alignment costs roughly the edit distance squared, so capping
    ///          the distance alone still allows billions of steps on two files
    ///          that share nothing. Counting the steps taken keeps the worst
    ///          case in the same order as the ordinary case instead of thousands
    ///          of times slower.
    std::uint64_t maxAlignmentSteps = 24'000'000;

    /// \brief Above this many changed rows, word detail is skipped entirely.
    std::uint32_t maxWordRows = 20000;

    /// \brief Lines longer than this are never split into words.
    ///
    /// \remarks Minified assets put a whole document on one line, and that is
    ///          not worth tokenising.
    std::uint32_t maxWordLineLength = 2000;
};

/// \brief The aligned rows of a text diff, plus what they add up to.
///
/// \remarks Both sides live in one row list rather than two independent lists.
///          That is what makes the panes scroll together: there is no scroll
///          ratio to keep in step and no drift, only a shared row index.
struct TextDiff {
    /// \brief The aligned rows, in file order.
    std::vector<DiffRow> rows;
    /// \brief Word detail, referenced by DiffRow::words.
    std::vector<WordRun> wordRuns;

    std::uint32_t addedRows = 0;     ///< Rows present on the right only.
    std::uint32_t deletedRows = 0;   ///< Rows present on the left only.
    std::uint32_t modifiedRows = 0;  ///< Rows where a line was replaced.
    std::uint32_t equalRows = 0;     ///< Rows identical on both sides.

    /// \brief Row holding each left line, indexed by zero-based line number.
    ///
    /// \remarks Turns a node's source position into a row to scroll to, which is
    ///          what lets a selection in the node view find its place in the
    ///          text.
    std::vector<std::uint32_t> leftLineToRow;

    /// \brief Row holding each right line, indexed by zero-based line number.
    std::vector<std::uint32_t> rightLineToRow;

    /// \brief Row index where each run of consecutive changed rows begins.
    ///
    /// \remarks Drives next-change and previous-change navigation and the
    ///          overview strip.
    std::vector<std::uint32_t> changeBlocks;

    /// \brief How complete this diff is.
    TextDiffQuality quality = TextDiffQuality::Full;
    /// \brief Wall-clock milliseconds spent producing this diff.
    double elapsedMillis = 0.0;
    /// \brief Whether the diff stopped early because it was cancelled.
    bool cancelled = false;

    /// \brief Reports whether the two files have the same lines.
    ///
    /// \returns `true` when nothing was added, deleted or modified.
    [[nodiscard]] bool identical() const noexcept {
        return addedRows == 0 && deletedRows == 0 && modifiedRows == 0;
    }

    /// \brief Returns how many rows differ.
    ///
    /// \returns The sum of added, deleted and modified rows.
    [[nodiscard]] std::uint32_t changedRows() const noexcept {
        return addedRows + deletedRows + modifiedRows;
    }
};

/// \brief Aligns two files line by line and picks out the words that changed.
///
/// \param left The left, usually older, file.
/// \param right The right, usually newer, file.
/// \param token Checked often enough that a cancel is noticed promptly even on
///        a large pair.
/// \param limits The guards bounding how much work to do.
///
/// \returns The aligned rows. TextDiff::cancelled is set when the token stopped
///          the work, in which case the other fields are incomplete.
///
/// \remarks Runs on a worker thread. Lines are interned to integers and the
///          common ends trimmed before any alignment runs, so the usual case of
///          a small edit in a large file costs very little.
[[nodiscard]] TextDiff diffText(const SourceFile& left, const SourceFile& right,
                                std::stop_token token, TextDiffLimits limits = {});

/// \brief Splits a line the way the word diff does.
///
/// \param line The line to split.
///
/// \returns Views into \p line covering it completely and in order: runs of
///          whitespace, runs of word characters, and single punctuation
///          characters.
///
/// \remarks Exposed for tests.
[[nodiscard]] std::vector<std::string_view> tokenizeLine(std::string_view line);

}  // namespace nmxd
