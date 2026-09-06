/// \file
/// \brief Implementation of the configuration values and where they come from.

#include "core/config.h"

#include <cstdlib>

namespace nmxd {

namespace {

/// \brief The configuration file directly under the home directory.
constexpr const char* kHomeScript = ".nmtreediff.lua";

/// \brief The directory a longer configuration lives in.
constexpr const char* kHomeDirectory = ".nmtreediff";

/// \brief The file inside that directory.
constexpr const char* kHomeDirectoryScript = "config.lua";

/// \brief Environment variables naming the home directory, in order of trust.
///
/// \remarks HOME first, because a user who sets it means it. USERPROFILE is
///          what Windows sets, and is the fallback rather than the first
///          choice for the same reason.
constexpr const char* kHomeVariables[] = {"HOME", "USERPROFILE"};

/// \brief Finds the user's home directory.
///
/// \returns The directory, or an empty path when nothing names one.
[[nodiscard]] std::filesystem::path homeDirectory() {
    for (const char* name : kHomeVariables) {
#ifdef _MSC_VER
        // getenv is deprecated on this toolchain, and the replacement hands
        // back memory of its own.
        std::size_t length = 0;
        char* value = nullptr;
        if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
            std::filesystem::path home(value);
            std::free(value);
            if (!home.empty()) {
                return home;
            }
        }
#else
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            return std::filesystem::path(value);
        }
#endif
    }
    return {};
}

}  // namespace

const char* describe(ConfigError error) noexcept {
    switch (error) {
        case ConfigError::NotFound:
            return "no file there";
        case ConfigError::NotReadable:
            return "the file could not be read";
        case ConfigError::Malformed:
            return "the script could not be used";
    }
    return "unknown error";
}

std::vector<std::filesystem::path> configSearchPaths() {
    const std::filesystem::path home = homeDirectory();
    if (home.empty()) {
        return {};
    }
    return {home / kHomeScript, home / kHomeDirectory / kHomeDirectoryScript};
}

}  // namespace nmxd
