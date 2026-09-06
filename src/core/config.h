#pragma once

/// \file
/// \brief The settings a configuration script asks for.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/provider.h"
#include "core/result.h"

namespace nmxd {

/// \brief A format provider defined by a script rather than compiled in.
///
/// \remarks Holds only what the script said. Turning it into something that
///          implements IFormatProvider is the bridge's job, and it needs the
///          interpreter the script was read from, which is why this carries a
///          name rather than a function.
struct ScriptedProviderSpec {
    /// \brief The name `--format` accepts for this provider.
    std::string name;
    /// \brief The name shown to a person.
    std::string displayName;
    /// \brief The built-in format whose parse this one shapes.
    std::string base;
    /// \brief Extensions this format claims, lower-cased and with their dots.
    std::vector<std::string> extensions;
    /// \brief Which way this format's graph reads best.
    GraphDirection direction = GraphDirection::Inherit;

    /// \brief Property names that sort first, in the order they sort.
    ///
    /// \remarks Presentation only, and never matching: properties are
    ///          compared as an unordered set whatever this says. It decides
    ///          what the node card, the details panel and the change list show
    ///          first, which also makes the change list read the same way twice.
    std::vector<std::string> propertyOrder;
    /// \brief Which script declared it, for a message that names the file.
    std::filesystem::path origin;

    /// \brief The text of that script.
    ///
    /// \remarks Carried rather than re-read, because each worker builds its
    ///          own interpreter and reloading from disk would let the file
    ///          change underneath a comparison that is halfway through.
    std::string source;
};

/// \brief What a configuration script asks the tool to do.
struct ProviderConfig {
    /// \brief Extension to provider name, in the order the script listed them.
    ///
    /// \remarks Extensions include their leading dot and are lower-cased on
    ///          load, so a file called LEVEL.BT resolves the same way as
    ///          level.bt.
    std::vector<std::pair<std::string, std::string>> extensions;

    /// \brief The provider used when nothing else claims a file, or empty to
    ///        leave the registry's own choice alone.
    std::string fallback;

    /// \brief The graph direction to start in, or Inherit to leave it alone.
    GraphDirection graphDirection = GraphDirection::Inherit;

    /// \brief The key that closes the window, or empty to leave it alone.
    ///
    /// \remarks Lower-cased. The word `none` means no key closes the window,
    ///          which is the answer for anyone whose muscle memory says Escape
    ///          means something else.
    std::string exitKey;

    /// \brief Providers the scripts defined.
    std::vector<ScriptedProviderSpec> providers;

    /// \brief Reports whether the configuration asks for anything.
    ///
    /// \returns `true` when it holds nothing at all.
    [[nodiscard]] bool empty() const noexcept {
        return extensions.empty() && fallback.empty() && exitKey.empty() && providers.empty() &&
               graphDirection == GraphDirection::Inherit;
    }
};

/// \brief Why a configuration file could not be used.
enum class ConfigError {
    NotFound,     ///< No file at that path.
    NotReadable,  ///< The file exists but could not be read.
    Malformed,    ///< The script did not run, or asked for something unknown.
};

/// \brief Converts a configuration error into a phrase suitable for a message.
///
/// \param error The error to describe.
///
/// \returns A short lower-case phrase, never null.
[[nodiscard]] const char* describe(ConfigError error) noexcept;

/// \brief One thing wrong with a configuration file.
struct ConfigProblem {
    /// \brief The one-based line the problem is on, or zero when unknown.
    ///
    /// \remarks A Lua error usually carries a line; a complaint about what a
    ///          script asked for often does not, because by then the script has
    ///          finished running.
    std::uint32_t line = 0;

    /// \brief What is wrong with it, phrased for the person who wrote the file.
    std::string message;

    /// \brief Which file the problem is in.
    std::filesystem::path origin;
};

/// \brief The order configuration files are read in.
///
/// \returns The home-directory scripts, most general first. A path is returned
///          whether or not a file is there.
///
/// \remarks Only the user's own files. Configuration is deliberately not
///          searched for beside the files being compared: a version control
///          system usually hands over temporary extracts, so the walk would
///          find a temporary directory rather than a repository, and a script
///          arriving with a file someone sent you is code you did not choose to
///          run. Anything else is named with `--config`.
[[nodiscard]] std::vector<std::filesystem::path> configSearchPaths();

}  // namespace nmxd
