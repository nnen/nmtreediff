/// \file
/// \brief Implementation of the text view.

#include "ui/text_view.h"

#include "core/layout_tree.h"
#include "ui/theme.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include <imgui.h>

namespace nmxd {

namespace {

/// \brief Cell fill behind an added line.
///
/// \remarks The fills sit behind text, so they are kept low in alpha. The marks
///          in the gutter and the overview carry the saturation instead.
constexpr ImU32 kAddedFill = IM_COL32(46, 160, 100, 38);

/// \brief Cell fill behind a deleted line.
constexpr ImU32 kDeletedFill = IM_COL32(210, 90, 85, 38);

/// \brief Cell fill behind a modified line.
constexpr ImU32 kModifiedFill = IM_COL32(215, 165, 70, 30);

/// \brief Highlight behind the words added within a modified line.
constexpr ImU32 kAddedWord = IM_COL32(46, 160, 100, 96);

/// \brief Highlight behind the words removed within a modified line.
constexpr ImU32 kDeletedWord = IM_COL32(210, 90, 85, 96);

/// \brief Gutter and overview mark for an added line.
constexpr ImU32 kAddedMark = IM_COL32(70, 190, 125, 255);

/// \brief Gutter and overview mark for a deleted line.
constexpr ImU32 kDeletedMark = IM_COL32(226, 110, 105, 255);

/// \brief Gutter and overview mark for a modified line.
constexpr ImU32 kModifiedMark = IM_COL32(224, 176, 82, 255);

/// \brief Width in pixels of the change overview strip.
constexpr float kOverviewWidth = 14.0f;

/// \brief Corner radius of the overview strip's backing, in pixels.
constexpr float kOverviewRounding = 4.0f;

/// \brief Fill behind bytes the format left out of the tree.
///
/// \remarks A neutral grey-blue rather than a diff colour, because dropped
///          content is a fact about the format and not about the change.
constexpr ImU32 kDroppedFill = IM_COL32(140, 150, 175, 70);

/// \brief Fill behind an element a shaping job failed on.
///
/// \remarks Violet, which no diff status uses, so a broken script is never
///          mistaken for a deletion.
constexpr ImU32 kFailedFill = IM_COL32(190, 90, 210, 90);

/// \brief Draws a fill behind the part of a line that spans cover.
///
/// \param line The line being drawn, at the current cursor position.
/// \param lineStart The line's offset in the file.
/// \param spans Sorted, disjoint spans in file offsets.
/// \param colour The fill.
///
/// \remarks Drawn before the text so the text lands on top. Only the spans
///          that touch this line are measured, found by one binary search,
///          so a file with many marks costs no more per row than one with a
///          few.
void fillSpans(std::string_view line, std::uint32_t lineStart, const std::vector<SourceSpan>& spans,
               ImU32 colour) {
    const std::uint32_t lineEnd = lineStart + static_cast<std::uint32_t>(line.size());
    auto it = std::lower_bound(spans.begin(), spans.end(), lineStart,
                               [](const SourceSpan& span, std::uint32_t at) { return span.end <= at; });

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetTextLineHeight();
    for (; it != spans.end() && it->begin < lineEnd; ++it) {
        const std::uint32_t from = std::max(it->begin, lineStart) - lineStart;
        const std::uint32_t to = std::min(it->end, lineEnd) - lineStart;
        if (to <= from) {
            continue;
        }
        const float left = ImGui::CalcTextSize(line.data(), line.data() + from).x;
        const float right = ImGui::CalcTextSize(line.data(), line.data() + to).x;
        draw->AddRectFilled(ImVec2(pos.x + left, pos.y), ImVec2(pos.x + right, pos.y + height),
                            colour, 2.0f);
    }
}

/// \brief Chooses the cell fill for a row status.
///
/// \param status The row's status.
///
/// \returns The fill colour, or zero for an unchanged row.
ImU32 fillFor(RowStatus status) {
    switch (status) {
        case RowStatus::Added:
            return kAddedFill;
        case RowStatus::Deleted:
            return kDeletedFill;
        case RowStatus::Modified:
            return kModifiedFill;
        case RowStatus::Equal:
            break;
    }
    return 0;
}

/// \brief Chooses the gutter and overview mark for a row status.
///
/// \param status The row's status.
///
/// \returns The mark colour, or zero for an unchanged row.
ImU32 markFor(RowStatus status) {
    switch (status) {
        case RowStatus::Added:
            return kAddedMark;
        case RowStatus::Deleted:
            return kDeletedMark;
        case RowStatus::Modified:
            return kModifiedMark;
        case RowStatus::Equal:
            break;
    }
    return 0;
}

/// \brief Draws one line, tinting the runs the word diff marked as changed.
///
/// \param text The line to draw.
/// \param segments Word detail for this line, or null when there is none.
/// \param highlight The colour to draw behind changed runs.
///
/// \remarks Falls back to plain text when there is no word detail, which is the
///          common case.
void drawLine(std::string_view text, const std::vector<WordSegment>* segments, ImU32 highlight) {
    if (text.empty()) {
        ImGui::TextUnformatted("");
        return;
    }
    if (segments == nullptr || segments->empty()) {
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        return;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    bool first = true;
    for (const auto& segment : *segments) {
        if (segment.end <= segment.begin || segment.begin >= text.size()) {
            continue;
        }
        const char* begin = text.data() + segment.begin;
        const char* end = text.data() + std::min<std::size_t>(segment.end, text.size());

        if (!first) {
            ImGui::SameLine(0.0f, 0.0f);
        }
        first = false;

        if (segment.changed) {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 size = ImGui::CalcTextSize(begin, end);
            draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), highlight, 2.0f);
        }
        ImGui::TextUnformatted(begin, end);
    }
    if (first) {
        ImGui::TextUnformatted("");
    }
}

}  // namespace

