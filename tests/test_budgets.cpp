#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "core/source.h"
#include "core/textdiff.h"

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
