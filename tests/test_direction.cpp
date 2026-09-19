#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/diff.h"
#include "core/layout_tree.h"
#include "core/registry.h"
#include "core/source.h"
#include "formats/xml_generic.h"

// Graph direction is a reader's choice that a format may override. These cover
// the three things that can go wrong: the layout ignoring the direction, the
// override going the wrong way round, and the two disagreeing about which axis
// grows with depth.

using nmtreediff::buildLayout;
using nmtreediff::GraphDirection;
using nmtreediff::IFormatProvider;
using nmtreediff::kInvalidLayout;
using nmtreediff::LayoutNode;
using nmtreediff::resolveDirection;
using nmtreediff::SourceFile;
using nmtreediff::Tree;
using nmtreediff::TreeLayout;

namespace {

const char* kDeepTree =
    "<root>"
    "  <branch name='a'><leaf/><leaf/></branch>"
    "  <branch name='b'><leaf/></branch>"
    "</root>";

Tree parse(const IFormatProvider& provider, const std::string& xml) {
    const auto source = SourceFile::fromMemory(xml, "test.xml");
    auto result = provider.parse(source, {});
    REQUIRE(result.ok());
    return std::move(result).value();
}

TreeLayout layOut(const IFormatProvider& provider, const Tree& tree, GraphDirection direction) {
    const auto model = nmtreediff::diffTrees(tree, tree, provider);
    return buildLayout(tree, tree, model, provider, {}, {}, direction);
}

/// A provider that answers one fixed direction and nothing else, standing in
/// for a format that knows the shape of its own trees. Only graphDirection()
/// is ever called on it, so the rest is the smallest thing that compiles.
class OpinionatedProvider : public IFormatProvider {
public:
    explicit OpinionatedProvider(GraphDirection direction) : direction_(direction) {}

    std::string_view name() const override { return "opinionated"; }
    std::string_view displayName() const override { return "Opinionated"; }
    std::span<const std::string_view> defaultExtensions() const override { return {}; }
    int score(const SourceFile&) const override { return 0; }

    nmtreediff::Result<Tree, nmtreediff::ParseError> read(const SourceFile&,
                                                          std::stop_token) const override {
        return nmtreediff::fail(nmtreediff::ParseError::Empty);
    }

    GraphDirection graphDirection() const override { return direction_; }

private:
    GraphDirection direction_;
};

}  // namespace

TEST_CASE("top down puts every child below its parent", "[direction]") {
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree tree = parse(*provider, kDeepTree);
    const auto layout = layOut(*provider, tree, GraphDirection::TopDown);

    REQUIRE(layout.size() > 3);
    CHECK(layout.direction == GraphDirection::TopDown);
    for (const LayoutNode& card : layout.nodes) {
        if (card.parent == kInvalidLayout) {
            continue;
        }
        const LayoutNode& parent = layout.nodes[card.parent];
        INFO("card " << card.title);
        CHECK(card.y >= parent.y + parent.height);
    }
}

TEST_CASE("left to right puts every child right of its parent", "[direction]") {
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree tree = parse(*provider, kDeepTree);
    const auto layout = layOut(*provider, tree, GraphDirection::LeftToRight);

    REQUIRE(layout.size() > 3);
    CHECK(layout.direction == GraphDirection::LeftToRight);
    for (const LayoutNode& card : layout.nodes) {
        if (card.parent == kInvalidLayout) {
            continue;
        }
        const LayoutNode& parent = layout.nodes[card.parent];
        INFO("card " << card.title);
        CHECK(card.x >= parent.x + parent.width);
    }
}

TEST_CASE("cards at one level line up", "[direction]") {
    // A ragged edge reads as disorder rather than as depth, and it is worst
    // left to right where card widths vary most.
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree tree = parse(*provider, kDeepTree);

    for (const GraphDirection direction :
         {GraphDirection::TopDown, GraphDirection::LeftToRight}) {
        const auto layout = layOut(*provider, tree, direction);

        // Siblings are at one level by construction, so comparing each card
        // against its first sibling is enough.
        for (const LayoutNode& parent : layout.nodes) {
            if (parent.children.size() < 2) {
                continue;
            }
            const LayoutNode& first = layout.nodes[parent.children.front()];
            for (const nmtreediff::LayoutId child : parent.children) {
                const LayoutNode& card = layout.nodes[child];
                const float a = direction == GraphDirection::LeftToRight ? card.x : card.y;
                const float b = direction == GraphDirection::LeftToRight ? first.x : first.y;
                CHECK(std::abs(a - b) < 0.01f);
            }
        }
    }
}

TEST_CASE("no two cards overlap in either direction", "[direction]") {
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree tree = parse(*provider, kDeepTree);

    for (const GraphDirection direction :
         {GraphDirection::TopDown, GraphDirection::LeftToRight}) {
        const auto layout = layOut(*provider, tree, direction);
        for (std::size_t i = 0; i < layout.size(); ++i) {
            for (std::size_t j = i + 1; j < layout.size(); ++j) {
                const LayoutNode& a = layout.nodes[i];
                const LayoutNode& b = layout.nodes[j];
                const bool apart = a.x + a.width <= b.x || b.x + b.width <= a.x ||
                                   a.y + a.height <= b.y || b.y + b.height <= a.y;
                INFO("cards " << i << " and " << j);
                CHECK(apart);
            }
        }
    }
}

TEST_CASE("a provider with no opinion takes the reader's choice", "[direction]") {
    const auto provider = nmtreediff::makeGenericXmlProvider();
    CHECK(provider->graphDirection() == GraphDirection::Inherit);
    CHECK(resolveDirection(*provider, GraphDirection::TopDown) == GraphDirection::TopDown);
    CHECK(resolveDirection(*provider, GraphDirection::LeftToRight) ==
          GraphDirection::LeftToRight);
}

TEST_CASE("a provider that names a direction overrules the default", "[direction]") {
    // Inherit has to mean "no opinion" rather than "top down", or a format that
    // simply did not answer would silently overrule a reader who did.
    const OpinionatedProvider opinionated(GraphDirection::LeftToRight);
    CHECK(resolveDirection(opinionated, GraphDirection::TopDown) == GraphDirection::LeftToRight);
    CHECK(resolveDirection(opinionated, GraphDirection::LeftToRight) ==
          GraphDirection::LeftToRight);
}

TEST_CASE("the behaviour tree asks to be read left to right", "[direction]") {
    // The sample format is the one that exercises the override, and a behaviour
    // tree is the deep narrow shape the option exists for.
    const auto registry = nmtreediff::makeDefaultRegistry();
    const IFormatProvider* provider = registry.byName("bt");
    REQUIRE(provider != nullptr);
    CHECK(provider->graphDirection() == GraphDirection::LeftToRight);

    // And the generic providers leave the choice alone.
    REQUIRE(registry.byName("xml") != nullptr);
    REQUIRE(registry.byName("json") != nullptr);
    CHECK(registry.byName("xml")->graphDirection() == GraphDirection::Inherit);
    CHECK(registry.byName("json")->graphDirection() == GraphDirection::Inherit);
}
