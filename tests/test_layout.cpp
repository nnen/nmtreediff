#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/diff.h"
#include "core/layout_tree.h"
#include "core/source.h"
#include "formats/xml_generic.h"

using nmxd::buildLayout;
using nmxd::findNodeAt;
using nmxd::IFormatProvider;
using nmxd::kInvalidLayout;
using nmxd::kInvalidNode;
using nmxd::LayoutNode;
using nmxd::NodeStatus;
using nmxd::Side;
using nmxd::SourceFile;
using nmxd::Tree;
using nmxd::TreeLayout;

namespace {

Tree parse(const IFormatProvider& provider, const std::string& xml) {
    const auto source = SourceFile::fromMemory(xml, "test.xml");
    auto result = provider.parse(source, {});
    REQUIRE(result.ok());
    return std::move(result).value();
}

/// Reports whether two cards overlap in both axes.
bool overlaps(const LayoutNode& a, const LayoutNode& b) {
    const bool apart = a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y ||
                       b.y + b.height <= a.y;
    return !apart;
}

}  // namespace

TEST_CASE("an unchanged pair lays out one card per node", "[layout]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = "<r><a/><b><c/></b></r>";
    const Tree left = parse(*provider, xml);
    const Tree right = parse(*provider, xml);
    const auto model = nmxd::diffTrees(left, right, *provider);

    const TreeLayout layout = buildLayout(left, right, model, *provider);

    CHECK(layout.size() == right.size());
    CHECK(layout.root != kInvalidLayout);
    CHECK(layout.width > 0.0f);
    CHECK(layout.height > 0.0f);
    for (const LayoutNode& card : layout.nodes) {
        CHECK(card.status == NodeStatus::Unchanged);
        CHECK(card.side == Side::Right);
    }
}

TEST_CASE("no two cards overlap", "[layout]") {
    // Overlapping cards would make the graph unreadable, and the packing is the
    // one part of layout that can silently get this wrong.
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><a><x/><y/></a><b/></r>");
    const Tree right = parse(
        *provider, "<r><a><x/><y/><z><deep><deeper/></deep></z></a><b/><c><d/><e/></c></r>");
    const auto model = nmxd::diffTrees(left, right, *provider);

    const TreeLayout layout = buildLayout(left, right, model, *provider);
    REQUIRE(layout.size() > 8);

    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        for (std::size_t j = i + 1; j < layout.nodes.size(); ++j) {
            INFO("cards " << i << " and " << j);
            CHECK_FALSE(overlaps(layout.nodes[i], layout.nodes[j]));
        }
    }
}

TEST_CASE("a child sits below its parent", "[layout]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = "<r><a><b/></a></r>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmxd::diffTrees(tree, tree, *provider);

    const TreeLayout layout = buildLayout(tree, tree, model, *provider);
    for (const LayoutNode& card : layout.nodes) {
        if (card.parent == kInvalidLayout) {
            continue;
        }
        const LayoutNode& parent = layout.nodes[card.parent];
        INFO("card " << card.title << " under " << parent.title);
        CHECK(card.y > parent.y);
    }
}

TEST_CASE("a lone child is centred under its parent", "[layout]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = "<root><onlychild/></root>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmxd::diffTrees(tree, tree, *provider);

    const TreeLayout layout = buildLayout(tree, tree, model, *provider);
    REQUIRE(layout.size() == 2);

    const LayoutNode& parent = layout.nodes[layout.root];
    const LayoutNode& child = layout.nodes[parent.children[0]];
    const float parentCentre = parent.x + parent.width * 0.5f;
    const float childCentre = child.x + child.width * 0.5f;
    CHECK(std::abs(parentCentre - childCentre) < 0.01f);
}

TEST_CASE("deleted nodes get a card from the left tree", "[layout]") {
    // Without this the node view would only ever show what survived, which is
    // the half of a diff a reader already has.
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><keep/><gone><under/></gone></r>");
    const Tree right = parse(*provider, "<r><keep/></r>");
    const auto model = nmxd::diffTrees(left, right, *provider);

    const TreeLayout layout = buildLayout(left, right, model, *provider);

    int deleted = 0;
    for (const LayoutNode& card : layout.nodes) {
        if (card.status == NodeStatus::Deleted) {
            CHECK(card.side == Side::Left);
            ++deleted;
        }
    }
    CHECK(deleted == 2);
    CHECK(layout.size() == right.size() + 2);
}

