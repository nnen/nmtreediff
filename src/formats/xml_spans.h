#pragma once

/// \file
/// \brief Recovering source spans from XML bytes that pugixml does not report.

#include <cstdint>
#include <string_view>
#include <vector>

#include "core/source.h"

namespace nmtreediff {

/// \brief Finds where a tag ends.
///
/// \param text The whole document.
/// \param from Offset of the tag's opening angle bracket.
///
/// \returns The offset just past the matching closing bracket, or the end of the
///          document when there is none.
///
/// \remarks pugixml reports where a node starts but not where it ends, and a
///          span needs both. Quoting is respected, so a `>` inside an attribute
///          value is not mistaken for the end of the tag.
[[nodiscard]] std::uint32_t endOfTag(std::string_view text, std::uint32_t from);

/// \brief Finds where the next closing tag ends.
///
/// \param text The whole document.
/// \param from Offset at or before the closing tag.
///
/// \returns The offset just past the closing tag, or the end of the document
///          when there is none.
[[nodiscard]] std::uint32_t endOfClosingTag(std::string_view text, std::uint32_t from);

/// \brief Recovers a span for every attribute in one start tag.
///
/// \param text The whole document.
/// \param tagBegin Offset of the tag's opening angle bracket.
/// \param tagEnd Offset just past the tag's closing bracket.
///
/// \returns One span per attribute, in document order, each covering the name,
///          the equals sign and the quoted value.
///
/// \remarks pugixml reports offsets for nodes but not for attributes, so the
///          start tag is scanned once and the spans matched up with the parsed
///          attributes, which arrive in the same order.
[[nodiscard]] std::vector<SourceSpan> scanAttributeSpans(std::string_view text,
                                                         std::uint32_t tagBegin,
                                                         std::uint32_t tagEnd);

/// \brief Reports whether a start tag closes itself.
///
/// \param text The whole document.
/// \param tagEnd Offset just past the start tag's closing bracket.
///
/// \returns `true` for a tag written as `<name/>`, which has no closing tag to
///          look for.
[[nodiscard]] bool isSelfClosing(std::string_view text, std::uint32_t tagEnd);

}  // namespace nmtreediff
