#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app/cli.h"
#include "app/report.h"
#include "core/config.h"
#include "core/diff.h"
#include "core/lua_config.h"
#include "core/lua_provider.h"
#include "core/registry.h"
#include "core/snapshot.h"
#include "core/source.h"
#include "core/textdiff.h"

// The headless report is what a build job reads, so what the tree says about
// dropped content and failed shaping jobs has to reach it, and only one of
// the two may fail the run.

using nmxd::ConfigProblem;
using nmxd::DiffSnapshot;
using nmxd::Options;
using nmxd::ProviderConfig;
using nmxd::ReportFormat;
using nmxd::SourceFile;
using nmxd::Stage;
using nmxd::Tree;

namespace {

/// A script that keeps <keep> elements, drops <skip>, and raises on <bad>.
constexpr const char* kScript =
    "provider 'picky' {\n"
    "  base = 'xml',\n"
    "  shape = function(doc, out)\n"
    "    local root = out:root(doc.root)\n"
    "    for child in doc.root:children() do\n"
    "      out:next(function(element, parent)\n"
    "        if element.name == 'bad' then error('cannot shape this') end\n"
    "        if element.name == 'keep' then parent:child(element) end\n"
    "      end, child, root)\n"
    "    end\n"
    "  end,\n"
    "}\n";

struct Fixture {
    nmxd::ProviderRegistry registry = nmxd::makeDefaultRegistry();
    DiffSnapshot snapshot;

    /// Compares two documents under one provider: the picky script by
    /// default, or a built-in by name for what the script's bare nodes cannot
    /// show.
    Fixture(const std::string& leftText, const std::string& rightText,
            const char* providerName = "picky") {
        ProviderConfig config;
        std::vector<ConfigProblem> problems;
        REQUIRE(nmxd::runConfigScript(kScript, "test.lua", config, problems));
        REQUIRE(nmxd::addScriptedProviders(registry, config).empty());
        const auto* provider = registry.byName(providerName);
        REQUIRE(provider != nullptr);

        auto left = std::make_shared<SourceFile>(SourceFile::fromMemory(leftText, "left", "left.xml"));
        auto right =
            std::make_shared<SourceFile>(SourceFile::fromMemory(rightText, "right", "right.xml"));
        auto leftTree = provider->parse(*left, {});
        auto rightTree = provider->parse(*right, {});
        REQUIRE(leftTree.ok());
        REQUIRE(rightTree.ok());

        snapshot.stage = Stage::TreeReady;
        snapshot.left = left;
        snapshot.right = right;
        snapshot.text = std::make_shared<nmxd::TextDiff>(nmxd::diffText(*left, *right, {}));
        snapshot.leftTree = std::make_shared<Tree>(std::move(leftTree).value());
        snapshot.rightTree = std::make_shared<Tree>(std::move(rightTree).value());
        snapshot.provider = provider;
        snapshot.treeDiff = std::make_shared<nmxd::DiffModel>(
            nmxd::diffTrees(*snapshot.leftTree, *snapshot.rightTree, *provider));
    }

    std::string report(ReportFormat format, bool exitCode, int& code) {
        Options options;
        options.headless = true;
        options.report = format;
        options.useExitCode = exitCode;
        std::ostringstream out;
        code = nmxd::writeReport(out, snapshot, options);
        return out.str();
    }
};

}  // namespace

TEST_CASE("dropped content is counted and warned about, and never fails the run", "[report]") {
    Fixture fixture("<r><keep/><skip a='1'/></r>", "<r><keep/><skip a='2'/></r>");

    int code = 0;
    const std::string text = fixture.report(ReportFormat::Text, true, code);
    CHECK(text.find("warning: left: the format left out 1 stretch of the file") != std::string::npos);
    CHECK(text.find("warning: right: the format left out 1 stretch of the file") != std::string::npos);
    CHECK(text.find("failed:") == std::string::npos);
    // The lines differ inside what the format left out, and the tree, which
    // decides, is identical: leaving it out was the format's decision. Zero,
    // then, and dropping never turns it into a two either.
    CHECK(text.find("lines: +0 -0 ~1 across 1 change") != std::string::npos);
    CHECK(code == 0);

    const std::string json = fixture.report(ReportFormat::Json, true, code);
    CHECK(json.find("\"dropped\": { \"left\": 1, \"right\": 1 }") != std::string::npos);
    CHECK(json.find("\"failures\": []") != std::string::npos);
    CHECK(code == 0);
}

