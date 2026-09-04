#include "core/textdiff.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>

namespace nmxd {

namespace {

// ---------------------------------------------------------------- ids ------

// Lines are compared as integers rather than strings. Hashing once up front
// turns every later comparison in the alignment into one integer compare,
// which is what keeps a large file affordable.
class Interner {
public:
    explicit Interner(std::size_t expected) { ids_.reserve(expected * 2); }

    std::uint32_t idOf(std::string_view text) {
        const auto [it, inserted] = ids_.emplace(text, static_cast<std::uint32_t>(ids_.size()));
        (void)inserted;
        return it->second;
    }

private:
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    std::unordered_map<std::string_view, std::uint32_t, Hash, std::equal_to<>> ids_;
};

// --------------------------------------------------------------- myers -----

// An aligned run of equal elements.
struct Snake {
    std::uint32_t aStart = 0;
    std::uint32_t bStart = 0;
    std::uint32_t length = 0;
};

struct Middle {
    int xStart = 0;
    int yStart = 0;
    int xEnd = 0;
    int yEnd = 0;
    int distance = 0;
};

// Myers' linear-space refinement: find one snake that lies on some shortest
// edit path, then recurse either side of it. Space stays O(N + M) whatever the
// edit distance turns out to be.
class Aligner {
public:
    Aligner(const std::uint32_t* a, const std::uint32_t* b, std::size_t total, int maxDistance,
            std::uint64_t stepBudget)
        : a_(a), b_(b), maxDistance_(maxDistance), stepBudget_(stepBudget) {
        forward_.assign(2 * total + 3, 0);
        backward_.assign(2 * total + 3, 0);
    }

    [[nodiscard]] bool capped() const noexcept { return capped_; }

    void align(int a0, int a1, int b0, int b1, std::vector<Snake>& out) {
        const int n = a1 - a0;
        const int m = b1 - b0;
        if (n <= 0 || m <= 0) {
            return;  // a pure insertion or deletion contributes no snake
        }

        Middle middle;
        if (!findMiddle(a0, n, b0, m, middle)) {
            // Past the ceiling. The region is reported as a wholesale
            // replacement, which the caller surfaces as reduced quality.
            capped_ = true;
            return;
        }

        if (middle.distance <= 1) {
            emitTrivial(a0, a1, b0, b1, out);
            return;
        }

        align(a0, a0 + middle.xStart, b0, b0 + middle.yStart, out);
        if (middle.xEnd > middle.xStart) {
            out.push_back(Snake{static_cast<std::uint32_t>(a0 + middle.xStart),
                                static_cast<std::uint32_t>(b0 + middle.yStart),
                                static_cast<std::uint32_t>(middle.xEnd - middle.xStart)});
        }
        align(a0 + middle.xEnd, a1, b0 + middle.yEnd, b1, out);
    }

private:
    // At most one edit in the region: the longest common prefix, the odd
    // element, then whatever remains lines up.
    void emitTrivial(int a0, int a1, int b0, int b1, std::vector<Snake>& out) {
        const int n = a1 - a0;
        const int m = b1 - b0;

        int prefix = 0;
        while (prefix < n && prefix < m && a_[a0 + prefix] == b_[b0 + prefix]) {
            ++prefix;
        }
        if (prefix > 0) {
            out.push_back(Snake{static_cast<std::uint32_t>(a0), static_cast<std::uint32_t>(b0),
                                static_cast<std::uint32_t>(prefix)});
        }

        const int sa = a0 + prefix + (n > m ? 1 : 0);
        const int sb = b0 + prefix + (m > n ? 1 : 0);
        const int rest = std::min(a1 - sa, b1 - sb);
        if (rest > 0) {
            out.push_back(Snake{static_cast<std::uint32_t>(sa), static_cast<std::uint32_t>(sb),
                                static_cast<std::uint32_t>(rest)});
        }
    }

