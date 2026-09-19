/// \file
/// \brief Implementation of reading the configuration scripts.

#include "app/configure.h"

#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/lua_config.h"
#include "core/lua_provider.h"
#include "core/registry.h"

namespace nmtreediff {

namespace {

/// \brief Writes one configuration problem to the log.
///
/// \param problem What was wrong.
///
/// \remarks One line naming the file, the line and the message, so the list
///          reads as a list. When the script left a traceback behind, it
///          follows on its own lines, because a person fixing a provider wants
///          to know which of their functions raised rather than only that one
///          did.
void report(const ConfigProblem& problem) {
    std::string line = "nmtreediff: " + problem.origin.string();
    if (problem.line != 0) {
        line += ":" + std::to_string(problem.line);
    }
    line += ": " + problem.message;
    logErr(line);
    if (!problem.detail.empty()) {
        logErr(problem.detail);
    }
}

}  // namespace

bool loadConfiguration(Options& options) {
    std::vector<ConfigProblem> problems;
    ProviderConfig config;
    const auto loaded = nmtreediff::loadConfiguration(options.configPath, config, problems);

    for (const ConfigProblem& problem : problems) {
        report(problem);
    }

    // A file that was not there at all has explained nothing above.
    if (!loaded.ok() && problems.empty()) {
        logErr("nmtreediff: " + options.configPath.string() + ": " + describe(loaded.error()));
    }

    // Names are checked even when the script already failed, so that one run
    // shows every mistake rather than the first one hiding the rest. Checked
    // here rather than where the configuration is applied, so the run stops
    // before any work starts.
    ProviderRegistry probe = makeDefaultRegistry();
    std::vector<std::string> unknown = addScriptedProviders(probe, config);
    const std::vector<std::string> rest = probe.apply(config);
    unknown.insert(unknown.end(), rest.begin(), rest.end());
    for (const std::string& name : unknown) {
        logErr("nmtreediff: no format called " + name + "; try --list-formats");
    }

    if (!loaded.ok() || !problems.empty() || !unknown.empty()) {
        return false;
    }

    options.providerConfig = std::move(config);
    return true;
}

}  // namespace nmtreediff