namespace {

/// \brief Turns a clicked row into a selection in the corresponding tree.
///
/// \param snapshot The snapshot being displayed.
/// \param row The row that was clicked.
/// \param selection The shared selection to write.
///
/// \remarks Prefers the right side, because that is the version being reviewed;
///          a row that exists only on the left resolves against the left tree
///          instead. A row whose tree has not been parsed yet selects nothing.
void selectRow(const DiffSnapshot& snapshot, const DiffRow& row, Selection& selection) {
    const bool useRight = row.rightLine != kNoLine && snapshot.rightTree != nullptr;
    const bool useLeft = row.leftLine != kNoLine && snapshot.leftTree != nullptr;
    if (!useRight && !useLeft) {
        return;
    }

    const Side side = useRight ? Side::Right : Side::Left;
    const SourceFile& source = useRight ? *snapshot.right : *snapshot.left;
    const Tree& tree = useRight ? *snapshot.rightTree : *snapshot.leftTree;
    const std::uint32_t line = useRight ? row.rightLine : row.leftLine;

    // The first non-blank column of the line, so clicking an indented element
    // lands on that element rather than on its parent.
    std::uint32_t offset = source.lineStart(line);
    const std::string_view text = source.line(line);
    for (char c : text) {
        if (c != ' ' && c != '\t') {
            break;
        }
        ++offset;
    }

    const NodeId node = findNodeAt(tree, offset);
    if (node != kInvalidNode) {
        selection.select(side, node, tree.node(node).span.begin);
    }
}

}  // namespace

void TextView::draw(const DiffSnapshot& snapshot, Selection& selection) {
    if (!snapshot.hasSources()) {
        if (snapshot.stage == Stage::Failed) {
            ImGui::TextColored(ImVec4(0.88f, 0.45f, 0.43f, 1.0f), "%s", snapshot.message.c_str());
        } else if (snapshot.stage == Stage::Idle) {
            ImGui::TextDisabled("Nothing open.");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Pass two files on the command line, for example:\n\n"
                "    nmxmldiff before.xml after.xml\n\n"
                "Perforce and Git can be configured to do that for you; see "
                "docs/vcs-integration.md.");
        } else {
            ImGui::TextDisabled("Loading...");
        }
        return;
    }

    if (snapshot.stage == Stage::SourcesReady || !snapshot.text) {
        // The files are read but the alignment is still running. Showing the
        // raw text now beats showing nothing until the diff lands.
        ImGui::TextDisabled("Aligning lines...");
        ImGui::Separator();
        drawRawText(*snapshot.left, *snapshot.right);
        return;
    }

    followSelection(snapshot, selection);
    refreshMarks(leftMarks_, snapshot.leftTree.get());
    refreshMarks(rightMarks_, snapshot.rightTree.get());

    const float available = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("rows", ImVec2(ImGui::GetContentRegionAvail().x - kOverviewWidth - 4.0f, 0),
                      ImGuiChildFlags_None);
    drawRows(snapshot, *snapshot.text, selection);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 4.0f);
    drawOverview(*snapshot.text, available);
}

