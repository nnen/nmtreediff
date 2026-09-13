#pragma once

/// \file
/// \brief Reading the configuration scripts, at startup and again on Reload.

#include "app/cli.h"

namespace nmxd {

/// \brief Reads every configuration script that applies to this run.
///
/// \param options The run's options, whose configuration is filled in on
///        success and left untouched on failure.
///
/// \returns `true` when there was nothing to read or every script was good.
///
/// \remarks Every problem is written to the log before giving up, so someone
///          fixing a configuration sees the whole list rather than one line
///          per run. A bad configuration is not partly applied: a diff read by
///          the wrong provider looks like a working diff, which is the failure
///          that goes unnoticed longest.
///
///          The same function serves startup and the window's Reload, so the
///          two cannot drift apart in what they accept.
[[nodiscard]] bool loadConfiguration(Options& options);

}  // namespace nmxd
