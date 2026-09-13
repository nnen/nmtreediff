#pragma once

/// \file
/// \brief Headless output for scripts and continuous integration.

#include <iosfwd>

#include "app/cli.h"
#include "core/snapshot.h"

namespace nmxd {

/// \brief Writes a finished snapshot to a stream.
///
/// \param out Where to write the report.
/// \param snapshot The snapshot to report on.
/// \param options The run's options, which choose the format and whether the
///        exit code reflects the result.
///
/// \returns The process exit code: 2 when the comparison could not be produced
///          or a shaping job failed, otherwise 1 for differing inputs and 0
///          for identical ones when Options::useExitCode is set, and 0
///          otherwise.
///
/// \remarks This path runs the same pipeline as the window, which is what lets
///          the end-to-end tests exercise everything without a display.
///
///          Whether the inputs differ is the tree's verdict whenever a format
///          resolved, and the line diff's only when none did. The report says
///          which under `comparison`.
[[nodiscard]] int writeReport(std::ostream& out, const DiffSnapshot& snapshot,
                              const Options& options);

}  // namespace nmxd
