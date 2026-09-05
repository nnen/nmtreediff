/// \file
/// \brief Process entry point and the choice between headless and windowed.

#include <chrono>
#include <cstdio>
#include <iostream>

#include "app/cli.h"
#include "app/report.h"
#include "core/session.h"

#if NMXD_HAVE_GUI
#include "ui/app_window.h"
#endif

namespace {

/// \brief Runs the whole pipeline with no window and writes a report.
///
/// \param options The run's options.
///
/// \returns The process exit code from nmxd::writeReport().
int runHeadless(const nmxd::Options& options) {
    nmxd::Session session;
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

    const nmxd::ParseResult parsed = nmxd::parseCommandLine(argc, argv);
    if (!parsed.shouldRun()) {
        return parsed.exitCode;
    }
    const nmxd::Options& options = *parsed.options;

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