    bool findMiddle(int a0, int n, int b0, int m, Middle& out) {
        const int delta = n - m;
        const bool oddDelta = (delta & 1) != 0;
        const int offset = n + m;
        const int limit = std::min(maxDistance_, (n + m + 1) / 2);

        int* vf = forward_.data() + offset;
        int* vb = backward_.data() + offset;

        // Only the seeds need initialising: at step d the algorithm reads only
        // diagonals written at step d - 1, so nothing stale is ever consulted.
        vf[1] = 0;
        vb[1] = 0;

        for (int d = 0; d <= limit; ++d) {
            // Charged once per diagonal per step, plus once per element matched
            // below. Checked here because the cost of a step is bounded.
            steps_ += static_cast<std::uint64_t>(d) + 1;
            if (steps_ > stepBudget_) {
                return false;
            }

            for (int k = -d; k <= d; k += 2) {
                int x = (k == -d || (k != d && vf[k - 1] < vf[k + 1])) ? vf[k + 1] : vf[k - 1] + 1;
                int y = x - k;
                const int xStart = x;
                const int yStart = y;
                while (x < n && y < m && a_[a0 + x] == b_[b0 + y]) {
                    ++x;
                    ++y;
                }
                steps_ += static_cast<std::uint64_t>(x - xStart);
                vf[k] = x;

                // A forward path of length d can only meet a reverse path of
                // length d - 1, which is possible when delta is odd.
                const int reverseK = delta - k;
                if (oddDelta && d > 0 && reverseK >= -(d - 1) && reverseK <= (d - 1) &&
                    x + vb[reverseK] >= n) {
                    out = Middle{xStart, yStart, x, y, 2 * d - 1};
                    return true;
                }
            }

            for (int k = -d; k <= d; k += 2) {
                int x = (k == -d || (k != d && vb[k - 1] < vb[k + 1])) ? vb[k + 1] : vb[k - 1] + 1;
                int y = x - k;
                const int xStart = x;
                const int yStart = y;
                while (x < n && y < m && a_[a0 + n - 1 - x] == b_[b0 + m - 1 - y]) {
                    ++x;
                    ++y;
                }
                steps_ += static_cast<std::uint64_t>(x - xStart);
                vb[k] = x;

                const int forwardK = delta - k;
                if (!oddDelta && forwardK >= -d && forwardK <= d && vf[forwardK] + x >= n) {
                    // Reverse coordinates count back from the end of the region.
                    out = Middle{n - x, m - y, n - xStart, m - yStart, 2 * d};
                    return true;
                }
            }
        }
        return false;
    }

    const std::uint32_t* a_;
    const std::uint32_t* b_;
    int maxDistance_;
    std::uint64_t stepBudget_;
    std::uint64_t steps_ = 0;
    bool capped_ = false;
    std::vector<int> forward_;
    std::vector<int> backward_;
};

// Aligns two id sequences into snakes, trimming the common ends first because
// that is nearly free and usually removes most of the work.
std::vector<Snake> alignSequences(const std::vector<std::uint32_t>& a,
                                  const std::vector<std::uint32_t>& b, int maxDistance,
                                  std::uint64_t stepBudget, bool& capped) {
    std::vector<Snake> snakes;

    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());

    int prefix = 0;
    while (prefix < n && prefix < m && a[prefix] == b[prefix]) {
        ++prefix;
    }
    int suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix && a[n - 1 - suffix] == b[m - 1 - suffix]) {
        ++suffix;
    }

    if (prefix > 0) {
        snakes.push_back(Snake{0, 0, static_cast<std::uint32_t>(prefix)});
    }

    if (prefix < n - suffix || prefix < m - suffix) {
        Aligner aligner(a.data(), b.data(), a.size() + b.size(), maxDistance, stepBudget);
        aligner.align(prefix, n - suffix, prefix, m - suffix, snakes);
        capped = capped || aligner.capped();
    }

    if (suffix > 0) {
        snakes.push_back(Snake{static_cast<std::uint32_t>(n - suffix),
                               static_cast<std::uint32_t>(m - suffix),
                               static_cast<std::uint32_t>(suffix)});
    }
    return snakes;
}

// ---------------------------------------------------------------- words ----

bool isWordChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.';
}

// How alike two lines are, judged on their shared ends. Cheap, and enough to
// decide whether splitting them into words would tell the reader anything.
double endSimilarity(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) {
        return 0.0;
    }
    const std::size_t limit = std::min(a.size(), b.size());
    std::size_t prefix = 0;
    while (prefix < limit && a[prefix] == b[prefix]) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < limit - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        ++suffix;
    }
    return static_cast<double>(prefix + suffix) / static_cast<double>(std::max(a.size(), b.size()));
}