/// \brief Scrolls to the row holding a selection made in the other view.
///
/// \param snapshot The snapshot being displayed.
/// \param selection The shared selection.
///
/// \remarks Acts only on a revision it has not seen, so the view follows a
///          selection someone else made without fighting the one it made itself.
void TextView::followSelection(const DiffSnapshot& snapshot, const Selection& selection) {
    if (!selection.active() || selection.revision == followedRevision_ || !snapshot.text) {
        return;
    }
    followedRevision_ = selection.revision;

    const SourceFile* source =
        selection.side == Side::Left ? snapshot.left.get() : snapshot.right.get();
    if (source == nullptr) {
        return;
    }

    const std::size_t line = source->lineAt(selection.offset);
    const auto& lineToRow =
        selection.side == Side::Left ? snapshot.text->leftLineToRow : snapshot.text->rightLineToRow;
    if (line >= lineToRow.size()) {
        return;
    }
    scrollToRow_ = lineToRow[line];
    selectedRow_ = lineToRow[line];
}

void TextView::drawRows(const DiffSnapshot& snapshot, const TextDiff& diff, Selection& selection) {
    const SourceFile& left = *snapshot.left;
    const SourceFile& right = *snapshot.right;
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable;

    if (!ImGui::BeginTable("rows", 5, kFlags)) {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 6.0f);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupColumn(left.label().c_str(), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("# ", ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupColumn(right.label().c_str(), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    rowHeight_ = ImGui::GetTextLineHeightWithSpacing();
    visibleRows_ = static_cast<std::uint32_t>(
        std::max(1.0f, ImGui::GetContentRegionAvail().y / std::max(rowHeight_, 1.0f)));

    if (scrollToRow_ >= 0) {
        // Rows are one text line tall, so the target scroll position is exact
        // and there is no need to render the row first to find it.
        const float target = static_cast<float>(scrollToRow_) * rowHeight_;
        ImGui::SetScrollY(std::max(0.0f, target - rowHeight_ * 3.0f));
        scrollToRow_ = -1;
    }

    const auto rowCount = static_cast<int>(diff.rows.size());
    ImGuiListClipper clipper;
    clipper.Begin(rowCount);
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const DiffRow& row = diff.rows[static_cast<std::size_t>(index)];
            const std::vector<WordSegment>* leftWords = nullptr;
            const std::vector<WordSegment>* rightWords = nullptr;
            if (row.words != kNoLine && row.words < diff.wordRuns.size()) {
                leftWords = &diff.wordRuns[row.words].left;
                rightWords = &diff.wordRuns[row.words].right;
            }

            ImGui::TableNextRow();

            // Gutter: a solid stripe, so the shape of the change is readable
            // without reading any of the text.
            ImGui::TableSetColumnIndex(0);
            if (row.status != RowStatus::Equal) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, markFor(row.status));
            }

            const ImU32 fill = fillFor(row.status);
            const bool leftChanged =
                row.status == RowStatus::Deleted || row.status == RowStatus::Modified;
            const bool rightChanged =
                row.status == RowStatus::Added || row.status == RowStatus::Modified;

            ImGui::TableSetColumnIndex(1);
            if (row.leftLine != kNoLine) {
                ImGui::TextDisabled("%u", row.leftLine + 1);
            }
            ImGui::TableSetColumnIndex(2);
            if (leftChanged) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, fill);
            }
            if (row.leftLine != kNoLine) {
                drawMarks(leftMarks_, left.line(row.leftLine), left.lineStart(row.leftLine));
                drawLine(left.line(row.leftLine), leftWords, kDeletedWord);
            }

            ImGui::TableSetColumnIndex(3);
            if (row.rightLine != kNoLine) {
                ImGui::TextDisabled("%u", row.rightLine + 1);
            }
            ImGui::TableSetColumnIndex(4);
            if (rightChanged) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, fill);
            }
            if (row.rightLine != kNoLine) {
                drawMarks(rightMarks_, right.line(row.rightLine), right.lineStart(row.rightLine));
                drawLine(right.line(row.rightLine), rightWords, kAddedWord);
            }

            // The whole row is the click target, so selecting does not depend on
            // hitting the text rather than the space beside it.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                ImGui::IsMouseHoveringRect(
                    ImVec2(ImGui::GetWindowPos().x, ImGui::GetCursorScreenPos().y - rowHeight_),
                    ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                           ImGui::GetCursorScreenPos().y))) {
                selectRow(snapshot, row, selection);
                selectedRow_ = static_cast<std::uint32_t>(index);
            }
        }
    }

    ImGui::EndTable();
}

