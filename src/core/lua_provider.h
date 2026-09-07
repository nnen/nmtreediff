#pragma once

/// \file
/// \brief A format provider written in script rather than compiled in.

#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/provider.h"

namespace nmxd {

class ProviderRegistry;

/// \brief Builds a provider from what a script declared.
///
/// \param spec What the script asked for.
/// \param base The built-in provider whose parse this one shapes.
/// \param script The text of the script that declared it.
///
/// \returns The provider, ready to register.
///
/// \remarks A scripted provider shapes; it does not parse bytes. The
///          requirements ask for custom XML-based and JSON-based formats, so
///          there is always an underlying format the tool already reads. For
///          XML the walker reports each element to the script as the parser
///          meets it; for any other base the base provider reads the file and
///          the script shapes that reading's tree. Either way every hot loop
///          stays in compiled code and the scripted surface stays far smaller
///          than IFormatProvider, which matters now that the surface is
///          published in two languages.
///
///          Two forms are read. The short form is five questions asked per
///          element. The full form is an enter and an exit callback, handed a
///          builder, which is the same IShaper interface a compiled format
///          implements with a Lua binding in front of it.
///
///          Each worker builds its own interpreter from \p script, because a
///          Lua state is not thread safe and the two sides of a diff parse in
///          parallel. The states share nothing, so a script cannot accumulate
///          state between files and quietly make one comparison depend on what
///          was opened before it.
[[nodiscard]] std::unique_ptr<IFormatProvider> makeScriptedProvider(
    const ScriptedProviderSpec& spec, const IFormatProvider& base, std::string script);

/// \brief Adds every provider a configuration declared to a registry.
///
/// \param registry The registry to add to.
/// \param config The configuration holding the declarations.
///
/// \returns One message per declaration that could not be used, empty when all
///          of them were.
///
/// \remarks A declaration naming a base format the build does not know is
///          reported rather than skipped, for the same reason an unknown
///          extension mapping is: a format that quietly does not exist sends
///          every diff through the wrong provider.
[[nodiscard]] std::vector<std::string> addScriptedProviders(ProviderRegistry& registry,
                                                            const ProviderConfig& config);

}  // namespace nmxd
