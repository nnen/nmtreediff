#pragma once

/// \file
/// \brief The default JSON format provider.

#include <memory>

#include "core/provider.h"

namespace nmxd {

/// \brief Creates the generic JSON provider.
///
/// \returns A provider that treats every object, every array and every array
///          element as a node, and every scalar member of an object as a
///          property.
///
/// \remarks This is the provider that proves IFormatProvider is not
///          XML-shaped. Two hooks that generic XML never exercises earn their
///          place here: child order, because reordering the members of an
///          object changes nothing while reordering an array changes
///          everything, and per-node ordering rather than per-format, because
///          one document contains both.
///
///          Scalar values are recorded exactly as they appear in the source,
///          quotes, escapes, numeric formatting and all. A diff tool that
///          normalised them would be deciding on the reader's behalf that a
///          change is not worth seeing.
///
///          The provider knows nothing about any schema and returns only weak
///          identity keys. A format that knows its own subclasses the
///          interface and returns strong keys instead.
[[nodiscard]] std::unique_ptr<IFormatProvider> makeGenericJsonProvider();

}  // namespace nmxd