void appendSegment(std::vector<WordSegment>& out, std::uint32_t begin, std::uint32_t end,
                   bool changed) {
    if (end <= begin) {
        return;
    }
    // Merging touching runs of the same kind keeps the highlight from being
    // chopped into per-token boxes.
    if (!out.empty() && out.back().changed == changed && out.back().end == begin) {
        out.back().end = end;
        return;
    }
    out.push_back(WordSegment{begin, end, changed});
}

WordRun diffWords(std::string_view left, std::string_view right) {
    const auto leftTokens = tokenizeLine(left);
    const auto rightTokens = tokenizeLine(right);

    Interner interner(leftTokens.size() + rightTokens.size());
    std::vector<std::uint32_t> a;
    std::vector<std::uint32_t> b;
    a.reserve(leftTokens.size());
    b.reserve(rightTokens.size());
    for (const auto token : leftTokens) {
        a.push_back(interner.idOf(token));
    }
    for (const auto token : rightTokens) {
        b.push_back(interner.idOf(token));
    }

    bool capped = false;
    // A line is short, so its tokens can always be aligned exactly.
    const auto snakes =
        alignSequences(a, b, static_cast<int>(a.size() + b.size() + 1), 1'000'000, capped);

    const auto offsetOf = [](std::string_view line, const std::vector<std::string_view>& tokens,
                             std::size_t index) -> std::uint32_t {
        if (index >= tokens.size()) {
            return static_cast<std::uint32_t>(line.size());
        }
        return static_cast<std::uint32_t>(tokens[index].data() - line.data());
    };

    WordRun run;
    std::size_t ai = 0;
    std::size_t bi = 0;
    for (const auto& snake : snakes) {
        appendSegment(run.left, offsetOf(left, leftTokens, ai),
                      offsetOf(left, leftTokens, snake.aStart), true);
        appendSegment(run.right, offsetOf(right, rightTokens, bi),
                      offsetOf(right, rightTokens, snake.bStart), true);

        appendSegment(run.left, offsetOf(left, leftTokens, snake.aStart),
                      offsetOf(left, leftTokens, snake.aStart + snake.length), false);
        appendSegment(run.right, offsetOf(right, rightTokens, snake.bStart),
                      offsetOf(right, rightTokens, snake.bStart + snake.length), false);

        ai = snake.aStart + snake.length;
        bi = snake.bStart + snake.length;
    }
    appendSegment(run.left, offsetOf(left, leftTokens, ai), static_cast<std::uint32_t>(left.size()),
                  true);
    appendSegment(run.right, offsetOf(right, rightTokens, bi),
                  static_cast<std::uint32_t>(right.size()), true);

    return run;
}

}  // namespace

const char* describe(TextDiffQuality quality) noexcept {
    switch (quality) {
        case TextDiffQuality::Full:
            return "complete";
        case TextDiffQuality::LineCapped:
            return "line alignment stopped at the edit-distance ceiling";
        case TextDiffQuality::WordsSkipped:
            return "word detail skipped: too many changed lines";
    }
    return "unknown";
}

std::vector<std::string_view> tokenizeLine(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        const std::size_t start = i;
        const auto c = static_cast<unsigned char>(line[i]);
        if (c == ' ' || c == '\t') {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
                ++i;
            }
        } else if (isWordChar(c)) {
            while (i < line.size() && isWordChar(static_cast<unsigned char>(line[i]))) {
                ++i;
            }
        } else {
            ++i;  // punctuation stands alone, which suits markup and JSON
        }
        tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

