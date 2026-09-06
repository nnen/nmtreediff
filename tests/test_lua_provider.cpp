#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/diff.h"
#include "core/lua_config.h"
#include "core/lua_provider.h"
#include "core/registry.h"
#include "core/source.h"

// The bridge is judged against the compiled behaviour-tree provider rather than
// against a plausible answer. The two read the same files and must produce the
// same trees, the same matches and the same change list. If they disagree,
// either the bridge is losing something or the scripted surface cannot say
// something the compiled provider can, and the difference names which case.

namespace fs = std::filesystem;

using nmxd::ConfigProblem;
using nmxd::GraphDirection;
using nmxd::IFormatProvider;
using nmxd::ProviderConfig;
using nmxd::SourceFile;
using nmxd::Tree;

namespace {

fs::path samplePath(const char* name) { return fs::path(NMXD_TESTDATA_DIR) / "sample" / name; }

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// Builds a registry with the sample script's provider in it.
nmxd::ProviderRegistry registryWithScript(const std::string& script) {
    ProviderConfig config;
    std::vector<ConfigProblem> problems;
    REQUIRE(nmxd::runConfigScript(script, "test.lua", config, problems));

    auto registry = nmxd::makeDefaultRegistry();
    REQUIRE(nmxd::addScriptedProviders(registry, config).empty());
    return registry;
}

Tree parseWith(const nmxd::ProviderRegistry& registry, const char* format, const char* sample) {
    const auto* provider = registry.byName(format);
    REQUIRE(provider != nullptr);

    const auto source = SourceFile::load(samplePath(sample), sample);
    REQUIRE(source.ok());
    auto tree = provider->parse(source.value(), {});
    REQUIRE(tree.ok());
    return std::move(tree).value();
}

}  // namespace

TEST_CASE("a scripted provider builds the same tree as the compiled one", "[lua]") {
    const auto registry = registryWithScript(readFile(samplePath("behaviortree.lua")));

    const Tree compiled = parseWith(registry, "bt", "guard_before.bt");
    const Tree scripted = parseWith(registry, "bt-lua", "guard_before.bt");

    REQUIRE(compiled.size() == scripted.size());
    for (nmxd::NodeId id = 0; id < compiled.size(); ++id) {
        INFO("node " << id);
        const auto& left = compiled.node(id);
        const auto& right = scripted.node(id);
        CHECK(left.kind == right.kind);
        CHECK(left.children.size() == right.children.size());
        CHECK(left.span.begin == right.span.begin);
        CHECK(left.span.end == right.span.end);

        REQUIRE(left.properties.size() == right.properties.size());
        for (std::size_t i = 0; i < left.properties.size(); ++i) {
            CHECK(left.properties[i].name == right.properties[i].name);
            CHECK(left.properties[i].value == right.properties[i].value);
        }
    }
}

TEST_CASE("a scripted provider reports the same changes", "[lua]") {
    // The whole pipeline, not just the parse: identity, matching and the change
    // list all have to agree, which is what makes this worth more than a tree
    // comparison on its own.
    const auto registry = registryWithScript(readFile(samplePath("behaviortree.lua")));

    const Tree compiledLeft = parseWith(registry, "bt", "guard_before.bt");
    const Tree compiledRight = parseWith(registry, "bt", "guard_after.bt");
    const Tree scriptedLeft = parseWith(registry, "bt-lua", "guard_before.bt");
    const Tree scriptedRight = parseWith(registry, "bt-lua", "guard_after.bt");

    const auto* compiled = registry.byName("bt");
    const auto* scripted = registry.byName("bt-lua");
    REQUIRE(compiled != nullptr);
    REQUIRE(scripted != nullptr);

    const auto compiledDiff = nmxd::diffTrees(compiledLeft, compiledRight, *compiled);
    const auto scriptedDiff = nmxd::diffTrees(scriptedLeft, scriptedRight, *scripted);

    CHECK(compiledDiff.added == scriptedDiff.added);
    CHECK(compiledDiff.deleted == scriptedDiff.deleted);
    CHECK(compiledDiff.modified == scriptedDiff.modified);
    CHECK(compiledDiff.moved == scriptedDiff.moved);

    CHECK(nmxd::serializeChanges(compiledLeft, compiledRight, compiledDiff) ==
          nmxd::serializeChanges(scriptedLeft, scriptedRight, scriptedDiff));
}

