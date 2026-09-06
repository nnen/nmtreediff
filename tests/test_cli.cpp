#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "app/cli.h"

using nmxd::InitialView;
using nmxd::parseArguments;
using nmxd::ReportFormat;

TEST_CASE("two paths are enough", "[cli]") {
    const auto parsed = parseArguments({"left.xml", "right.xml"});
    REQUIRE(parsed.shouldRun());
    const auto& options = *parsed.options;
    CHECK(options.leftPath == "left.xml");
    CHECK(options.rightPath == "right.xml");
    CHECK_FALSE(options.headless);
    CHECK(options.view == InitialView::Text);
}

TEST_CASE("labels default to the paths", "[cli]") {
    const auto parsed = parseArguments({"left.xml", "right.xml"});
    REQUIRE(parsed.shouldRun());
    CHECK(parsed.options->leftLabel == "left.xml");
    CHECK(parsed.options->rightLabel == "right.xml");
}

TEST_CASE("a version control system can override the titles", "[cli]") {
    const auto parsed = parseArguments(
        {"--left-label", "//depot/tree.xml#4", "--right-label", "workspace", "a.xml", "b.xml"});
    REQUIRE(parsed.shouldRun());
    CHECK(parsed.options->leftLabel == "//depot/tree.xml#4");
    CHECK(parsed.options->rightLabel == "workspace");
}

TEST_CASE("options may come before or after the paths", "[cli]") {
    // Perforce lets a user define the argument order, so both shapes have to
    // parse to the same thing.
    const auto before = parseArguments({"--view", "node", "a.xml", "b.xml"});
    const auto after = parseArguments({"a.xml", "b.xml", "--view", "node"});

    REQUIRE(before.shouldRun());
    REQUIRE(after.shouldRun());
    CHECK(before.options->view == InitialView::Node);
    CHECK(after.options->view == InitialView::Node);
    CHECK(before.options->leftPath == after.options->leftPath);
    CHECK(before.options->rightPath == after.options->rightPath);
}

TEST_CASE("the headless reporting switches parse", "[cli]") {
    const auto parsed =
        parseArguments({"--headless", "--report", "json", "--exit-code", "a.xml", "b.xml"});
    REQUIRE(parsed.shouldRun());
    CHECK(parsed.options->headless);
    CHECK(parsed.options->report == ReportFormat::Json);
    CHECK(parsed.options->useExitCode);
}

TEST_CASE("a format can be forced", "[cli]") {
    const auto parsed = parseArguments({"--format", "bt-xml", "a.xml", "b.xml"});
    REQUIRE(parsed.shouldRun());
    CHECK(parsed.options->format == "bt-xml");
}

TEST_CASE("no arguments opens an empty window rather than exiting", "[cli]") {
    // Double-clicking the executable used to flash a console and vanish. With
    // no paths the window is expected to open and explain itself instead.
    const auto none = parseArguments({});
    REQUIRE(none.shouldRun());
    CHECK(none.exitCode == 0);
    CHECK_FALSE(none.options->hasInputs());
}

TEST_CASE("one path alone starts the window with that side chosen", "[cli]") {
    // Half a pair used to be refused. Now that files can be chosen in the
    // window, it is a starting point: the given side is kept and the other is
    // asked for.
    const auto one = parseArguments({"only.xml"});
    REQUIRE(one.shouldRun());
    CHECK(one.exitCode == 0);
    CHECK_FALSE(one.options->hasInputs());
    CHECK(one.options->leftPath == "only.xml");
    CHECK(one.options->rightPath.empty());
}

TEST_CASE("a label is not invented for a path that was not given", "[cli]") {
    // A label stands in for a path. Defaulting it from an empty path would put
    // an empty title on a side nobody has chosen yet.
    const auto one = parseArguments({"only.xml"});
    REQUIRE(one.shouldRun());
    CHECK(one.options->leftLabel == "only.xml");
    CHECK(one.options->rightLabel.empty());
}

TEST_CASE("headless still requires both paths", "[cli]") {
    // Headless has nobody to ask, so the rule that relaxed for the window
    // cannot relax here.
    const auto neither = parseArguments({"--headless"});
    CHECK_FALSE(neither.shouldRun());
    CHECK(neither.exitCode != 0);

    const auto one = parseArguments({"--headless", "only.xml"});
    CHECK_FALSE(one.shouldRun());
    CHECK(one.exitCode != 0);
}

TEST_CASE("an unknown view is refused rather than guessed", "[cli]") {
    const auto parsed = parseArguments({"--view", "graph", "a.xml", "b.xml"});
    CHECK_FALSE(parsed.shouldRun());
    CHECK(parsed.exitCode != 0);
}