TEST_CASE("the tree decides the verdict, so a reformat is not a change", "[report]") {
    // The whole adoption argument in one pair: the same document, reindented.
    // The exit code and the identical field used to read the line diff, so a
    // report that printed "identical" for the tree exited 1 anyway.
    Fixture fixture("<r><keep a='1'/></r>", "<r>\n  <keep a='1'/>\n</r>\n", "xml");

    int code = 0;
    const std::string text = fixture.report(ReportFormat::Text, true, code);
    CHECK(text.find("lines: +") != std::string::npos);
    CHECK(text.find("\nidentical\n") != std::string::npos);
    CHECK(code == 0);

    const std::string json = fixture.report(ReportFormat::Json, true, code);
    CHECK(json.find("\"comparison\": \"tree\"") != std::string::npos);
    CHECK(json.find("\"identical\": true") != std::string::npos);
    CHECK(code == 0);
}

TEST_CASE("the JSON report lists every change the text report lists", "[report]") {
    // Counts alone gave automation strictly less than a person got. Each entry
    // carries what the text line carries: status, the path on each side, and
    // the properties that differ.
    Fixture fixture("<r><keep a='1'/></r>", "<r><keep a='2'/><keep/></r>", "xml");

    int code = 0;
    const std::string text = fixture.report(ReportFormat::Text, true, code);
    CHECK(text.find("~ /r/keep[0] [a]\n+ /r/keep[1]\n") != std::string::npos);
    CHECK(code == 1);

    const std::string json = fixture.report(ReportFormat::Json, true, code);
    CHECK(json.find("\"changes\": [\n"
                    "      { \"status\": \"modified\", \"moved\": false, \"left\": \"/r/keep[0]\", "
                    "\"right\": \"/r/keep[0]\", \"properties\": [\"a\"] },\n"
                    "      { \"status\": \"added\", \"moved\": false, \"left\": null, "
                    "\"right\": \"/r/keep[1]\", \"properties\": [] }\n"
                    "    ]\n") != std::string::npos);
    CHECK(json.find("\"trimmedContainers\": 0") != std::string::npos);
    CHECK(json.find("\"identical\": false") != std::string::npos);
    CHECK(code == 1);
}

TEST_CASE("without a format the lines decide", "[report]") {
    // A file no format claims has only its lines to go by, and the report
    // says so rather than pretending to a tree it does not have.
    auto left = std::make_shared<SourceFile>(SourceFile::fromMemory("a b\n", "left", "left.txt"));
    auto right = std::make_shared<SourceFile>(SourceFile::fromMemory("a  b\n", "right", "right.txt"));
    DiffSnapshot snapshot;
    snapshot.stage = Stage::TextReady;
    snapshot.left = left;
    snapshot.right = right;
    snapshot.text = std::make_shared<nmxd::TextDiff>(nmxd::diffText(*left, *right, {}));

    Options options;
    options.headless = true;
    options.report = ReportFormat::Json;
    options.useExitCode = true;
    std::ostringstream out;
    const int code = nmxd::writeReport(out, snapshot, options);
    CHECK(out.str().find("\"comparison\": \"lines\"") != std::string::npos);
    CHECK(out.str().find("\"identical\": false") != std::string::npos);
    CHECK(code == 1);
}

TEST_CASE("a failed shaping job is listed with its line and fails the run", "[report]") {
    Fixture fixture("<r>\n  <keep/>\n</r>", "<r>\n  <keep/>\n  <bad/>\n</r>");

    int code = 0;
    const std::string text = fixture.report(ReportFormat::Text, false, code);
    CHECK(text.find("failed: right:3: ") != std::string::npos);
    CHECK(text.find("cannot shape this") != std::string::npos);
    // Two, whatever --exit-code says: a script that raised is a bug and a
    // build job must not read the comparison as sound.
    CHECK(code == 2);

    const std::string json = fixture.report(ReportFormat::Json, true, code);
    CHECK(json.find("\"failures\": [\n    { \"side\": \"right\", \"line\": 3, \"message\": \"") !=
          std::string::npos);
    CHECK(json.find("cannot shape this") != std::string::npos);
    CHECK(code == 2);
}
