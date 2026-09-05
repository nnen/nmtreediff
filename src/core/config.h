#pragma once

/// \file
/// \brief The configuration file that maps a studio's file extensions to
///        providers.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"

namespace nmxd {

/// \brief What a configuration file asks the registry to do.
///
/// \remarks Deliberately small. The only thing a studio cannot express any
///          other way is which provider handles its own file extensions, and a
///          configuration format grows teeth the moment it can express more
///          than that.
struct ProviderConfig {
    /// \brief Extension to provider name, in the order the file listed them.
    ///
    /// \remarks Extensions include their leading dot and are lower-cased on
    ///          load, so a file called LEVEL.BT resolves the same way as
    ///          level.bt.
    std::vector<std::pair<std::string, std::string>> extensions;

    /// \brief The provider used when nothing else claims a file, or empty to
    ///        leave the registry's own choice alone.
    std::string fallback;

    /// \brief Reports whether the configuration asks for anything.
    ///
    /// \returns `true` when it holds no mappings and no fallback.
    [[nodiscard]] bool empty() const noexcept { return extensions.empty() && fallback.empty(); }
};

/// \brief Why a configuration file could not be used.
enum class ConfigError {
    NotFound,   ///< No file at that path.
    NotReadable,///< The file exists but could not be read.
    Malformed,  ///< At least one line was not understood.
};

/// \brief Converts a configuration error into a phrase suitable for a message.
///
/// \param error The error to describe.
///
/// \returns A short lower-case phrase, never null.
[[nodiscard]] const char* describe(ConfigError error) noexcept;

/// \brief One thing wrong with a configuration file.
struct ConfigProblem {
    /// \brief The one-based line the problem is on.
    std::uint32_t line = 0;
    /// \brief What is wrong with it, phrased for the person who wrote the file.
    std::string message;
};

/// \brief Reads a configuration from text.
///
/// \param text The file's contents.
/// \param problems Filled with everything wrong with the text. Never cleared,
///        so several files can accumulate into one list.
///
/// \returns The configuration, which is complete only when \p problems gained
///          nothing.
///
/// \remarks The format is one `key = value` per line, `#` starts a comment, and
///          blank lines are ignored. A key beginning with a dot is an
///          extension; the only other key is `fallback`.
///
///          Every line is read even after one fails, so a person fixing a
///          configuration sees every mistake at once rather than one per run.
[[nodiscard]] ProviderConfig parseProviderConfig(std::string_view text,
                                                 std::vector<ConfigProblem>& problems);

/// \brief Reads a configuration from a file.
///
/// \param path The file to read.
/// \param problems Filled with everything wrong with the file.
///
/// \returns The configuration, or a ConfigError when the file could not be
///          read at all.
///
/// \remarks A missing file is an error rather than an empty configuration.
///          Passing `--config` and silently getting the default behaviour is
///          the kind of failure that is noticed weeks later, in the form of a
///          diff that was quietly read by the wrong provider.
[[nodiscard]] Result<ProviderConfig, ConfigError> loadProviderConfig(
    const std::filesystem::path& path, std::vector<ConfigProblem>& problems);

}  // namespace nmxd
