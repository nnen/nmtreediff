#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "core/diff.h"
#include "core/source.h"
#include "core/textdiff.h"
#include "formats/json_generic.h"

using namespace std::chrono;
using nmxd::SourceFile;
using nmxd::TextDiffQuality;

namespace {

// A generated asset file: repetitive, machine-written, and large, which is
// what the tool is aimed at rather than hand-edited configuration.
std::string generateTree(std::size_t nodes, std::size_t modifyEvery) {
    std::string out;
    out.reserve(nodes * 96);
    out += "<behaviortree version=\"2\">\n";
    for (std::size_t i = 0; i < nodes; ++i) {
        const bool tweak = modifyEvery > 0 && (i % modifyEvery) == 0;
        out += "  <node id=\"n";
        out += std::to_string(i);
        out += "\" type=\"MoveTo\" name=\"Step ";
        out += std::to_string(i);
        out += "\">\n    <property name=\"speed\" value=\"";
        out += tweak ? "1.4" : "1.0";
        out += "\"/>\n    <property name=\"waypoint\" value=\"wp_";
        out += std::to_string(i % 64);
        out += "\"/>\n  </node>\n";
    }
    out += "</behaviortree>\n";
    return out;
}

std::stop_token neverStopped() {
    static std::stop_source source;
    return source.get_token();
}

}  // namespace

TEST_CASE("a 20 MB pair diffs inside the milestone budget", "[budget][.slow]") {
    // Sized to land near twenty megabytes a side, the figure the plan set as
    // the point at which the text view has to stay usable.
    constexpr std::size_t kNodes = 150000;

    const auto leftText = generateTree(kNodes, 0);
    const auto rightText = generateTree(kNodes, 500);

    INFO("left bytes " << leftText.size() << ", right bytes " << rightText.size());
    CHECK(leftText.size() > 15u * 1024 * 1024);

    const auto left = SourceFile::fromMemory(leftText, "left");
    const auto right = SourceFile::fromMemory(rightText, "right");

    const auto started = steady_clock::now();
    const auto diff = nmxd::diffText(left, right, neverStopped());
    const double millis = duration<double, std::milli>(steady_clock::now() - started).count();

    INFO("diff took " << millis << " ms, "
                      << "rows " << diff.rows.size() << ", changed " << diff.changedRows());

    CHECK(diff.quality == TextDiffQuality::Full);
    CHECK(diff.modifiedRows == kNodes / 500);
    CHECK(millis < 800.0);
}

TEST_CASE("a pair that shares nothing still finishes", "[budget][.slow]") {
    // The worst case for the alignment: no common lines at all. The ceiling is
    // what stops this from running away, and it must announce itself.
    std::string left;
    std::string right;
    for (int i = 0; i < 200000; ++i) {
        left += "left line " + std::to_string(i) + "\n";
        right += "right line " + std::to_string(i) + "\n";
    }

    const auto a = SourceFile::fromMemory(left, "left");
    const auto b = SourceFile::fromMemory(right, "right");

    const auto started = steady_clock::now();
    const auto diff = nmxd::diffText(a, b, neverStopped());
    const double millis = duration<double, std::milli>(steady_clock::now() - started).count();

    INFO("diff took " << millis << " ms with quality " << nmxd::describe(diff.quality));
    CHECK_FALSE(diff.identical());
    CHECK(millis < 4000.0);
}

namespace {

// A generated JSON asset, shaped the way exported game data usually is: one
// long array of records rather than a deep tree. Each entity becomes five
// nodes, so the node count is what the budget is really about, not the byte
// count.
std::string generateJson(std::size_t entities, std::size_t modifyEvery) {
    std::string out;
    out.reserve(entities * 160);
    out += "{\n  \"version\": 1,\n  \"entities\": [\n";
    for (std::size_t i = 0; i < entities; ++i) {
        const bool tweak = modifyEvery > 0 && (i % modifyEvery) == 0;
        if (i > 0) {
            out += ",\n";
        }
        out += "    { \"id\": \"e";
        out += std::to_string(i);
        out += "\", \"kind\": \"prop\", \"x\": ";
        out += std::to_string(i);
        out += ", \"y\": ";
        out += tweak ? "-1" : "0";
        out += ", \"tags\": [\"a\", \"b\"], \"transform\": { \"scale\": 1.0, \"rot\": 0 } }";
    }
    out += "\n  ]\n}\n";
    return out;
}

}  // namespace

TEST_CASE("a 100k node JSON pair parses and matches inside the budget", "[budget][.slow]") {
    // The same shape of measurement as the XML case, so that the two built-in
    // formats can be compared rather than each being judged against itself.
    // Each entity is two nodes: itself and its transform. Its tag list is one
    // property rather than a subtree, which is what M9 changed, so the entity
    // count had to rise for this to still measure the hundred thousand nodes it
    // is named for.
    constexpr std::size_t kEntities = 50000;
    constexpr std::size_t kModifyEvery = 500;

    const auto provider = nmxd::makeGenericJsonProvider();
    const auto left = SourceFile::fromMemory(generateJson(kEntities, 0), "left");
    const auto right = SourceFile::fromMemory(generateJson(kEntities, kModifyEvery), "right");

    const auto parseStarted = steady_clock::now();
    auto leftTree = provider->parse(left, neverStopped());
    auto rightTree = provider->parse(right, neverStopped());
    const double parseMillis =
        duration<double, std::milli>(steady_clock::now() - parseStarted).count();

    REQUIRE(leftTree.ok());
    REQUIRE(rightTree.ok());
    CHECK(leftTree.value().size() > 100000);

    const auto matchStarted = steady_clock::now();
    const auto model = nmxd::diffTrees(leftTree.value(), rightTree.value(), *provider);
    const double matchMillis =
        duration<double, std::milli>(steady_clock::now() - matchStarted).count();

    INFO("bytes " << left.size() << " a side, nodes " << leftTree.value().size()
                  << ", parse " << parseMillis << " ms for both sides, match " << matchMillis
                  << " ms");

    // Only the tweaked entities changed, and each shows up as one modified
    // node. Anything else means the match went wrong rather than slowly, which
    // a timing check alone would not catch.
    CHECK(model.quality == nmxd::MatchQuality::Full);
    CHECK(model.modified == kEntities / kModifyEvery);
    CHECK(model.added == 0);
    CHECK(model.deleted == 0);

    CHECK(parseMillis < 2000.0);
    CHECK(matchMillis < 2000.0);
}

TEST_CASE("parsing a large JSON document can be cancelled", "[budget][.slow]") {
    // Cancellation is the normal path when someone switches format or reloads,
    // so the provider has to notice a stop token part way through a document
    // rather than only between documents.
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto source = SourceFile::fromMemory(generateJson(40000, 0), "left");

    std::stop_source stop;
    stop.request_stop();

    const auto started = steady_clock::now();
    auto parsed = provider->parse(source, stop.get_token());
    const double millis = duration<double, std::milli>(steady_clock::now() - started).count();

    INFO("cancelled after " << millis << " ms");
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error() == nmxd::ParseError::Cancelled);
    CHECK(millis < 50.0);
}