TextDiff diffText(const SourceFile& left, const SourceFile& right, std::stop_token token,
                  TextDiffLimits limits) {
    const auto started = std::chrono::steady_clock::now();

    TextDiff diff;

    Interner interner(left.lineCount() + right.lineCount());
    std::vector<std::uint32_t> a;
    std::vector<std::uint32_t> b;
    a.reserve(left.lineCount());
    b.reserve(right.lineCount());

    for (std::size_t i = 0; i < left.lineCount(); ++i) {
        a.push_back(interner.idOf(left.line(i)));
    }
    if (token.stop_requested()) {
        diff.cancelled = true;
        return diff;
    }
    for (std::size_t i = 0; i < right.lineCount(); ++i) {
        b.push_back(interner.idOf(right.line(i)));
    }
    if (token.stop_requested()) {
        diff.cancelled = true;
        return diff;
    }

    bool capped = false;
    const auto snakes = alignSequences(a, b, static_cast<int>(limits.maxEditDistance),
                                      limits.maxAlignmentSteps, capped);
    if (token.stop_requested()) {
        diff.cancelled = true;
        return diff;
    }
    if (capped) {
        diff.quality = TextDiffQuality::LineCapped;
    }

    diff.rows.reserve(std::max(a.size(), b.size()) + 16);

    // Walk the snakes, filling the gaps between them with change blocks.
    std::uint32_t ai = 0;
    std::uint32_t bi = 0;
    bool inChange = false;

    const auto beginChange = [&diff, &inChange] {
        if (!inChange) {
            diff.changeBlocks.push_back(static_cast<std::uint32_t>(diff.rows.size()));
            inChange = true;
        }
    };

    const auto emitGap = [&](std::uint32_t aEnd, std::uint32_t bEnd) {
        const std::uint32_t deletions = aEnd - ai;
        const std::uint32_t additions = bEnd - bi;
        if (deletions == 0 && additions == 0) {
            return;
        }
        beginChange();

        // Pairing positionally is what turns a rewritten line into one row with
        // the changed words picked out, rather than two rows to compare by eye.
        const std::uint32_t paired = std::min(deletions, additions);
        for (std::uint32_t i = 0; i < paired; ++i) {
            DiffRow row;
            row.status = RowStatus::Modified;
            row.leftLine = ai + i;
            row.rightLine = bi + i;
            diff.rows.push_back(row);
            ++diff.modifiedRows;
        }
        for (std::uint32_t i = paired; i < deletions; ++i) {
            DiffRow row;
            row.status = RowStatus::Deleted;
            row.leftLine = ai + i;
            diff.rows.push_back(row);
            ++diff.deletedRows;
        }
        for (std::uint32_t i = paired; i < additions; ++i) {
            DiffRow row;
            row.status = RowStatus::Added;
            row.rightLine = bi + i;
            diff.rows.push_back(row);
            ++diff.addedRows;
        }
        ai = aEnd;
        bi = bEnd;
    };

    for (const auto& snake : snakes) {
        emitGap(snake.aStart, snake.bStart);
        for (std::uint32_t i = 0; i < snake.length; ++i) {
            DiffRow row;
            row.status = RowStatus::Equal;
            row.leftLine = snake.aStart + i;
            row.rightLine = snake.bStart + i;
            diff.rows.push_back(row);
        }
        diff.equalRows += snake.length;
        ai = snake.aStart + snake.length;
        bi = snake.bStart + snake.length;
        inChange = false;

        if (token.stop_requested()) {
            diff.cancelled = true;
            return diff;
        }
    }
    emitGap(static_cast<std::uint32_t>(a.size()), static_cast<std::uint32_t>(b.size()));

    // Word detail last, so the expensive part is the part that can be skipped.
    if (diff.modifiedRows > limits.maxWordRows) {
        // A capped alignment is the more serious of the two, so it keeps the
        // report. Missing word detail is cosmetic; a missing line match is not.
        if (diff.quality == TextDiffQuality::Full) {
            diff.quality = TextDiffQuality::WordsSkipped;
        }
    } else {
        for (auto& row : diff.rows) {
            if (row.status != RowStatus::Modified) {
                continue;
            }
            const auto leftText = left.line(row.leftLine);
            const auto rightText = right.line(row.rightLine);
            if (leftText.size() > limits.maxWordLineLength ||
                rightText.size() > limits.maxWordLineLength) {
                continue;
            }
            if (endSimilarity(leftText, rightText) < 0.15) {
                continue;  // nothing in common worth pointing at
            }
            row.words = static_cast<std::uint32_t>(diff.wordRuns.size());
            diff.wordRuns.push_back(diffWords(leftText, rightText));

            if (token.stop_requested()) {
                diff.cancelled = true;
                return diff;
            }
        }
    }

    diff.elapsedMillis =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return diff;
}

}  // namespace nmxd
