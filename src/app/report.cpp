#include "app/report.h"

#include <ostream>
#include <string>
#include <string_view>

namespace nmxd {

namespace {

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

    const TextDiff& text = *snapshot.text;
    const bool identical = text.identical();

    if (options.report == ReportFormat::Json) {
        out << "{\n";
        out << "  \"status\": \"ok\",\n";
        out << "  \"comparison\": \"lines\",\n";
        out << "  \"identical\": " << (identical ? "true" : "false") << ",\n";
        out << "  \"quality\": \"" << jsonEscape(describe(text.quality)) << "\",\n";
        out << "  \"added\": " << text.addedRows << ",\n";
        out << "  \"deleted\": " << text.deletedRows << ",\n";
        out << "  \"modified\": " << text.modifiedRows << ",\n";
        out << "  \"unchanged\": " << text.equalRows << ",\n";
        out << "  \"changeBlocks\": " << text.changeBlocks.size() << ",\n";
        out << "  \"elapsedMillis\": " << snapshot.elapsedMillis << ",\n";
        out << "  \"left\": { \"label\": \"" << jsonEscape(snapshot.left->label())
            << "\", \"bytes\": " << snapshot.left->size()
            << ", \"lines\": " << snapshot.left->lineCount() << " },\n";
        out << "  \"right\": { \"label\": \"" << jsonEscape(snapshot.right->label())
            << "\", \"bytes\": " << snapshot.right->size()
            << ", \"lines\": " << snapshot.right->lineCount() << " }\n";
        out << "}\n";
    } else {
        out << snapshot.left->label() << ": " << snapshot.left->size() << " bytes, "
            << snapshot.left->lineCount() << " lines\n";
        out << snapshot.right->label() << ": " << snapshot.right->size() << " bytes, "
            << snapshot.right->lineCount() << " lines\n";
        if (identical) {
            out << "identical\n";
        } else {
            out << "+" << text.addedRows << " -" << text.deletedRows << " ~" << text.modifiedRows
                << " across " << text.changeBlocks.size() << " change"
                << (text.changeBlocks.size() == 1 ? "" : "s") << "\n";
        }
        if (text.quality != TextDiffQuality::Full) {
            out << "warning: " << describe(text.quality) << "\n";
        }
    }

    if (options.useExitCode) {
        return identical ? 0 : 1;
    }
    return 0;
}

}  // namespace nmxd
