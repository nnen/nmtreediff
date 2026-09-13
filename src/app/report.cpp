/// \file
/// \brief Implementation of the headless report.

#include "app/report.h"

#include "core/diff.h"
#include "core/provider.h"

#include <ostream>
#include <utility>
#include <string>
#include <string_view>

namespace nmxd {

namespace {

/// \brief Escapes a string for inclusion in a JSON document.
///
/// \param text The string to escape.
///
/// \returns The escaped text, without surrounding quotes. Control characters
///          become \\u00XX escapes.
std::string jsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    out += kHex[static_cast<unsigned char>(c) & 0xF];
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

/// \brief Counts the shaping jobs that failed on both sides.
///
/// \param snapshot The finished comparison.
///
/// \returns The total, or zero when the trees are not there.
std::size_t failureCount(const DiffSnapshot& snapshot) {
    std::size_t count = 0;
    if (snapshot.leftTree) {
        count += snapshot.leftTree->failures().size();
    }
    if (snapshot.rightTree) {
        count += snapshot.rightTree->failures().size();
    }
    return count;
}

/// \brief Writes the failures of one side as JSON array entries.
///
/// \param out Where to write.
/// \param side The side's name.
/// \param source The side's file, for line numbers.
/// \param tree The side's tree.
/// \param first Whether nothing has been written to the array yet; cleared
///        once something is.
void writeJsonFailures(std::ostream& out, const char* side, const SourceFile& source,
                       const Tree& tree, bool& first) {
    for (const ShapeFailure& failure : tree.failures()) {
        out << (first ? "\n" : ",\n") << "    { \"side\": \"" << side << "\", \"line\": "
            << (failure.span.end > failure.span.begin ? source.lineAt(failure.span.begin) + 1 : 0)
            << ", \"message\": \"" << jsonEscape(failure.message) << "\" }";
        first = false;
    }
}

/// \brief Writes a node's path as a JSON string, or null for no node.
///
/// \param out Where to write.
/// \param tree The tree the node belongs to.
/// \param id The node, or kInvalidNode.
void writeJsonPath(std::ostream& out, const Tree& tree, NodeId id) {
    if (id == kInvalidNode) {
        out << "null";
        return;
    }
    out << '"' << jsonEscape(nodePath(tree, id)) << '"';
}

/// \brief Writes the change list as a JSON array, one entry per changed node.
///
/// \param out Where to write.
/// \param left The left tree.
/// \param right The right tree.
/// \param model The classified diff.
///
/// \remarks The same list the text report prints, so automation gets what a
///          person gets: status, both paths, and the names of the properties
///          that differ. A path is null on the side the node is not on.
void writeJsonChanges(std::ostream& out, const Tree& left, const Tree& right,
                      const DiffModel& model) {
    out << "    \"changes\": [";
    bool first = true;
    for (const Change& change : model.changes) {
        out << (first ? "\n" : ",\n") << "      { \"status\": \"" << describe(change.status)
            << "\", \"moved\": " << (change.moved ? "true" : "false") << ", \"left\": ";
        writeJsonPath(out, left, change.left);
        out << ", \"right\": ";
        writeJsonPath(out, right, change.right);
        out << ", \"properties\": [";
        for (std::size_t i = 0; i < change.changedProperties.size(); ++i) {
            out << (i > 0 ? ", " : "") << '"' << jsonEscape(change.changedProperties[i]) << '"';
        }
        out << "] }";
        first = false;
    }
    out << (first ? "" : "\n    ") << "]\n";
}

/// \brief Writes the failures of one side as lines for a person.
///
/// \param out Where to write.
/// \param source The side's file, for its label and line numbers.
/// \param tree The side's tree.
void writeTextFailures(std::ostream& out, const SourceFile& source, const Tree& tree) {
    for (const ShapeFailure& failure : tree.failures()) {
        out << "failed: " << source.label();
        if (failure.span.end > failure.span.begin) {
            out << ":" << source.lineAt(failure.span.begin) + 1;
        }
        out << ": " << failure.message << "\n";
    }
}

/// \brief Reports whether the tree comparison is there to decide the verdict.
///
/// \param snapshot The finished comparison.
///
/// \returns `true` when a format resolved and the trees were compared.
///
/// \remarks The tree decides whenever it can, because a reformat is not a
///          change and the tree is what knows that; the line diff answers only
///          for a file no format claims. The exit code and the `identical`
///          field used to read the line diff even with a tree to hand, so the
///          whitespace-only pair exited 1 under a report that said identical.
bool treeDecides(const DiffSnapshot& snapshot) { return snapshot.treeDiff != nullptr; }

/// \brief Writes the line-diff summary the way a person reads it.
///
/// \param out Where to write.
/// \param text The line diff.
void writeLineSummary(std::ostream& out, const TextDiff& text) {
    if (text.identical()) {
        out << "lines: identical\n";
        return;
    }
    out << "lines: +" << text.addedRows << " -" << text.deletedRows << " ~" << text.modifiedRows
        << " across " << text.changeBlocks.size() << " change"
        << (text.changeBlocks.size() == 1 ? "" : "s") << "\n";
}

}  // namespace

namespace {

/// \brief Writes the machine-readable report.
///
/// \param out Where to write.
/// \param snapshot The finished comparison.
/// \param identical Whether the two sides matched, by whichever comparison
///        decides.
///
/// \remarks Split from the text report because the two share only the
///          numbers they quote. One function emitting both was long enough that
///          the shape of either was hard to see.
void writeJsonReport(std::ostream& out, const DiffSnapshot& snapshot, bool identical) {
    const TextDiff& text = *snapshot.text;
    out << "{\n";
    out << "  \"status\": \"ok\",\n";
    out << "  \"comparison\": \"" << (treeDecides(snapshot) ? "tree" : "lines") << "\",\n";
    out << "  \"identical\": " << (identical ? "true" : "false") << ",\n";
    out << "  \"quality\": \"" << jsonEscape(describe(text.quality)) << "\",\n";
    out << "  \"added\": " << text.addedRows << ",\n";
    out << "  \"deleted\": " << text.deletedRows << ",\n";
    out << "  \"modified\": " << text.modifiedRows << ",\n";
    out << "  \"unchanged\": " << text.equalRows << ",\n";
    out << "  \"changeBlocks\": " << text.changeBlocks.size() << ",\n";
    if (snapshot.provider != nullptr && snapshot.leftTree && snapshot.rightTree) {
        out << "  \"format\": \"" << jsonEscape(snapshot.provider->name()) << "\",\n";
        out << "  \"leftNodes\": " << snapshot.leftTree->size() << ",\n";
        out << "  \"rightNodes\": " << snapshot.rightTree->size() << ",\n";
    }
    if (snapshot.treeDiff != nullptr) {
        const DiffModel& tree = *snapshot.treeDiff;
        out << "  \"tree\": {\n";
        out << "    \"quality\": \"" << jsonEscape(describe(tree.quality)) << "\",\n";
        out << "    \"added\": " << tree.added << ",\n";
        out << "    \"deleted\": " << tree.deleted << ",\n";
        out << "    \"modified\": " << tree.modified << ",\n";
        out << "    \"moved\": " << tree.moved << ",\n";
        out << "    \"unchanged\": " << tree.unchanged << ",\n";
        out << "    \"trimmedContainers\": " << tree.trimmedParents << ",\n";
        writeJsonChanges(out, *snapshot.leftTree, *snapshot.rightTree, tree);
        out << "  },\n";
    }
    if (snapshot.leftTree && snapshot.rightTree) {
        // What the format left out, and where a script failed. Dropping is a
        // format's decision and only counted; a failure is a bug and listed.
        out << "  \"dropped\": { \"left\": " << snapshot.leftTree->unrepresented().size()
            << ", \"right\": " << snapshot.rightTree->unrepresented().size() << " },\n";
        out << "  \"failures\": [";
        bool first = true;
        writeJsonFailures(out, "left", *snapshot.left, *snapshot.leftTree, first);
        writeJsonFailures(out, "right", *snapshot.right, *snapshot.rightTree, first);
        out << (first ? "" : "\n  ") << "],\n";
    }
    out << "  \"elapsedMillis\": " << snapshot.elapsedMillis << ",\n";
    out << "  \"left\": { \"label\": \"" << jsonEscape(snapshot.left->label())
        << "\", \"bytes\": " << snapshot.left->size()
        << ", \"lines\": " << snapshot.left->lineCount() << " },\n";
    out << "  \"right\": { \"label\": \"" << jsonEscape(snapshot.right->label())
        << "\", \"bytes\": " << snapshot.right->size()
        << ", \"lines\": " << snapshot.right->lineCount() << " }\n";
    out << "}\n";
}

/// \brief Writes the report meant for a person.
///
/// \param out Where to write.
/// \param snapshot The finished comparison.
///
/// \remarks The line summary comes first and is labelled as lines. When a
///          format resolved, the node counts and the change list follow, and
///          the change list is the verdict: `identical` when it is empty,
///          whatever the lines did. Without a format the line summary is all
///          there is, and it decides.
void writeTextReport(std::ostream& out, const DiffSnapshot& snapshot) {
    const TextDiff& text = *snapshot.text;
    out << snapshot.left->label() << ": " << snapshot.left->size() << " bytes, "
        << snapshot.left->lineCount() << " lines\n";
    out << snapshot.right->label() << ": " << snapshot.right->size() << " bytes, "
        << snapshot.right->lineCount() << " lines\n";
    writeLineSummary(out, text);
    if (snapshot.provider != nullptr && snapshot.leftTree && snapshot.rightTree) {
        out << "format " << snapshot.provider->name() << ": " << snapshot.leftTree->size()
            << " and " << snapshot.rightTree->size() << " nodes\n";
    }
    if (snapshot.treeDiff != nullptr) {
        const DiffModel& tree = *snapshot.treeDiff;
        out << "nodes: +" << tree.added << " -" << tree.deleted << " ~" << tree.modified << " >"
            << tree.moved << "\n";
        if (tree.quality != MatchQuality::Full) {
            out << "warning: " << describe(tree.quality);
            if (tree.trimmedParents > 0) {
                out << " under " << tree.trimmedParents << " container"
                    << (tree.trimmedParents == 1 ? "" : "s");
            }
            out << "\n";
        }
        out << serializeChanges(*snapshot.leftTree, *snapshot.rightTree, tree);
    }
    if (text.quality != TextDiffQuality::Full) {
        out << "warning: " << describe(text.quality) << "\n";
    }
    if (snapshot.leftTree && snapshot.rightTree) {
        for (const auto& [source, tree] :
             {std::pair{snapshot.left.get(), snapshot.leftTree.get()},
              std::pair{snapshot.right.get(), snapshot.rightTree.get()}}) {
            const std::size_t dropped = tree->unrepresented().size();
            if (dropped > 0) {
                out << "warning: " << source->label() << ": the format left out " << dropped
                    << " stretch" << (dropped == 1 ? "" : "es") << " of the file\n";
            }
            writeTextFailures(out, *source, *tree);
        }
    }
}

}  // namespace

int writeReport(std::ostream& out, const DiffSnapshot& snapshot, const Options& options) {
    if (snapshot.stage == Stage::Failed || !snapshot.hasSources()) {
        const std::string message =
            snapshot.message.empty() ? std::string("no result was produced") : snapshot.message;
        if (options.report == ReportFormat::Json) {
            out << "{\n  \"status\": \"error\",\n  \"message\": \"" << jsonEscape(message)
                << "\"\n}\n";
        } else {
            out << "error: " << message << "\n";
        }
        return 2;
    }

    if (!snapshot.text) {
        const char* message = "the diff did not complete";
        if (options.report == ReportFormat::Json) {
            out << "{\n  \"status\": \"error\",\n  \"message\": \"" << message << "\"\n}\n";
        } else {
            out << "error: " << message << "\n";
        }
        return 2;
    }

    // The tree's verdict whenever there is one, the lines' otherwise. A build
    // job wired to the exit code is cashing in the claim that a reformat is
    // not a change, and only the tree can honour it.
    const bool identical =
        treeDecides(snapshot) ? snapshot.treeDiff->identical() : snapshot.text->identical();

    if (options.report == ReportFormat::Json) {
        writeJsonReport(out, snapshot, identical);
    } else {
        writeTextReport(out, snapshot);
    }

    // A shaping job that raised is a bug in the format, and a build job
    // wired to this must not read the comparison as sound. Dropped content is
    // a format's decision and never fails the run. The report is written
    // first either way, so the failure is on the record.
    if (failureCount(snapshot) > 0) {
        return 2;
    }
    if (options.useExitCode) {
        return identical ? 0 : 1;
    }
    return 0;
}

}  // namespace nmxd