TEST_CASE("added nodes are marked and come from the right", "[layout]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><keep/></r>");
    const Tree right = parse(*provider, "<r><keep/><fresh/></r>");
    const auto model = nmxd::diffTrees(left, right, *provider);

    const TreeLayout layout = buildLayout(left, right, model, *provider);

    int added = 0;
    for (const LayoutNode& card : layout.nodes) {
        if (card.status == NodeStatus::Added) {
            CHECK(card.side == Side::Right);
            ++added;
        }
    }
    CHECK(added == 1);
}

TEST_CASE("a moved node points back at where it used to sit", "[layout]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><g1><n a=\"1\"/></g1><g2/></r>");
    const Tree right = parse(*provider, "<r><g1/><g2><n a=\"1\"/></g2></r>");
    const auto model = nmxd::diffTrees(left, right, *provider);

    const TreeLayout layout = buildLayout(left, right, model, *provider);

    bool foundGhost = false;
    for (const LayoutNode& card : layout.nodes) {
        if (card.status == NodeStatus::Moved && card.movedFrom != kInvalidLayout) {
            // The ghost points at the old parent, not at the new one.
            CHECK(card.movedFrom != card.parent);
            foundGhost = true;
        }
    }
    CHECK(foundGhost);
}

TEST_CASE("subtrees know whether anything below them changed", "[layout]") {
    // This is what the default collapse reads: a subtree with nothing to report
    // is worth hiding, and one with a change in it is not.
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><quiet><x/></quiet><noisy><y k=\"1\"/></noisy></r>");
    const Tree right = parse(*provider, "<r><quiet><x/></quiet><noisy><y k=\"2\"/></noisy></r>");
    const auto model = nmxd::diffTrees(left, right, *provider);

    const TreeLayout layout = buildLayout(left, right, model, *provider);

    bool checkedQuiet = false;
    bool checkedNoisy = false;
    for (const LayoutNode& card : layout.nodes) {
        if (card.title == "quiet") {
            CHECK_FALSE(card.subtreeChanged);
            checkedQuiet = true;
        }
        if (card.title == "noisy") {
            CHECK(card.subtreeChanged);
            checkedNoisy = true;
        }
    }
    CHECK(checkedQuiet);
    CHECK(checkedNoisy);
    CHECK(layout.nodes[layout.root].subtreeChanged);
}

TEST_CASE("a card counts what it would hide", "[layout]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = "<r><a><b><c/></b></a></r>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmxd::diffTrees(tree, tree, *provider);

    const TreeLayout layout = buildLayout(tree, tree, model, *provider);
    CHECK(layout.nodes[layout.root].hiddenDescendants == 3);
}

TEST_CASE("cancelling the layout reports that it stopped", "[layout]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    std::string xml = "<r>";
    for (int i = 0; i < 400; ++i) {
        xml += "<n k=\"" + std::to_string(i) + "\"/>";
    }
    xml += "</r>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmxd::diffTrees(tree, tree, *provider);

    std::stop_source source;
    source.request_stop();
    const TreeLayout layout = buildLayout(tree, tree, model, *provider, source.get_token());
    CHECK(layout.cancelled);
}

TEST_CASE("a byte offset resolves to the innermost node covering it", "[layout]") {
    // This is what turns a click in the text view into a selection in the node
    // view, so an off-by-one here selects the wrong node.
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = "<root><outer><inner/></outer></root>";
    const Tree tree = parse(*provider, xml);

    const auto offsetOf = [&xml](const std::string& needle) {
        return static_cast<std::uint32_t>(xml.find(needle));
    };

    const nmxd::NodeId root = tree.root();
    const nmxd::NodeId outer = tree.node(root).children[0];
    const nmxd::NodeId inner = tree.node(outer).children[0];

    CHECK(findNodeAt(tree, offsetOf("<root>")) == root);
    CHECK(findNodeAt(tree, offsetOf("<outer>")) == outer);
    CHECK(findNodeAt(tree, offsetOf("<inner/>")) == inner);

    // Past the end of the document there is nothing to select.
    CHECK(findNodeAt(tree, static_cast<std::uint32_t>(xml.size() + 10)) == kInvalidNode);
}

TEST_CASE("the layout can be looked up by document node", "[layout]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = "<r><a/></r>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmxd::diffTrees(tree, tree, *provider);

    const TreeLayout layout = buildLayout(tree, tree, model, *provider);
    const auto id = layout.find(Side::Right, tree.node(tree.root()).children[0]);
    REQUIRE(id != kInvalidLayout);
    CHECK(layout.nodes[id].title == "a");
    CHECK(layout.find(Side::Left, 999) == kInvalidLayout);
}
