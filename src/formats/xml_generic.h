#pragma once

// The default XML provider: every element is a node, every attribute is a
// property, and a leaf element's text becomes the #text property.
//
// It knows nothing about any schema. A format with its own idea of what counts
// as a node subclasses the interface instead of configuring this one.

#include <memory>

#include "core/provider.h"

namespace nmxd {

[[nodiscard]] std::unique_ptr<IFormatProvider> makeGenericXmlProvider();

}  // namespace nmxd
