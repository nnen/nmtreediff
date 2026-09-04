#pragma once

// Command line surface. Perforce lets a user define the argument order for a
// custom diff tool, so this needs no tolerance for unusual argument shapes:
// plain named options and two positional paths are enough.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nmxd {

enum class InitialView {
    Text,
    Node,
};

enum class ReportFormat {
    Text,
    Json,
};

struct Options {
    std::filesystem::path leftPath;
    std::filesystem::path rightPath;

    std::string leftLabel;   // empty means use the path
    std::string rightLabel;

    std::string format;      // empty means sniff the provider
    InitialView view = InitialView::Text;
    std::filesystem::path configPath;

    bool headless = false;
    ReportFormat report = ReportFormat::Text;
    bool useExitCode = false;

    // Run this many frames and exit, printing the startup and frame timings.
    // Zero runs until the window is closed. This is how the budget test
    // measures startup without a person watching the window.
    unsigned maxFrames = 0;
};

// The outcome of parsing: either options to run with, or a process exit code
// because help or a version string was printed, or the arguments were wrong.
struct ParseResult {
    std::optional<Options> options;
    int exitCode = 0;

    [[nodiscard]] bool shouldRun() const noexcept { return options.has_value(); }
};

[[nodiscard]] ParseResult parseCommandLine(int argc, char** argv);

// Exposed for tests, which build an argument vector rather than a process.
[[nodiscard]] ParseResult parseArguments(const std::vector<std::string>& arguments);

}  // namespace nmxd
