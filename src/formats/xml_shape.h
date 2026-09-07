#pragma once

/// \file
/// \brief Driving a shaper from an XML document.

#include <stop_token>

#include "core/provider.h"
#include "core/shape.h"

namespace nmxd {

/// \brief Parses an XML document and runs its elements through a shaper.
///
/// \param source The file to parse.
/// \param shaper What decides the meaning of each element.
/// \param provider The provider the tree belongs to, for its name and for
///        the hashing pass, which asks it about child order.
/// \param token Checked on a bounded interval while walking.
///
/// \returns The tree, finalised and hashed, or a ParseError.
///
/// \remarks This is the one place XML is read. Every XML-based format, compiled
///          or scripted, is a shaper handed to this function, so span recovery
///          and cancellation are written once. Elements are reported in
///          document order: each is opened before anything inside it and
///          closed after, and the walk keeps its own stack rather than
///          recursing, so a deep document cannot overflow it.
[[nodiscard]] Result<Tree, ParseError> shapeXmlDocument(const SourceFile& source, IShaper& shaper,
                                                        const IFormatProvider& provider,
                                                        std::stop_token token);

}  // namespace nmxd
