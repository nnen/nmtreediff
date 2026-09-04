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

    const bool identical = snapshot.left->bytes() == snapshot.right->bytes();

    if (options.report == ReportFormat::Json) {
        out << "{\n";
        out << "  \"status\": \"ok\",\n";
        out << "  \"comparison\": \"bytes\",\n";
        out << "  \"identical\": " << (identical ? "true" : "false") << ",\n";
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
        out << (identical ? "identical" : "different") << " (byte comparison; the tree diff "
                                                          "arrives at M3)\n";
    }

    if (options.useExitCode) {
        return identical ? 0 : 1;
    }
    return 0;
}

}  // namespace nmxd
