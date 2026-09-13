/// \file
/// \brief Process entry point and the choice between headless and windowed.

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "app/cli.h"
#include "app/configure.h"
#include "app/console.h"
#include "app/report.h"
#include "core/config.h"
#include "core/log.h"
#include "core/lua_provider.h"
#include "core/registry.h"
#include "core/session.h"

#if NMXD_HAVE_GUI
#include "ui/app_window.h"
#endif

namespace {

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

    // Before anything is written: a GUI-subsystem process has to find its
    // console or pipe, and the sink has to know whether it found one.
    const nmxd::OutputStreams streams = nmxd::attachToCaller();
    nmxd::Log::instance().forwardTo(streams.out, streams.err);

    nmxd::ParseResult parsed = nmxd::parseCommandLine(argc, argv);
    if (!parsed.shouldRun()) {
        return parsed.exitCode;
    }
    nmxd::Options options = std::move(*parsed.options);

    if (!nmxd::loadConfiguration(options)) {
        return 2;
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
        nmxd::logErr("nmxmldiff: could not open a window; try --headless");
        return 2;
    }
    return window.run();
#else
    (void)processStart;
    nmxd::logErr("nmxmldiff: built without the GUI; use --headless");
    return 2;
#endif
}
