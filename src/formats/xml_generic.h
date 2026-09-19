#pragma once

/// \file
/// \brief The default XML format provider.

#include <memory>

#include "core/provider.h"

namespace nmtreediff {

/// \brief Creates the generic XML provider.
///
/// \returns A provider that treats every element as a node and every attribute
///          as a property.
///
/// \remarks A leaf element's text becomes the property named kTextProperty.
///          Mixed content does not, because folding text into a node that also
///          has element children would invent a difference none of the target
///          formats mean.
///
///          The provider knows nothing about any schema, and returns only weak
///          identity keys: an `id` attribute in arbitrary XML might be a stable
///          key or might be a colour swatch name. A format that knows its own
///          schema subclasses the interface and returns strong keys instead.
[[nodiscard]] std::unique_ptr<IFormatProvider> makeGenericXmlProvider();

}  // namespace nmtreediff