void TextView::refreshMarks(SideMarks& marks, const Tree* tree) {
    if (marks.tree == tree) {
        return;
    }
    marks.tree = tree;
    marks.dropped.clear();
    marks.failed.clear();
    if (tree == nullptr) {
        return;
    }

    // The tree's dropped spans are already sorted and disjoint. Failures
    // arrive in the order they happened and may nest, so they are sorted and
    // merged here once, when the tree changes, rather than per row.
    marks.dropped = tree->unrepresented();
    for (const ShapeFailure& failure : tree->failures()) {
        if (failure.span.end > failure.span.begin) {
            marks.failed.push_back(failure.span);
        }
    }
    std::sort(marks.failed.begin(), marks.failed.end(),
              [](const SourceSpan& a, const SourceSpan& b) { return a.begin < b.begin; });
    std::vector<SourceSpan> merged;
    for (const SourceSpan& span : marks.failed) {
        if (!merged.empty() && span.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, span.end);
        } else {
            merged.push_back(span);
        }
    }
    marks.failed = std::move(merged);
}

void TextView::drawMarks(const SideMarks& marks, std::string_view line,
                         std::uint32_t lineStart) const {
    if (showDropped_ && !marks.dropped.empty()) {
        fillSpans(line, lineStart, marks.dropped, kDroppedFill);
    }
    if (showFailed_ && !marks.failed.empty()) {
        fillSpans(line, lineStart, marks.failed, kFailedFill);
    }
}

void TextView::drawOverview(const TextDiff& diff, float height) {
    ImGui::BeginChild("overview", ImVec2(kOverviewWidth, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                        theme::kSurfaceRaised, kOverviewRounding);

    const auto rowCount = static_cast<float>(std::max<std::size_t>(diff.rows.size(), 1));
    for (std::size_t index = 0; index < diff.rows.size(); ++index) {
        const ImU32 mark = markFor(diff.rows[index].status);
        if (mark == 0) {
            continue;
        }
        // Every change is at least a pixel tall, so a single changed line in a
        // large file is still findable.
        const float y = origin.y + (static_cast<float>(index) / rowCount) * size.y;
        draw->AddRectFilled(ImVec2(origin.x + 2.0f, y),
                            ImVec2(origin.x + size.x - 2.0f, y + 2.0f), mark);
    }

    ImGui::InvisibleButton("overview_hit", size);
    if (ImGui::IsItemActive() || ImGui::IsItemClicked()) {
        const float local = ImGui::GetIO().MousePos.y - origin.y;
        const float fraction = std::clamp(local / std::max(size.y, 1.0f), 0.0f, 1.0f);
        scrollToRow_ = static_cast<std::int64_t>(fraction * rowCount);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImGui::EndChild();
}

void TextView::drawRawText(const SourceFile& left, const SourceFile& right) {
    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;

    if (!ImGui::BeginTable("raw", 2, kFlags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(left.label().c_str(), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(right.label().c_str(), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    const int rows = static_cast<int>(std::max(left.lineCount(), right.lineCount()));
    ImGuiListClipper clipper;
    clipper.Begin(rows);
    while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const auto line = static_cast<std::size_t>(index);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (line < left.lineCount()) {
                const auto text = left.line(line);
                ImGui::TextUnformatted(text.data(), text.data() + text.size());
            }
            ImGui::TableSetColumnIndex(1);
            if (line < right.lineCount()) {
                const auto text = right.line(line);
                ImGui::TextUnformatted(text.data(), text.data() + text.size());
            }
        }
    }
    ImGui::EndTable();
}

void TextView::goToNextChange(const DiffSnapshot& snapshot) {
    if (!snapshot.text || snapshot.text->changeBlocks.empty()) {
        return;
    }
    const auto& blocks = snapshot.text->changeBlocks;
    currentBlock_ = std::min<std::int64_t>(currentBlock_ + 1, static_cast<std::int64_t>(blocks.size()) - 1);
    selectedRow_ = blocks[static_cast<std::size_t>(currentBlock_)];
    scrollToRow_ = selectedRow_;
}

void TextView::goToPreviousChange(const DiffSnapshot& snapshot) {
    if (!snapshot.text || snapshot.text->changeBlocks.empty()) {
        return;
    }
    const auto& blocks = snapshot.text->changeBlocks;
    currentBlock_ = std::max<std::int64_t>(currentBlock_ - 1, 0);
    selectedRow_ = blocks[static_cast<std::size_t>(currentBlock_)];
    scrollToRow_ = selectedRow_;
}

}  // namespace nmxd