TEST_CASE("a scripted identity survives a change of kind", "[lua]") {
    // The thing no structural heuristic can do, and the reason the surface has
    // a word for a strong key rather than inferring one.
    const auto registry = registryWithScript(readFile(samplePath("behaviortree.lua")));
    const auto* provider = registry.byName("bt-lua");
    REQUIRE(provider != nullptr);

    const Tree left = parseWith(registry, "bt-lua", "guard_before.bt");
    const Tree right = parseWith(registry, "bt-lua", "guard_after.bt");
    const auto model = nmxd::diffTrees(left, right, *provider);

    // "Stand easy" is an Idle before and a LookAround after, and it keeps its
    // identifier. It has to survive as one modified node rather than becoming a
    // deletion beside an addition.
    const std::string changes = nmxd::serializeChanges(left, right, model);
    CHECK(changes.find("LookAround[0] [type") != std::string::npos);
    CHECK(changes.find("- /behaviortree/Selector[0]/Idle") == std::string::npos);
}

TEST_CASE("a scripted provider states its own graph direction", "[lua]") {
    const auto registry = registryWithScript(readFile(samplePath("behaviortree.lua")));
    const auto* provider = registry.byName("bt-lua");
    REQUIRE(provider != nullptr);
    CHECK(provider->graphDirection() == GraphDirection::LeftToRight);
    CHECK(provider->displayName() == "Behavior tree (script)");
}

TEST_CASE("a script may claim extensions and win them", "[lua]") {
    // Registered ahead of the built-ins, so a format that names an extension
    // gets it rather than losing a tie to the format it is built on.
    const auto registry = registryWithScript(
        "provider 'mine' {\n"
        "  base = 'xml',\n"
        "  extensions = { '.MINE' },\n"
        "  is_node = function(e) return true end,\n"
        "}\n");

    const auto source = SourceFile::fromMemory("<r/>", "thing.mine", "thing.mine");
    REQUIRE(registry.resolve(source) != nullptr);
    CHECK(registry.resolve(source)->name() == "mine");
}

TEST_CASE("a provider built on a format that does not exist is reported", "[lua]") {
    ProviderConfig config;
    std::vector<ConfigProblem> problems;
    REQUIRE(nmxd::runConfigScript("provider 'x' { base = 'yaml' }\n", "test.lua", config,
                                  problems));

    auto registry = nmxd::makeDefaultRegistry();
    const auto unknown = nmxd::addScriptedProviders(registry, config);
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0] == "yaml");
    CHECK(registry.byName("x") == nullptr);
}

TEST_CASE("a script that keeps every element still parses", "[lua]") {
    // The smallest possible provider: one that changes nothing. It should read
    // exactly like the format it is built on, which is what says the bridge
    // adds nothing of its own.
    const auto registry = registryWithScript("provider 'passthrough' { base = 'xml' }\n");

    const auto source = SourceFile::fromMemory("<r><a x='1'/><b/></r>", "t.xml", "t.xml");
    const auto* scripted = registry.byName("passthrough");
    const auto* plain = registry.byName("xml");
    REQUIRE(scripted != nullptr);
    REQUIRE(plain != nullptr);

    auto shaped = scripted->parse(source, {});
    auto generic = plain->parse(source, {});
    REQUIRE(shaped.ok());
    REQUIRE(generic.ok());
    CHECK(shaped.value().size() == generic.value().size());
}

TEST_CASE("a script that will not stop is cancelled", "[lua][slow]") {
    // Constraint C makes no exception for code the user wrote. A script with a
    // loop in it would otherwise hold a worker for ever, and switching format
    // or reloading a file has to be able to take that worker back.
    const auto registry = registryWithScript(
        "provider 'spinner' {\n"
        "  base = 'xml',\n"
        "  is_node = function(e)\n"
        "    while true do end\n"
        "  end,\n"
        "}\n");

    const auto* provider = registry.byName("spinner");
    REQUIRE(provider != nullptr);

    const auto source = SourceFile::fromMemory("<r><a/></r>", "t.xml", "t.xml");

    std::stop_source stopping;
    std::thread canceller([&stopping] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stopping.request_stop();
    });

    const auto started = std::chrono::steady_clock::now();
    const auto parsed = provider->parse(source, stopping.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    canceller.join();

    // It gave up rather than running to the end, and it did so promptly.
    CHECK_FALSE(parsed.ok());
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 2000);
}
