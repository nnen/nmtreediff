/// \file
/// \brief Process entry point and the choice between headless and windowed.

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "app/cli.h"
#include "app/report.h"
#include "core/config.h"
#include "core/lua_config.h"
#include "core/lua_provider.h"
#include "core/registry.h"
#include "core/session.h"

#if NMXD_HAVE_GUI
#include "ui/app_window.h"
#endif

namespace {

/// \brief Reads every configuration script that applies to this run.
///
/// \param options The run's options, whose configuration is filled in.
///
/// \returns Zero when there was nothing to read or every script was good,
///          and 2 when one could not be used.
///
/// \remarks Every problem is reported before giving up, so someone fixing a
///          configuration sees the whole list rather than one line per run. A
///          bad configuration stops the run rather than being partly applied: a
///          diff read by the wrong provider looks like a working diff, which is
///          the failure that goes unnoticed longest.
int loadConfiguration(nmxd::Options& options) {
    std::vector<nmxd::ConfigProblem> problems;
    nmxd::ProviderConfig config;
    const auto loaded = nmxd::loadConfiguration(options.configPath, config, problems);

    for (const auto& problem : problems) {
        std::cerr << "nmxmldiff: " << problem.origin.string();
        if (problem.line != 0) {
            std::cerr << ":" << problem.line;
        }
        std::cerr << ": " << problem.message << '\n';
    }

    // A file that was not there at all has explained nothing above.
    if (!loaded.ok() && problems.empty()) {
        std::cerr << "nmxmldiff: " << options.configPath.string() << ": "
                  << nmxd::describe(loaded.error()) << '\n';
    }

    // Names are checked even when the script already failed, so that one run
    // shows every mistake rather than the first one hiding the rest. Checked
    // here rather than where the configuration is applied, so the run stops
    // before any work starts.
    nmxd::ProviderRegistry probe = nmxd::makeDefaultRegistry();
    std::vector<std::string> unknown = nmxd::addScriptedProviders(probe, config);
    const std::vector<std::string> rest = probe.apply(config);
    unknown.insert(unknown.end(), rest.begin(), rest.end());
    for (const auto& name : unknown) {
        std::cerr << "nmxmldiff: no format called " << name << "; try --list-formats" << '\n';
    }

    if (!loaded.ok() || !problems.empty() || !unknown.empty()) {
        return 2;
    }

    options.providerConfig = std::move(config);
    return 0;
}

/// \brief Prints the formats this build knows.
///
/// \param options The run's options, whose configuration is applied first.
///
/// \returns Zero.
///
/// \remarks Printed after the configuration is applied, so the listing shows
///          the extensions this machine actually resolves rather than the ones
///          the build shipped with.
int listFormats(const nmxd::Options& options) {
    // Scripted formats are added the same way the session adds them, because a
    // format that works but does not appear here is the one a person gives up
    // looking for.
    nmxd::ProviderRegistry registry = nmxd::makeDefaultRegistry();
    (void)nmxd::addScriptedProviders(registry, options.providerConfig);
    (void)registry.apply(options.providerConfig);

    std::cout << "provider interface version " << nmxd::kProviderInterfaceVersion << '\n';
    for (const auto name : registry.names()) {
        const nmxd::IFormatProvider* provider = registry.byName(name);
        std::cout << "  " << name << "  " << provider->displayName() << '\n'
                  << "    default extensions:";
        for (const auto extension : provider->defaultExtensions()) {
            std::cout << ' ' << extension;
        }
        std::cout << '\n';
    }

    if (!options.providerConfig.extensions.empty()) {
        std::cout << "configured overrides:" << '\n';
        for (const auto& [extension, providerName] : options.providerConfig.extensions) {
            std::cout << "  " << extension << " -> " << providerName << '\n';
        }
    }
    return 0;
}

/// \brief Runs the whole pipeline with no window and writes a report.
///
/// \param options The run's options.
///
/// \returns The process exit code from nmxd::writeReport().
int runHeadless(const nmxd::Options& options) {
    nmxd::Session session;
    (void)session.configureProviders(options.providerConfig);
    session.open(nmxd::SessionRequest{options.leftPath, options.rightPath, options.leftLabel,
                                      options.rightLabel, options.format});
    session.waitIdle();

    const auto snapshot = session.snapshot();
    static const nmxd::DiffSnapshot kEmpty;
    return nmxd::writeReport(std::cout, snapshot ? *snapshot : kEmpty, options);
}

}  // namespace

/// \brief Process entry point.
///
/// \param argc Argument count, including the program name.
/// \param argv Argument vector, including the program name.
///
/// \returns The process exit code.
///
/// \remarks Captures the start time before anything else happens, so the
///          startup budget covers everything the user waits for rather than
///          only the part after initialisation.
int main(int argc, char** argv) {
    // Captured first so the startup budget covers everything the user waits
    // for, not just the part after initialisation.
    const auto processStart = std::chrono::steady_clock::now();

    nmxd::ParseResult parsed = nmxd::parseCommandLine(argc, argv);
    if (!parsed.shouldRun()) {
        return parsed.exitCode;
    }
    nmxd::Options options = std::move(*parsed.options);

    if (const int failed = loadConfiguration(options)) {
        return failed;
    }
    if (options.listFormats) {
        return listFormats(options);
    }

    if (options.headless) {
        return runHeadless(options);
    }

#if NMXD_HAVE_GUI
    nmxd::AppWindow window(options, processStart);
    if (!window.open()) {
        std::fprintf(stderr, "could not open a window; try --headless\n");
        return 2;
    }
    return window.run();
#else
    (void)processStart;
    std::fprintf(stderr, "built without the GUI; use --headless\n");
    return 2;
#endif
}
