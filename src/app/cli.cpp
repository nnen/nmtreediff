/// \file
/// \brief Implementation of command line parsing.

#include "app/cli.h"

#include <CLI/CLI.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace nmxd {

namespace {

/// \brief The one-line description shown by `--help`.
constexpr const char* kDescription =
    "Diff tree-shaped data in XML and JSON, as text and as a node graph.";

/// \brief Declares every option on an app and parses one argument list.
///
/// \param app The CLI11 app to declare options on.
/// \param reversedArgs The arguments in reverse order, which is how CLI11
///        consumes them.
///
/// \returns The parsed options, or an exit code when parsing failed or when a
///          help or version request was handled.
ParseResult parseInto(CLI::App& app, std::vector<std::string> reversedArgs) {
    Options options;
    std::string viewName = "text";
    std::string reportName = "text";

    app.add_option("left", options.leftPath, "Left (older) file");
    app.add_option("right", options.rightPath, "Right (newer) file");

    app.add_option("--format", options.format,
                   "Format provider to use; sniffed from the file when omitted");
    app.add_option("--left-label", options.leftLabel, "Title to show for the left side");
    app.add_option("--right-label", options.rightLabel, "Title to show for the right side");
    app.add_option("--view", viewName, "Initial view")
        ->check(CLI::IsMember({"text", "node"}));
    app.add_option("--config", options.configPath,
                   "Provider configuration file mapping extensions to formats");
    app.add_flag("--list-formats", options.listFormats,
                 "Print the formats this build knows, with their extensions, and exit");

    app.add_flag("--headless", options.headless, "Run without opening a window");
    app.add_option("--report", reportName, "Headless output format")
        ->check(CLI::IsMember({"text", "json"}));
    app.add_flag("--exit-code", options.useExitCode,
                 "Exit 0 when the inputs match and 1 when they differ");
    app.add_option("--max-frames", options.maxFrames,
                   "Render this many frames, print the timings and exit (0 runs normally)");
    app.add_option("--screenshot", options.screenshotPath,
                   "Save the last rendered frame to this file as a bitmap");

    try {
        // CLI11 consumes the vector from the back, which is why the caller
        // hands it over already reversed.
        app.parse(std::move(reversedArgs));
    } catch (const CLI::ParseError& error) {
        return ParseResult{std::nullopt, app.exit(error)};
    }

    // Listing formats answers a question about the build rather than about a
    // pair of files, so it is the one mode that needs no inputs at all.
    if (options.listFormats) {
        return ParseResult{std::move(options), 0};
    }

    const bool onlyOneGiven = options.leftPath.empty() != options.rightPath.empty();
    if (onlyOneGiven || (options.headless && !options.hasInputs())) {
        std::cerr << "nmxmldiff: two files are required"
                  << (options.headless ? " in headless mode" : "") << '\n'
                  << "usage: nmxmldiff [options] <left> <right>\n";
        return ParseResult{std::nullopt, 2};
    }

    options.view = (viewName == "node") ? InitialView::Node : InitialView::Text;
    options.report = (reportName == "json") ? ReportFormat::Json : ReportFormat::Text;

    if (options.leftLabel.empty()) {
        options.leftLabel = options.leftPath.string();
    }
    if (options.rightLabel.empty()) {
        options.rightLabel = options.rightPath.string();
    }

    return ParseResult{std::move(options), 0};
}

}  // namespace

ParseResult parseArguments(const std::vector<std::string>& arguments) {
    CLI::App app{kDescription, "nmxmldiff"};
    app.set_version_flag("--version", std::string(NMXD_VERSION));

    std::vector<std::string> reversed(arguments.rbegin(), arguments.rend());
    return parseInto(app, std::move(reversed));
}

ParseResult parseCommandLine(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    return parseArguments(arguments);
}

}  // namespace nmxd
