#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "core/config.h"
#include "core/file_filters.h"
#include "core/lua_config.h"
#include "core/lua_provider.h"
#include "core/registry.h"

using nmtreediff::FileFilter;
using nmtreediff::ProviderConfig;

namespace {

/// Finds the entry with a name, or fails the test.
const FileFilter& entryNamed(const std::vector<FileFilter>& filters, const std::string& name) {
    for (const FileFilter& filter : filters) {
        if (filter.name == name) {
            return filter;
        }
    }
    FAIL("no filter called " << name);
    return filters.front();
}

/// Whether a comma-separated spec lists an extension.
bool admits(const FileFilter& filter, const std::string& extension) {
    const std::string padded = "," + filter.extensions + ",";
    return padded.find("," + extension + ",") != std::string::npos;
}

}  // namespace

TEST_CASE("the dialog offers every built-in format and everything first", "[filters]") {
    const auto registry = nmtreediff::makeDefaultRegistry();
    const auto filters = nmtreediff::fileFiltersFor(registry);

    REQUIRE(filters.size() >= 2);
    CHECK(filters.front().name == "Tree data");

    // The first entry admits what every other entry admits.
    for (std::size_t i = 1; i < filters.size(); ++i) {
        INFO(filters[i].name << ": " << filters[i].extensions);
        std::string spec = filters[i].extensions + ",";
        while (!spec.empty()) {
            const std::size_t comma = spec.find(',');
            CHECK(admits(filters.front(), spec.substr(0, comma)));
            spec.erase(0, comma + 1);
        }
    }

    // The built-ins by the names they show, with their own extensions.
    CHECK(admits(entryNamed(filters, "XML (generic)"), "xml"));
    CHECK(admits(entryNamed(filters, "JSON (generic)"), "json"));
    CHECK(admits(filters.front(), "bt"));

    // No dots and no upper case anywhere, which is the form the dialog takes.
    for (const FileFilter& filter : filters) {
        CHECK(filter.extensions.find('.') == std::string::npos);
        for (const char c : filter.extensions) {
            CHECK_FALSE((c >= 'A' && c <= 'Z'));
        }
    }
}

TEST_CASE("a scripted format gets an entry under its own name", "[filters]") {
    // R21's example: a Blackboard format claiming .blackboard is offered as
    // "Blackboard", and the everything entry admits .blackboard too.
    ProviderConfig config;
    std::vector<nmtreediff::ConfigProblem> problems;
    REQUIRE(nmtreediff::runConfigScript(
        "provider 'blackboard' {\n"
        "  display_name = 'Blackboard',\n"
        "  base = 'xml',\n"
        "  extensions = { '.Blackboard', '.bb' },\n"
        "}\n"
        "provider 'silent' { base = 'json' }\n",
        "test.lua", config, problems));
    auto registry = nmtreediff::makeDefaultRegistry();
    REQUIRE(nmtreediff::addScriptedProviders(registry, config).empty());

    const auto filters = nmtreediff::fileFiltersFor(registry);
    const FileFilter& blackboard = entryNamed(filters, "Blackboard");
    CHECK(blackboard.extensions == "blackboard,bb");
    CHECK(admits(filters.front(), "blackboard"));
    CHECK(admits(filters.front(), "bb"));
    CHECK(admits(filters.front(), "xml"));

    // A format claiming nothing has nothing to filter by, so no entry.
    for (const FileFilter& filter : filters) {
        CHECK(filter.name != "silent");
    }
}

TEST_CASE("a configured extension is admitted under the format it points at", "[filters]") {
    ProviderConfig config;
    std::vector<nmtreediff::ConfigProblem> problems;
    REQUIRE(nmtreediff::runConfigScript("formats { ['.leveldata'] = 'json', ['.XML'] = 'bt' }\n",
                                        "test.lua", config, problems));
    auto registry = nmtreediff::makeDefaultRegistry();
    REQUIRE(registry.apply(config).empty());

    const auto filters = nmtreediff::fileFiltersFor(registry);
    CHECK(admits(entryNamed(filters, "JSON (generic)"), "leveldata"));
    CHECK(admits(filters.front(), "leveldata"));

    // .xml now resolves to the behaviour tree, so it is credited there as
    // well as staying on XML's own list; the everything entry lists it once.
    CHECK(admits(entryNamed(filters, "Behavior tree (XML)"), "xml"));
    CHECK(admits(entryNamed(filters, "XML (generic)"), "xml"));
    CHECK(filters.front().extensions.find("xml,") == filters.front().extensions.rfind("xml,"));
}
