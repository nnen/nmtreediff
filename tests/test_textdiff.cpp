#include <catch2/catch_test_macros.hpp>

#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "core/textdiff.h"

using nmxd::DiffRow;
using nmxd::kNoLine;
using nmxd::RowStatus;
using nmxd::SourceFile;
using nmxd::TextDiff;
using nmxd::TextDiffLimits;
using nmxd::TextDiffQuality;
using nmxd::tokenizeLine;

namespace {

std::stop_token neverStopped() {
    static std::stop_source source;
    return source.get_token();
}

TextDiff diffOf(const std::string& left, const std::string& right, TextDiffLimits limits = {}) {
    const auto a = SourceFile::fromMemory(left, "left");
    const auto b = SourceFile::fromMemory(right, "right");
    return nmxd::diffText(a, b, neverStopped(), limits);
}

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

// A file is never zero lines: an empty one holds a single empty line, the same
// way every editor counts it. The reference model has to agree, or it measures
// a different problem than the one the diff solved.
std::vector<std::string> asLines(const std::vector<std::string>& lines) {
    return lines.empty() ? std::vector<std::string>{std::string()} : lines;
}

// The length of the shortest edit script, by dynamic programming. Slow, but
// obviously correct, which is the point: it is the yardstick the fast
// implementation is measured against.
std::size_t optimalEditDistance(const std::vector<std::string>& a,
                                const std::vector<std::string>& b) {
    std::vector<std::vector<std::size_t>> d(a.size() + 1, std::vector<std::size_t>(b.size() + 1));
    for (std::size_t i = 0; i <= a.size(); ++i) {
        d[i][0] = i;
    }
    for (std::size_t j = 0; j <= b.size(); ++j) {
        d[0][j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        for (std::size_t j = 1; j <= b.size(); ++j) {
            d[i][j] = (a[i - 1] == b[j - 1]) ? d[i - 1][j - 1]
                                             : std::min(d[i - 1][j], d[i][j - 1]) + 1;
        }
    }
    return d[a.size()][b.size()];
}

// Insertions plus deletions implied by the rows. A modified row is one of each.
std::size_t scriptLength(const TextDiff& diff) {
    return diff.addedRows + diff.deletedRows + 2 * static_cast<std::size_t>(diff.modifiedRows);
}

}  // namespace

TEST_CASE("identical input produces only equal rows", "[textdiff]") {
    const auto diff = diffOf("a\nb\nc\n", "a\nb\nc\n");
    CHECK(diff.identical());
    CHECK(diff.rows.size() == 3);
    CHECK(diff.equalRows == 3);
    CHECK(diff.changeBlocks.empty());
    CHECK(diff.quality == TextDiffQuality::Full);
}

TEST_CASE("an inserted line is one added row", "[textdiff]") {
    const auto diff = diffOf("a\nc\n", "a\nb\nc\n");
    REQUIRE(diff.rows.size() == 3);
    CHECK(diff.rows[0].status == RowStatus::Equal);
    CHECK(diff.rows[1].status == RowStatus::Added);
    CHECK(diff.rows[1].leftLine == kNoLine);
    CHECK(diff.rows[1].rightLine == 1);
    CHECK(diff.rows[2].status == RowStatus::Equal);
    CHECK(diff.addedRows == 1);
    CHECK(diff.changeBlocks.size() == 1);
    CHECK(diff.changeBlocks[0] == 1);
}

TEST_CASE("a deleted line is one deleted row", "[textdiff]") {
    const auto diff = diffOf("a\nb\nc\n", "a\nc\n");
    REQUIRE(diff.rows.size() == 3);
    CHECK(diff.rows[1].status == RowStatus::Deleted);
    CHECK(diff.rows[1].leftLine == 1);
    CHECK(diff.rows[1].rightLine == kNoLine);
    CHECK(diff.deletedRows == 1);
}

TEST_CASE("a replaced line pairs into one modified row", "[textdiff]") {
    const auto diff = diffOf("a\nbefore\nc\n", "a\nafter\nc\n");
    REQUIRE(diff.rows.size() == 3);
    CHECK(diff.rows[1].status == RowStatus::Modified);
    CHECK(diff.rows[1].leftLine == 1);
    CHECK(diff.rows[1].rightLine == 1);
    CHECK(diff.modifiedRows == 1);
}

TEST_CASE("uneven change blocks pair what they can", "[textdiff]") {
    // Two lines out, three in: two pair up as modified and one is left over.
    const auto diff = diffOf("head\nx1\nx2\ntail\n", "head\ny1\ny2\ny3\ntail\n");
    CHECK(diff.modifiedRows == 2);
    CHECK(diff.addedRows == 1);
    CHECK(diff.deletedRows == 0);
    CHECK(diff.changeBlocks.size() == 1);
}

TEST_CASE("empty against non-empty is all additions", "[textdiff]") {
    const auto diff = diffOf("", "a\nb\nc\n");
    CHECK(diff.deletedRows + diff.modifiedRows <= 1);
    CHECK(diff.rows.size() >= 3);
    CHECK_FALSE(diff.identical());
}

TEST_CASE("files sharing nothing produce no equal rows", "[textdiff]") {
    const auto diff = diffOf("a\nb\nc\n", "x\ny\nz\n");
    CHECK(diff.equalRows == 0);
    CHECK(diff.modifiedRows == 3);
}

TEST_CASE("the alignment is optimal on random input", "[textdiff]") {
    // The strongest evidence available without reimplementing Myers: for a few
    // thousand random pairs, the edit script is exactly as short as the one a
    // dynamic-programming table proves is possible.
    std::mt19937 rng(20260905);
    std::uniform_int_distribution<int> lengthPick(0, 14);
    std::uniform_int_distribution<int> alphabetPick(0, 4);

    for (int trial = 0; trial < 3000; ++trial) {
        std::vector<std::string> a;
        std::vector<std::string> b;
        const int an = lengthPick(rng);
        const int bn = lengthPick(rng);
        for (int i = 0; i < an; ++i) {
            a.push_back(std::string(1, static_cast<char>('a' + alphabetPick(rng))));
        }
        for (int i = 0; i < bn; ++i) {
            b.push_back(std::string(1, static_cast<char>('a' + alphabetPick(rng))));
        }

        const auto diff = diffOf(join(a), join(b));
        const std::size_t expected = optimalEditDistance(asLines(a), asLines(b));

        INFO("trial " << trial << " left=" << join(a) << " right=" << join(b));
        REQUIRE(scriptLength(diff) == expected);
    }
}

TEST_CASE("the rows reconstruct both sides in order", "[textdiff]") {
    // Every left line appears once, in order, and likewise for the right. A row
    // list that fails this would misalign the two panes.
    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> lengthPick(0, 20);
    std::uniform_int_distribution<int> alphabetPick(0, 3);

    for (int trial = 0; trial < 500; ++trial) {
        std::vector<std::string> a;
        std::vector<std::string> b;
        for (int i = 0, n = lengthPick(rng); i < n; ++i) {
            a.push_back(std::string(1, static_cast<char>('a' + alphabetPick(rng))));
        }
        for (int i = 0, n = lengthPick(rng); i < n; ++i) {
            b.push_back(std::string(1, static_cast<char>('a' + alphabetPick(rng))));
        }

        const auto diff = diffOf(join(a), join(b));

        std::uint32_t nextLeft = 0;
        std::uint32_t nextRight = 0;
        for (const auto& row : diff.rows) {
            if (row.leftLine != kNoLine) {
                REQUIRE(row.leftLine == nextLeft);
                ++nextLeft;
            }
            if (row.rightLine != kNoLine) {
                REQUIRE(row.rightLine == nextRight);
                ++nextRight;
            }
        }
        // join() appends a newline per line, so an empty vector still yields
        // one (empty) line of input.
        REQUIRE(nextLeft == std::max<std::size_t>(a.size(), 1));
        REQUIRE(nextRight == std::max<std::size_t>(b.size(), 1));
    }
}

TEST_CASE("equal rows always pair a left line with a right line", "[textdiff]") {
    const auto diff = diffOf("a\nb\nc\nd\n", "a\nx\nc\nd\n");
    for (const auto& row : diff.rows) {
        if (row.status == RowStatus::Equal) {
            CHECK(row.leftLine != kNoLine);
            CHECK(row.rightLine != kNoLine);
        }
    }
}

TEST_CASE("change blocks mark the start of each run", "[textdiff]") {
    const auto diff = diffOf("a\nb\nc\nd\ne\n", "a\nB\nc\nD\ne\n");
    REQUIRE(diff.changeBlocks.size() == 2);
    CHECK(diff.rows[diff.changeBlocks[0]].status != RowStatus::Equal);
    CHECK(diff.rows[diff.changeBlocks[1]].status != RowStatus::Equal);
    CHECK(diff.changeBlocks[0] < diff.changeBlocks[1]);
}

TEST_CASE("tokenising keeps punctuation apart from words", "[textdiff]") {
    const auto tokens = tokenizeLine("  <node id=\"a1\"/>");
    std::string rebuilt;
    for (const auto token : tokens) {
        rebuilt += token;
    }
    CHECK(rebuilt == "  <node id=\"a1\"/>");
    CHECK(tokens.size() > 5);
}

TEST_CASE("word detail points at what actually changed", "[textdiff]") {
    const auto diff = diffOf("  <property name=\"speed\" value=\"1.0\"/>\n",
                             "  <property name=\"speed\" value=\"1.4\"/>\n");
    REQUIRE(diff.modifiedRows == 1);
    REQUIRE(diff.rows[0].words != kNoLine);

    const auto& run = diff.wordRuns[diff.rows[0].words];
    REQUIRE_FALSE(run.left.empty());

    const auto changedText = [](const std::string& line, const std::vector<nmxd::WordSegment>& s) {
        std::string out;
        for (const auto& segment : s) {
            if (segment.changed) {
                out += line.substr(segment.begin, segment.end - segment.begin);
            }
        }
        return out;
    };

    const std::string leftLine = "  <property name=\"speed\" value=\"1.0\"/>";
    const std::string rightLine = "  <property name=\"speed\" value=\"1.4\"/>";
    CHECK(changedText(leftLine, run.left) == "1.0");
    CHECK(changedText(rightLine, run.right) == "1.4");
}

TEST_CASE("word segments tile the whole line without gaps", "[textdiff]") {
    const auto diff = diffOf("alpha beta gamma delta\n", "alpha BETA gamma DELTA\n");
    REQUIRE(diff.rows[0].words != kNoLine);
    const auto& run = diff.wordRuns[diff.rows[0].words];

    std::uint32_t cursor = 0;
    for (const auto& segment : run.left) {
        CHECK(segment.begin == cursor);
        CHECK(segment.end > segment.begin);
        cursor = segment.end;
    }
    CHECK(cursor == std::string("alpha beta gamma delta").size());
}

TEST_CASE("word detail is skipped past the row ceiling and says so", "[textdiff]") {
    std::string left;
    std::string right;
    for (int i = 0; i < 40; ++i) {
        left += "value " + std::to_string(i) + "\n";
        right += "value " + std::to_string(i + 1000) + "\n";
    }

    TextDiffLimits limits;
    limits.maxWordRows = 5;
    const auto diff = diffOf(left, right, limits);

    CHECK(diff.quality == TextDiffQuality::WordsSkipped);
    CHECK(diff.wordRuns.empty());
    for (const auto& row : diff.rows) {
        CHECK(row.words == kNoLine);
    }
}

TEST_CASE("long lines are left unsplit", "[textdiff]") {
    const std::string longLeft(4000, 'a');
    const std::string longRight(4000, 'b');
    const auto diff = diffOf(longLeft + "\n", longRight + "\n");
    REQUIRE(diff.modifiedRows == 1);
    CHECK(diff.rows[0].words == kNoLine);
}

TEST_CASE("the edit-distance ceiling degrades visibly rather than silently", "[textdiff]") {
    std::string left;
    std::string right;
    for (int i = 0; i < 400; ++i) {
        left += "left " + std::to_string(i) + "\n";
        right += "right " + std::to_string(i) + "\n";
    }

    TextDiffLimits limits;
    limits.maxEditDistance = 4;
    const auto diff = diffOf(left, right, limits);

    CHECK(diff.quality == TextDiffQuality::LineCapped);
    CHECK_FALSE(diff.identical());
}

TEST_CASE("a cancelled diff reports that it stopped", "[textdiff]") {
    std::string left;
    std::string right;
    for (int i = 0; i < 5000; ++i) {
        left += "line " + std::to_string(i) + "\n";
        right += "line " + std::to_string(i * 2) + "\n";
    }

    const auto a = SourceFile::fromMemory(left, "left");
    const auto b = SourceFile::fromMemory(right, "right");

    std::stop_source source;
    source.request_stop();
    const auto diff = nmxd::diffText(a, b, source.get_token());

    CHECK(diff.cancelled);
}
