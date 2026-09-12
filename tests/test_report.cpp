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

    Fixture(const std::string& leftText, const std::string& rightText) {
        ProviderConfig config;
        std::vector<ConfigProblem> problems;
        REQUIRE(nmxd::runConfigScript(kScript, "test.lua", config, problems));
        REQUIRE(nmxd::addScriptedProviders(registry, config).empty());
        const auto* provider = registry.byName("picky");
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
    // The exit code follows the comparison, one here because the lines
    // differ, and dropping never turns it into a two.
    CHECK(code == 1);

    const std::string json = fixture.report(ReportFormat::Json, true, code);
    CHECK(json.find("\"dropped\": { \"left\": 1, \"right\": 1 }") != std::string::npos);
    CHECK(json.find("\"failures\": []") != std::string::npos);
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
