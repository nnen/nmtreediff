#pragma once

// Headless output. This path runs the same pipeline as the window, which is
// what lets the end-to-end tests exercise everything without a display.

#include <iosfwd>

#include "app/cli.h"
#include "core/snapshot.h"

namespace nmxd {

// Writes the snapshot to the stream in the requested format and returns the
// process exit code.
//
// At M0 the comparison is a byte comparison, because no diff engine exists
// yet. The report says so, so nobody mistakes it for a tree diff.
[[nodiscard]] int writeReport(std::ostream& out, const DiffSnapshot& snapshot,
                              const Options& options);

}  // namespace nmxd
