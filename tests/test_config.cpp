#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/lua_config.h"
#include "core/registry.h"
#include "core/source.h"

namespace fs = std::filesystem;

using nmxd::ConfigError;
using nmxd::ConfigProblem;
using nmxd::GraphDirection;
using nmxd::ProviderConfig;
using nmxd::SourceFile;

namespace {

/// Runs a script and expects it to be understood in full.
ProviderConfig runClean(const std::string& script) {
    ProviderConfig config;
    std::vector<ConfigProblem> problems;
    const bool ok = nmxd::runConfigScript(script, "test.lua", config, problems);
    for (const auto& problem : problems) {
        UNSCOPED_INFO("problem: " << problem.message);
    }
    CHECK(problems.empty());
    CHECK(ok);
    return config;
}

/// Runs a script and collects what was wrong with it.
std::vector<ConfigProblem> runDirty(const std::string& script, ProviderConfig& config) {
    std::vector<ConfigProblem> problems;
    CHECK_FALSE(nmxd::runConfigScript(script, "test.lua", config, problems));
    return problems;
}

SourceFile makeSource(std::string text, std::string name) {
    return SourceFile::fromMemory(std::move(text), name, name);
}

}  // namespace

TEST_CASE("a script maps extensions to providers", "[config]") {
    const auto config = runClean(
        "-- The studio's own suffixes.\n"
        "formats {\n"
        "  ['.bt'] = 'bt',\n"
        "  ['.asset'] = 'json',\n"
        "}\n"
        "fallback 'json'\n");

    REQUIRE(config.extensions.size() == 2);
    CHECK(config.fallback == "json");
    CHECK_FALSE(config.empty());

    // A Lua table has no order of its own, so the pair is looked up rather than
    // indexed. Order only matters between files, not inside one.
    bool sawBt = false;
    bool sawAsset = false;
    for (const auto& [extension, provider] : config.extensions) {
        sawBt = sawBt || (extension == ".bt" && provider == "bt");
        sawAsset = sawAsset || (extension == ".asset" && provider == "json");
    }
    CHECK(sawBt);
    CHECK(sawAsset);
}

TEST_CASE("extensions are matched without regard to case", "[config]") {
    // Windows is careless about extensions and an exporter is not always
    // consistent.
    const auto config = runClean("formats { ['.BT'] = 'bt' }\n");
    REQUIRE(config.extensions.size() == 1);
    CHECK(config.extensions[0].first == ".bt");
}

TEST_CASE("a script sets the graph direction and the exit key", "[config]") {
    const auto config = runClean("graph_direction 'left_to_right'\nexit_key 'None'\n");
    CHECK(config.graphDirection == GraphDirection::LeftToRight);
    CHECK(config.exitKey == "none");
}

TEST_CASE("an empty script asks for nothing", "[config]") {
    const auto config = runClean("-- nothing but a comment\n");
    CHECK(config.empty());
    CHECK(config.graphDirection == GraphDirection::Inherit);
}

TEST_CASE("a script may compute its configuration", "[config]") {
    // The reason for a scripting language rather than a table of pairs: a
    // studio with twenty suffixes should write a loop, not twenty lines.
    const auto config = runClean(
        "local map = {}\n"
        "for _, suffix in ipairs{'.lvl', '.ent', '.mat'} do\n"
        "  map[suffix] = 'json'\n"
        "end\n"
        "formats(map)\n");
    CHECK(config.extensions.size() == 3);
}

TEST_CASE("a script that will not parse is reported with its line", "[config]") {
    ProviderConfig config;
    const auto problems = runDirty("formats {\nthis is not lua\n", config);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].line == 2);
    CHECK_FALSE(problems[0].message.empty());
}

TEST_CASE("a script that raises keeps one line as the message and the rest as detail",
          "[config]") {
    // The listing wants one line per problem. The person fixing the script
    // wants to know which of their functions raised, which is the traceback.
    ProviderConfig config;
    const auto problems = runDirty(
        "local function deep()\n"
        "  error('boom')\n"
        "end\n"
        "deep()\n",
        config);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message == "boom");
    CHECK(problems[0].line == 2);
    CHECK(problems[0].message.find('\n') == std::string::npos);
    CHECK(problems[0].detail.find("stack traceback") != std::string::npos);
    CHECK(problems[0].detail.find("deep") != std::string::npos);
}

TEST_CASE("a script that asks for something impossible is reported", "[config]") {
    ProviderConfig config;
    const auto problems = runDirty(
        "graph_direction 'sideways'\n"
        "formats { ['bt'] = 'bt' }\n",
        config);

    // Both mistakes, not just the first: someone fixing a configuration should
    // see the whole list rather than one per run.
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].message.find("sideways") != std::string::npos);
    CHECK(problems[1].message.find("dot") != std::string::npos);
}

