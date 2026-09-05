#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/registry.h"
#include "core/source.h"

namespace fs = std::filesystem;

using nmxd::ConfigError;
using nmxd::ConfigProblem;
using nmxd::ProviderConfig;
using nmxd::SourceFile;

namespace {

ProviderConfig parse(std::string_view text, std::vector<ConfigProblem>& problems) {
    return nmxd::parseProviderConfig(text, problems);
}

ProviderConfig parseClean(std::string_view text) {
    std::vector<ConfigProblem> problems;
    auto config = parse(text, problems);
    INFO("unexpected problems: " << problems.size());
    CHECK(problems.empty());
    return config;
}

SourceFile makeSource(std::string text, std::string name) {
    return SourceFile::fromMemory(std::move(text), name, name);
}

}  // namespace

TEST_CASE("a configuration maps extensions to providers", "[config]") {
    const auto config = parseClean(
        "# The studio's own suffixes.\n"
        ".bt = bt\n"
        ".asset = json\n"
        "\n"
        "fallback = json\n");

    REQUIRE(config.extensions.size() == 2);
    CHECK(config.extensions[0].first == ".bt");
    CHECK(config.extensions[0].second == "bt");
    CHECK(config.extensions[1].first == ".asset");
    CHECK(config.extensions[1].second == "json");
    CHECK(config.fallback == "json");
    CHECK_FALSE(config.empty());
}

TEST_CASE("whitespace, comments and blank lines are ignored", "[config]") {
    const auto config = parseClean(
        "\n"
        "   # a comment on its own line\n"
        "   .BT   =   bt      # and one after a mapping\n"
        "\t\n");

    REQUIRE(config.extensions.size() == 1);
    // Lower-cased on load, because Windows is case-insensitive about extensions
    // and an exporter is not always consistent.
    CHECK(config.extensions[0].first == ".bt");
    CHECK(config.extensions[0].second == "bt");
}

TEST_CASE("an empty configuration asks for nothing", "[config]") {
    const auto config = parseClean("# nothing but a comment\n");
    CHECK(config.empty());
}

TEST_CASE("a last line with no newline still counts", "[config]") {
    const auto config = parseClean(".bt = bt");
    REQUIRE(config.extensions.size() == 1);
    CHECK(config.extensions[0].second == "bt");
}

TEST_CASE("every bad line is reported, not just the first", "[config]") {
    // Someone fixing a configuration should see the whole list rather than one
    // mistake per run.
    std::vector<ConfigProblem> problems;
    const auto config = parse(
        ".bt = bt\n"
        "this line has no equals sign\n"
        ".json =\n"
        "colour = red\n"
        ". = bt\n",
        problems);

    REQUIRE(problems.size() == 4);
    CHECK(problems[0].line == 2);
    CHECK(problems[1].line == 3);
    CHECK(problems[2].line == 4);
    CHECK(problems[3].line == 5);

    // The good line is still there, because the caller decides what a partly
    // understood file means rather than the parser deciding for it.
    REQUIRE(config.extensions.size() == 1);
    CHECK(config.extensions[0].first == ".bt");
}

TEST_CASE("a missing configuration file is an error", "[config]") {
    // Passing --config and silently getting the default behaviour is the kind
    // of failure that is noticed weeks later.
    std::vector<ConfigProblem> problems;
    const auto loaded =
        nmxd::loadProviderConfig(fs::path(NMXD_TESTDATA_DIR) / "no-such-file.conf", problems);
    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error() == ConfigError::NotFound);
}

TEST_CASE("a configuration file loads from disk", "[config]") {
    const fs::path path =
        fs::temp_directory_path() / "nmxd_test_providers.conf";
    {
        std::ofstream out(path, std::ios::binary);
        out << "# studio formats\n.bt = bt\nfallback = xml\n";
    }

    std::vector<ConfigProblem> problems;
    const auto loaded = nmxd::loadProviderConfig(path, problems);
    REQUIRE(loaded.ok());
    CHECK(problems.empty());
    REQUIRE(loaded.value().extensions.size() == 1);
    CHECK(loaded.value().extensions[0].second == "bt");
    CHECK(loaded.value().fallback == "xml");

    fs::remove(path);
}

TEST_CASE("a configured extension beats sniffing", "[config][registry]") {
    auto registry = nmxd::makeDefaultRegistry();

    // Without the override this file is JSON by any reading of it.
    const auto source = makeSource("{\"a\": 1}", "level.data");
    REQUIRE(registry.resolve(source) != nullptr);
    CHECK(registry.resolve(source)->name() == "json");

    CHECK(registry.apply(ProviderConfig{{{".data", "xml"}}, {}}).empty());
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
    // A file that names an extension twice should behave the way a reader of it
    // would expect rather than keeping whichever line came first.
    auto registry = nmxd::makeDefaultRegistry();
    CHECK(registry.apply(ProviderConfig{{{".data", "xml"}, {".data", "bt"}}, {}}).empty());

    REQUIRE(registry.overrideFor(".data") != nullptr);
    CHECK(registry.overrideFor(".data")->name() == "bt");
}

TEST_CASE("an unknown provider name is reported, not ignored", "[config][registry]") {
    // A typo in a studio-wide configuration would otherwise send every artist's
    // diff quietly through the wrong provider.
    auto registry = nmxd::makeDefaultRegistry();
    const auto unknown = registry.apply(ProviderConfig{{{".bt", "behaviour-tree"}}, "yaml"});

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

    CHECK(registry.apply(ProviderConfig{{}, "json"}).empty());
    REQUIRE(registry.resolve(source) != nullptr);
    CHECK(registry.resolve(source)->name() == "json");
}
