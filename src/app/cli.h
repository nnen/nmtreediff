#pragma once

/// \file
/// \brief The command line surface.

#include <filesystem>

#include "core/config.h"
#include <optional>
#include <string>
#include <vector>

namespace nmtreediff {

/// \brief Which view opens first.
enum class InitialView {
    Text,  ///< The text view.
    Node,  ///< The node view.
};

/// \brief How a headless run reports its result.
enum class ReportFormat {
    Text,  ///< Lines meant for a person.
    Json,  ///< A single object meant for a script.
};

/// \brief Everything the command line can set.
struct Options {
    /// \brief Path to the left, usually older, file.
    ///
    /// \remarks May be empty: launched with no arguments the window opens in an
    ///          empty state rather than flashing a console and exiting.
    std::filesystem::path leftPath;

    /// \brief Path to the right, usually newer, file. May be empty.
    std::filesystem::path rightPath;

    /// \brief Title to show for the left side, defaulting to the path.
    std::string leftLabel;
    /// \brief Title to show for the right side, defaulting to the path.
    std::string rightLabel;

    /// \brief The format provider to force, or empty to sniff one.
    std::string format;
    /// \brief Which view opens first.
    InitialView view = InitialView::Text;
    /// \brief A configuration file to load.
    std::filesystem::path configPath;

    /// \brief What that configuration file said, once it has been read.
    ///
    /// \remarks Filled in after parsing, because reading a file is not the
    ///          command line's job. Empty when no configuration was asked for.
    ProviderConfig providerConfig;

    /// \brief Print the formats this build knows and exit.
    ///
    /// \remarks The answer to "what does --format accept" and to "why did my
    ///          file resolve to that", which are the two questions a studio asks
    ///          while setting the tool up.
    bool listFormats = false;

    /// \brief Whether to run without opening a window.
    bool headless = false;
    /// \brief How a headless run reports its result.
    ReportFormat report = ReportFormat::Text;
    /// \brief Whether to exit 0 for identical inputs and 1 for different ones.
    bool useExitCode = false;

    /// \brief Save the last rendered frame to this file, then exit.
    ///
    /// \remarks Empty saves nothing. Meant for looking at the interface without
    ///          a person watching it, and for catching a visual regression that
    ///          no assertion would notice.
    std::filesystem::path screenshotPath;

    /// \brief Render this many frames, print the timings and exit.
    ///
    /// \remarks Zero runs until the window is closed. This is how the budget
    ///          test measures startup and frame time without a person watching
    ///          the window.
    unsigned maxFrames = 0;

    /// \brief The key that closes the window, or empty to use the
    ///        configured one.
    ///
    /// \remarks Overrides whatever a configuration script asked for, the way
    ///          `--format` overrides a resolved format. The word `none` means no
    ///          key closes the window.
    std::string exitKey;

    /// \brief Reports whether a file pair was given.
    ///
    /// \returns `true` when both paths are set.
    [[nodiscard]] bool hasInputs() const noexcept {
        return !leftPath.empty() && !rightPath.empty();
    }
};

/// \brief The outcome of parsing a command line.
struct ParseResult {
    /// \brief The parsed options, or nothing when the process should just exit.
    std::optional<Options> options;

    /// \brief The exit code to return when \ref options is empty.
    ///
    /// \remarks Zero after `--help` or `--version` printed their output, and
    ///          non-zero when the arguments were wrong.
    int exitCode = 0;

    /// \brief Reports whether the program should carry on.
    ///
    /// \returns `true` when options were produced.
    [[nodiscard]] bool shouldRun() const noexcept { return options.has_value(); }
};

/// \brief Parses a process command line.
///
/// \param argc Argument count, including the program name.
/// \param argv Argument vector, including the program name.
///
/// \returns The options to run with, or an exit code.
///
/// \remarks Perforce lets a user define the argument order for a custom diff
///          tool, so plain named options and two positional paths are enough and
///          no tolerance for unusual argument shapes is needed.
[[nodiscard]] ParseResult parseCommandLine(int argc, char** argv);

/// \brief Parses an argument list without a program name.
///
/// \param arguments The arguments, in the order they were given.
///
/// \returns The options to run with, or an exit code.
///
/// \remarks Exposed for tests, which build an argument vector rather than a
///          process.
[[nodiscard]] ParseResult parseArguments(const std::vector<std::string>& arguments);

}  // namespace nmtreediff
