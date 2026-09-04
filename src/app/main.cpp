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

int runHeadless(const nmxd::Options& options) {
    nmxd::Session session;
    session.open(nmxd::SessionRequest{options.leftPath, options.rightPath, options.leftLabel,
                                      options.rightLabel});
    session.waitIdle();

    const auto snapshot = session.snapshot();
    static const nmxd::DiffSnapshot kEmpty;
    return nmxd::writeReport(std::cout, snapshot ? *snapshot : kEmpty, options);
}

}  // namespace

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
