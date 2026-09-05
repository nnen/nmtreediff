/// \file
/// \brief Implementation of provider configuration loading.

#include "core/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

namespace nmxd {

namespace {

/// \brief The one key that is not an extension.
constexpr std::string_view kFallbackKey = "fallback";

/// \brief Removes leading and trailing whitespace.
///
/// \param text The text to trim.
///
/// \returns The same view with the whitespace on both ends removed.
std::string_view trim(std::string_view text) {
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// \brief Lower-cases a string.
///
/// \param text The text to convert.
///
/// \returns A lower-case copy.
std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

const char* describe(ConfigError error) noexcept {
    switch (error) {
        case ConfigError::NotFound:
            return "no file at that path";
        case ConfigError::NotReadable:
            return "the file could not be read";
        case ConfigError::Malformed:
            return "the file was not understood";
    }
    return "unknown error";
}

ProviderConfig parseProviderConfig(std::string_view text, std::vector<ConfigProblem>& problems) {
    ProviderConfig config;

    std::uint32_t lineNumber = 0;
    std::size_t position = 0;
    while (position <= text.size()) {
        ++lineNumber;
        const std::size_t breakAt = text.find('\n', position);
        std::string_view line =
            text.substr(position, breakAt == std::string_view::npos ? std::string_view::npos
                                                                    : breakAt - position);
        position = breakAt == std::string_view::npos ? text.size() + 1 : breakAt + 1;

        // A comment runs to the end of the line. Nothing in a key or a provider
        // name can contain a hash, so there is no quoting to get wrong.
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            problems.push_back(
                ConfigProblem{lineNumber, "expected a line of the form \"key = value\""});
            continue;
        }

        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (key.empty()) {
            problems.push_back(ConfigProblem{lineNumber, "the key is missing"});
            continue;
        }
        if (value.empty()) {
            problems.push_back(ConfigProblem{lineNumber,
                                             "\"" + std::string(key) + "\" has no value"});
            continue;
        }

        if (key == kFallbackKey) {
            config.fallback = value;
            continue;
        }
        if (key.front() == '.') {
            if (key.size() == 1) {
                problems.push_back(ConfigProblem{lineNumber, "\".\" is not an extension"});
                continue;
            }
            config.extensions.emplace_back(lower(key), std::string(value));
            continue;
        }

        problems.push_back(ConfigProblem{
            lineNumber, "\"" + std::string(key) +
                            "\" is neither an extension, which starts with a dot, nor \"fallback\""});
    }

    return config;
}

Result<ProviderConfig, ConfigError> loadProviderConfig(const std::filesystem::path& path,
                                                       std::vector<ConfigProblem>& problems) {
    std::error_code status;
    if (!std::filesystem::exists(path, status) || status) {
        return fail(ConfigError::NotFound);
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(ConfigError::NotReadable);
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    if (!in && !in.eof()) {
        return fail(ConfigError::NotReadable);
    }

    return parseProviderConfig(text, problems);
}

}  // namespace nmxd