TEST_CASE("a missing configuration file is an error", "[config]") {
    // Passing --config and silently getting the default behaviour is the kind
    // of failure that is noticed weeks later.
    ProviderConfig config;
    std::vector<ConfigProblem> problems;
    const auto loaded =
        nmxd::loadConfigScript(fs::path(NMXD_TESTDATA_DIR) / "no-such-file.lua", config, problems);
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error() == ConfigError::NotFound);
}

TEST_CASE("a configuration file loads from disk", "[config]") {
    const fs::path path = fs::temp_directory_path() / "nmxd_test_config.lua";
    {
        std::ofstream out(path, std::ios::binary);
        out << "formats { ['.bt'] = 'bt' }\nfallback 'xml'\n";
    }

    ProviderConfig config;
    std::vector<ConfigProblem> problems;
    const auto loaded = nmxd::loadConfigScript(path, config, problems);
    REQUIRE(loaded.ok());
    CHECK(problems.empty());
    REQUIRE(config.extensions.size() == 1);
    CHECK(config.fallback == "xml");

    fs::remove(path);
}

TEST_CASE("a later script overrides an earlier one", "[config]") {
    // This is what makes the search order mean anything: the home file is the
    // general answer and --config is the specific one.
    ProviderConfig config;
    std::vector<ConfigProblem> problems;
    CHECK(nmxd::runConfigScript("fallback 'xml'\ngraph_direction 'top_down'\n", "home.lua",
                                config, problems));
    CHECK(nmxd::runConfigScript("fallback 'json'\n", "explicit.lua", config, problems));

    CHECK(problems.empty());
    CHECK(config.fallback == "json");
    // And leaves alone what the later file did not mention.
    CHECK(config.graphDirection == GraphDirection::TopDown);
}

TEST_CASE("a configured extension beats sniffing", "[config][registry]") {
    auto registry = nmxd::makeDefaultRegistry();

    // Without the override this file is JSON by any reading of it.
    const auto source = makeSource("{\"a\": 1}", "level.data");
    REQUIRE(registry.resolve(source) != nullptr);
    CHECK(registry.resolve(source)->name() == "json");

    CHECK(registry.mapExtension(".data", "xml"));
    REQUIRE(registry.resolve(source) != nullptr);
    CHECK(registry.resolve(source)->name() == "xml");

    // But an explicit format still wins, because that is a person correcting a
    // guess right now rather than a standing decision.
    CHECK(registry.resolve(source, "json")->name() == "json");
}

TEST_CASE("an override is matched without regard to case", "[config][registry]") {
    auto registry = nmxd::makeDefaultRegistry();
    CHECK(registry.mapExtension(".Data", "xml"));

    REQUIRE(registry.overrideFor(".DATA") != nullptr);
    CHECK(registry.overrideFor(".DATA")->name() == "xml");
    CHECK(registry.overrideFor(".other") == nullptr);
}

TEST_CASE("the last mapping of an extension wins", "[config][registry]") {
    auto registry = nmxd::makeDefaultRegistry();
    ProviderConfig config;
    config.extensions = {{".data", "xml"}, {".data", "bt"}};
    CHECK(registry.apply(config).empty());

    REQUIRE(registry.overrideFor(".data") != nullptr);
    CHECK(registry.overrideFor(".data")->name() == "bt");
}

TEST_CASE("an unknown provider name is reported, not ignored", "[config][registry]") {
    // A typo in a studio-wide configuration would otherwise send every artist's
    // diff quietly through the wrong provider.
    auto registry = nmxd::makeDefaultRegistry();
    ProviderConfig config;
    config.extensions = {{".bt", "behaviour-tree"}};
    config.fallback = "yaml";
    const auto unknown = registry.apply(config);

    REQUIRE(unknown.size() == 2);
    CHECK(unknown[0] == "behaviour-tree");
    CHECK(unknown[1] == "yaml");

    // And nothing was applied from the bad entries.
    CHECK(registry.overrideFor(".bt") == nullptr);
}

TEST_CASE("a configured fallback catches what nothing claims", "[config][registry]") {
    auto registry = nmxd::makeDefaultRegistry();
    const auto source = makeSource("neither one thing nor the other", "mystery.dat");

    REQUIRE(registry.resolve(source) != nullptr);
    CHECK(registry.resolve(source)->name() == "xml");

    ProviderConfig config;
    config.fallback = "json";
    CHECK(registry.apply(config).empty());
    REQUIRE(registry.resolve(source) != nullptr);
    CHECK(registry.resolve(source)->name() == "json");
}
