/// \file
/// \brief Implementation of XML span recovery.

#include "formats/xml_spans.h"

namespace nmxd {

namespace {

/// \brief Reports whether a byte is XML whitespace.
///
/// \param c The byte to test.
///
/// \returns `true` for space, tab, carriage return and line feed.
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

std::uint32_t endOfTag(std::string_view text, std::uint32_t from) {
    bool inSingle = false;
    bool inDouble = false;
    for (std::uint32_t i = from; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (c == '"' && !inSingle) {
            inDouble = !inDouble;
        } else if (c == '>' && !inSingle && !inDouble) {
            return i + 1;
        }
    }
    return static_cast<std::uint32_t>(text.size());
}

std::uint32_t endOfClosingTag(std::string_view text, std::uint32_t from) {
    for (std::uint32_t i = from; i + 1 < text.size(); ++i) {
        if (text[i] == '<' && text[i + 1] == '/') {
            return endOfTag(text, i);
        }
    }
    return static_cast<std::uint32_t>(text.size());
}

std::vector<SourceSpan> scanAttributeSpans(std::string_view text, std::uint32_t tagBegin,
                                           std::uint32_t tagEnd) {
    std::vector<SourceSpan> spans;

    std::uint32_t i = tagBegin + 1;  // past '<'
    while (i < tagEnd && !isSpace(text[i]) && text[i] != '>' && text[i] != '/') {
        ++i;  // past the element name
    }

    while (i < tagEnd) {
        while (i < tagEnd && isSpace(text[i])) {
            ++i;
        }
        if (i >= tagEnd || text[i] == '>' || text[i] == '/') {
            break;
        }

        const std::uint32_t nameBegin = i;
        while (i < tagEnd && !isSpace(text[i]) && text[i] != '=' && text[i] != '>' &&
               text[i] != '/') {
            ++i;
        }
        std::uint32_t end = i;

        while (i < tagEnd && isSpace(text[i])) {
            ++i;
        }
        if (i < tagEnd && text[i] == '=') {
            ++i;
            while (i < tagEnd && isSpace(text[i])) {
                ++i;
            }
            if (i < tagEnd && (text[i] == '"' || text[i] == '\'')) {
                const char quote = text[i];
                ++i;
                while (i < tagEnd && text[i] != quote) {
                    ++i;
                }
                if (i < tagEnd) {
                    ++i;  // past the closing quote
                }
                end = i;
            }
        }
        spans.push_back(SourceSpan{nameBegin, end});
    }
    return spans;
}

bool isSelfClosing(std::string_view text, std::uint32_t tagEnd) {
    return tagEnd >= 2 && tagEnd <= text.size() && text[tagEnd - 2] == '/';
}

}  // namespace nmxd
