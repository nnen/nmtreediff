#pragma once

/// \file
/// \brief Driving a shaper from a tree another provider already built.

#include <stop_token>

#include "core/provider.h"
#include "core/shape.h"

namespace nmxd {

/// \brief Runs the nodes of an existing tree through a shaper.
///
/// \param generic The tree to reshape, usually a base format's reading of the
///        file.
/// \param source The file the tree was read from, for raw slices.
/// \param shaper What decides the meaning of each node.
/// \param provider The provider the result belongs to, for its name and for
///        the hashing pass.
/// \param token Checked on a bounded interval while walking.
///
/// \returns The reshaped tree, finalised and hashed, or a ParseError.
///
/// \remarks Every node is reported as an element named after its kind, with
///          its properties as the attributes and its span as the span. This
///          is how a format built on a base that has no walker of its own
///          still reaches the same shaper interface: the base reads the file,
///          and the shaper reads the base's tree. It costs one intermediate
///          tree, which a native walker does not, so XML and JSON each have
///          one and this serves everything else, such as a script built on a
///          compiled format of the studio's own.
[[nodiscard]] Result<Tree, ParseError> shapeTree(const Tree& generic, const SourceFile& source,
                                                 IShaper& shaper, const IFormatProvider& provider,
                                                 std::stop_token token);

}  // namespace nmxd
