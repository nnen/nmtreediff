#pragma once

/// \file
/// \brief Reading a configuration script.

#include <filesystem>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/result.h"

namespace nmtreediff {

/// \brief Reads a configuration from the text of a script.
///
/// \param text The script.
/// \param origin The file it came from, used only in messages.
/// \param config The configuration to add to. Later calls override earlier
///        ones, which is what makes the search order mean anything.
/// \param problems Filled with everything wrong with the script. Never cleared,
///        so several files accumulate into one list.
///
/// \returns `true` when the script ran and asked only for things that exist.
///
/// \remarks The script is given four functions: `formats` takes a table of
///          extension to provider name, `fallback` and `graph_direction` and
///          `exit_key` each take one string, and `provider` declares a format
///          written in script. Everything the standard library offers is also
///          there, so a studio can build its table with a loop.
[[nodiscard]] bool runConfigScript(std::string_view text, const std::filesystem::path& origin,
                                   ProviderConfig& config, std::vector<ConfigProblem>& problems);

/// \brief Reads a configuration script from a file.
///
/// \param path The file to read.
/// \param config The configuration to add to.
/// \param problems Filled with everything wrong with the script.
///
/// \returns Nothing on success, or the reason the file could not be used.
///
/// \remarks A missing file is an error here. Passing `--config` and silently
///          getting the default behaviour is the kind of failure noticed weeks
///          later, as a diff quietly read by the wrong provider. The search
///          paths are treated differently by their caller, because a home
///          directory with no configuration in it is the ordinary case.
[[nodiscard]] Result<std::monostate, ConfigError> loadConfigScript(
    const std::filesystem::path& path, ProviderConfig& config,
    std::vector<ConfigProblem>& problems);

/// \brief Reads every configuration file that exists, in order.
///
/// \param explicitPath A file named on the command line, or empty.
/// \param config The configuration to fill in.
/// \param problems Filled with everything wrong with any of them.
///
/// \returns Nothing on success, or the reason a named file could not be used.
///
/// \remarks The home-directory files are read if they are there and passed over
///          if they are not. A file named with `--config` must exist, because
///          asking for it and not getting it is the failure worth catching.
[[nodiscard]] Result<std::monostate, ConfigError> loadConfiguration(
    const std::filesystem::path& explicitPath, ProviderConfig& config,
    std::vector<ConfigProblem>& problems);

}  // namespace nmtreediff
