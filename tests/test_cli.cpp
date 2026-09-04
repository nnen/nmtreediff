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

TEST_CASE("one path alone is refused", "[cli]") {
    // Half a pair is a mistake rather than a deliberate empty start.
    const auto one = parseArguments({"only.xml"});
    CHECK_FALSE(one.shouldRun());
    CHECK(one.exitCode != 0);
}

TEST_CASE("headless still requires both paths", "[cli]") {
    const auto parsed = parseArguments({"--headless"});
    CHECK_FALSE(parsed.shouldRun());
    CHECK(parsed.exitCode != 0);
}

TEST_CASE("an unknown view is refused rather than guessed", "[cli]") {
    const auto parsed = parseArguments({"--view", "graph", "a.xml", "b.xml"});
    CHECK_FALSE(parsed.shouldRun());
    CHECK(parsed.exitCode != 0);
}
