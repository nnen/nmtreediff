#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/diff.h"
#include "core/layout_tree.h"
#include "core/source.h"
#include "formats/bt_xml.h"
#include "formats/xml_generic.h"

using nmtreediff::buildLayout;
using nmtreediff::findNodeAt;
using nmtreediff::IFormatProvider;
using nmtreediff::kInvalidLayout;
using nmtreediff::kInvalidNode;
using nmtreediff::LayoutNode;
using nmtreediff::NodeStatus;
using nmtreediff::Side;
using nmtreediff::SourceFile;
using nmtreediff::Tree;
using nmtreediff::TreeLayout;

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
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const std::string xml = "<r><a/><b><c/></b></r>";
    const Tree left = parse(*provider, xml);
    const Tree right = parse(*provider, xml);
    const auto model = nmtreediff::diffTrees(left, right, *provider);

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
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><a><x/><y/></a><b/></r>");
    const Tree right = parse(
        *provider, "<r><a><x/><y/><z><deep><deeper/></deep></z></a><b/><c><d/><e/></c></r>");
    const auto model = nmtreediff::diffTrees(left, right, *provider);

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
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const std::string xml = "<r><a><b/></a></r>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmtreediff::diffTrees(tree, tree, *provider);

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
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const std::string xml = "<root><onlychild/></root>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmtreediff::diffTrees(tree, tree, *provider);

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
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><keep/><gone><under/></gone></r>");
    const Tree right = parse(*provider, "<r><keep/></r>");
    const auto model = nmtreediff::diffTrees(left, right, *provider);

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
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><keep/></r>");
    const Tree right = parse(*provider, "<r><keep/><fresh/></r>");
    const auto model = nmtreediff::diffTrees(left, right, *provider);

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
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><g1><n a=\"1\"/></g1><g2/></r>");
    const Tree right = parse(*provider, "<r><g1/><g2><n a=\"1\"/></g2></r>");
    const auto model = nmtreediff::diffTrees(left, right, *provider);

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
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><quiet><x/></quiet><noisy><y k=\"1\"/></noisy></r>");
    const Tree right = parse(*provider, "<r><quiet><x/></quiet><noisy><y k=\"2\"/></noisy></r>");
    const auto model = nmtreediff::diffTrees(left, right, *provider);

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
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const std::string xml = "<r><a><b><c/></b></a></r>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmtreediff::diffTrees(tree, tree, *provider);

    const TreeLayout layout = buildLayout(tree, tree, model, *provider);
    CHECK(layout.nodes[layout.root].hiddenDescendants == 3);
}

TEST_CASE("cancelling the layout reports that it stopped", "[layout]") {
    const auto provider = nmtreediff::makeGenericXmlProvider();
    std::string xml = "<r>";
    for (int i = 0; i < 400; ++i) {
        xml += "<n k=\"" + std::to_string(i) + "\"/>";
    }
    xml += "</r>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmtreediff::diffTrees(tree, tree, *provider);

    std::stop_source source;
    source.request_stop();
    const TreeLayout layout = buildLayout(tree, tree, model, *provider, source.get_token());
    CHECK(layout.cancelled);
}

TEST_CASE("a byte offset resolves to the innermost node covering it", "[layout]") {
    // This is what turns a click in the text view into a selection in the node
    // view, so an off-by-one here selects the wrong node.
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const std::string xml = "<root><outer><inner/></outer></root>";
    const Tree tree = parse(*provider, xml);

    const auto offsetOf = [&xml](const std::string& needle) {
        return static_cast<std::uint32_t>(xml.find(needle));
    };

    const nmtreediff::NodeId root = tree.root();
    const nmtreediff::NodeId outer = tree.node(root).children[0];
    const nmtreediff::NodeId inner = tree.node(outer).children[0];

    CHECK(findNodeAt(tree, offsetOf("<root>")) == root);
    CHECK(findNodeAt(tree, offsetOf("<outer>")) == outer);
    CHECK(findNodeAt(tree, offsetOf("<inner/>")) == inner);

    // Past the end of the document there is nothing to select.
    CHECK(findNodeAt(tree, static_cast<std::uint32_t>(xml.size() + 10)) == kInvalidNode);
}

TEST_CASE("the layout can be looked up by document node", "[layout]") {
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const std::string xml = "<r><a/></r>";
    const Tree tree = parse(*provider, xml);
    const auto model = nmtreediff::diffTrees(tree, tree, *provider);

    const TreeLayout layout = buildLayout(tree, tree, model, *provider);
    const auto id = layout.find(Side::Right, tree.node(tree.root()).children[0]);
    REQUIRE(id != kInvalidLayout);
    CHECK(layout.nodes[id].title == "a");
    CHECK(layout.find(Side::Left, 999) == kInvalidLayout);
}

TEST_CASE("a pair nested thousands of levels deep compares and lays out without recursing",
          "[layout][deep]") {
    // Generated data reaches this depth and hand-authored data does not, which
    // is why no sample caught it. Reading and shaping stopped recursing with
    // the DOM interface; this covers the subtree pass of the matcher, the
    // classifier's walks and the layout's walk and placement, each of which
    // used to recurse once per level and take the process down with no
    // message somewhere past five thousand.
    constexpr int kDepth = 20000;
    const auto nested = [](const std::string& innermost) {
        std::string xml;
        xml.reserve(static_cast<std::size_t>(kDepth) * 8 + innermost.size());
        for (int i = 0; i < kDepth; ++i) {
            xml += "<n>";
        }
        xml += innermost;
        for (int i = 0; i < kDepth; ++i) {
            xml += "</n>";
        }
        return xml;
    };
    const auto provider = nmtreediff::makeGenericXmlProvider();

    SECTION("an identical pair pairs every level through the subtree pass") {
        const Tree left = parse(*provider, nested("<leaf v=\"1\"/>"));
        const Tree right = parse(*provider, nested("<leaf v=\"1\"/>"));
        const auto model = nmtreediff::diffTrees(left, right, *provider);
        CHECK(model.identical());
        CHECK(model.unchanged == left.size());

        const TreeLayout layout = buildLayout(left, right, model, *provider);
        CHECK(layout.size() == right.size());
    }

    SECTION("a change at the bottom is one modification, placed") {
        const Tree left = parse(*provider, nested("<leaf v=\"1\"/>"));
        const Tree right = parse(*provider, nested("<leaf v=\"2\"/>"));
        const auto model = nmtreediff::diffTrees(left, right, *provider);
        CHECK(model.modified == 1);
        CHECK(model.added == 0);
        CHECK(model.deleted == 0);

        const TreeLayout layout = buildLayout(left, right, model, *provider);
        REQUIRE(layout.size() == right.size());
        const nmtreediff::LayoutId deepest =
            layout.find(Side::Right, static_cast<nmtreediff::NodeId>(right.size() - 1));
        REQUIRE(deepest != kInvalidLayout);
        CHECK(layout.nodes[deepest].status == NodeStatus::Modified);
        // Placement is by level, so the bottom card sits below every other.
        CHECK(layout.nodes[deepest].y > layout.nodes[layout.root].y);
    }

    SECTION("a deep subtree lost on one side and gained on the other is walked whole") {
        std::string gone = "<gone>";
        std::string come = "<come>";
        for (int i = 0; i < kDepth / 2; ++i) {
            gone += "<x>";
            come += "<y>";
        }
        for (int i = 0; i < kDepth / 2; ++i) {
            gone += "</x>";
            come += "</y>";
        }
        gone += "</gone>";
        come += "</come>";

        const Tree left = parse(*provider, nested(gone));
        const Tree right = parse(*provider, nested(come));
        const auto model = nmtreediff::diffTrees(left, right, *provider);
        CHECK(model.deleted == static_cast<std::uint32_t>(kDepth / 2 + 1));
        CHECK(model.added == static_cast<std::uint32_t>(kDepth / 2 + 1));

        // The union holds every right node and every left node the right
        // side lost.
        const TreeLayout layout = buildLayout(left, right, model, *provider);
        CHECK(layout.size() == right.size() + static_cast<std::size_t>(kDepth / 2 + 1));
    }
}

namespace {

/// A behaviour tree: the compiled provider flags Inverter and Cooldown as
/// decorators, so a chain of them over a leaf is the stacking case.
const char* kDecoratorChain =
    "<behaviortree version='2'>"
    "<node id='a' type='Inverter' name='Not'>"
    "<node id='b' type='Cooldown' name='Every so often'>"
    "<node id='c' type='Wait' name='Hold'/>"
    "</node></node></behaviortree>";

/// Stacked cards share one width and touch along y, whichever way the graph
/// runs; the reader sees a block.
void checkStackedUnder(const TreeLayout& layout, nmtreediff::LayoutId upper,
                       nmtreediff::LayoutId lower) {
    const LayoutNode& top = layout.nodes[upper];
    const LayoutNode& bottom = layout.nodes[lower];
    CHECK(bottom.stackedOnParent);
    CHECK(bottom.parent == upper);
    CHECK(bottom.x == top.x);
    CHECK(bottom.width == top.width);
    CHECK(bottom.y == top.y + top.height);
}

}  // namespace

TEST_CASE("a flagged node stacks on its only child, vertically in both directions", "[layout]") {
    const auto provider = nmtreediff::makeBehaviorTreeProvider();
    const Tree left = parse(*provider, kDecoratorChain);
    const Tree right = parse(*provider, kDecoratorChain);
    const auto model = nmtreediff::diffTrees(left, right, *provider);

    const nmtreediff::NodeId inverter = right.node(right.root()).children[0];
    const nmtreediff::NodeId cooldown = right.node(inverter).children[0];
    const nmtreediff::NodeId wait = right.node(cooldown).children[0];

    for (const nmtreediff::GraphDirection direction :
         {nmtreediff::GraphDirection::TopDown, nmtreediff::GraphDirection::LeftToRight}) {
        const bool topDown = direction == nmtreediff::GraphDirection::TopDown;
        INFO("direction " << (topDown ? "top-down" : "left-to-right"));
        const TreeLayout layout = buildLayout(left, right, model, *provider, {}, {}, direction);
        REQUIRE(layout.size() == right.size());

        const nmtreediff::LayoutId head = layout.find(Side::Right, inverter);
        const nmtreediff::LayoutId middle = layout.find(Side::Right, cooldown);
        const nmtreediff::LayoutId foot = layout.find(Side::Right, wait);
        REQUIRE(head != kInvalidLayout);
        REQUIRE(middle != kInvalidLayout);
        REQUIRE(foot != kInvalidLayout);

        // The root is not a decorator, so the chain starts one level below it
        // with the usual gap, and the whole chain sits on that one level.
        const LayoutNode& root = layout.nodes[layout.root];
        const LayoutNode& top = layout.nodes[head];
        CHECK_FALSE(top.stackedOnParent);
        if (direction == nmtreediff::GraphDirection::TopDown) {
            CHECK(top.y == root.y + root.height + layout.metrics.levelGap);
        } else {
            CHECK(top.x == root.x + root.width + layout.metrics.levelGap);
        }
        checkStackedUnder(layout, head, middle);
        checkStackedUnder(layout, middle, foot);

        // Every member names the bottom, and a card in no stack names itself,
        // so a collapse asked of any member acts on the block's subtree.
        CHECK(top.stackBottom == foot);
        CHECK(layout.nodes[middle].stackBottom == foot);
        CHECK(layout.nodes[foot].stackBottom == foot);
        CHECK(root.stackBottom == layout.root);
    }
}

TEST_CASE("the layout carries the format's entry pin", "[layout]") {
    const auto provider = nmtreediff::makeBehaviorTreeProvider();
    const Tree tree = parse(*provider, kDecoratorChain);
    const auto model = nmtreediff::diffTrees(tree, tree, *provider);

    CHECK(buildLayout(tree, tree, model, *provider).entryPin == nmtreediff::StackEntryPin::Top);
    CHECK(buildLayout(tree, tree, model, *provider, {}, {}, nmtreediff::GraphDirection::LeftToRight,
                      nmtreediff::StackDirection::Vertical, nmtreediff::StackEntryPin::Bottom)
              .entryPin == nmtreediff::StackEntryPin::Bottom);
    // The sample format pins the entry to the bottom, the way its editors draw it.
    CHECK(provider->stackEntryPin() == nmtreediff::StackEntryPin::Bottom);
}

TEST_CASE("a flagged node with two children in the union draws unstacked", "[layout]") {
    // The decorator's child was replaced: a deleted child beside an added one.
    // The two of them are the change, and stacking either would hide it.
    const auto provider = nmtreediff::makeBehaviorTreeProvider();
    const Tree left = parse(*provider,
                            "<behaviortree version='2'><node id='a' type='Inverter'>"
                            "<node id='b' type='Wait'/></node></behaviortree>");
    const Tree right = parse(*provider,
                             "<behaviortree version='2'><node id='a' type='Inverter'>"
                             "<node id='c' type='MoveTo'/></node></behaviortree>");
    const auto model = nmtreediff::diffTrees(left, right, *provider);
    CHECK(model.added == 1);
    CHECK(model.deleted == 1);

    const TreeLayout layout = buildLayout(left, right, model, *provider);
    const nmtreediff::LayoutId inverter =
        layout.find(Side::Right, right.node(right.root()).children[0]);
    REQUIRE(inverter != kInvalidLayout);
    const LayoutNode& card = layout.nodes[inverter];
    CHECK(card.stackable);
    REQUIRE(card.children.size() == 2);
    for (const nmtreediff::LayoutId child : card.children) {
        CHECK_FALSE(layout.nodes[child].stackedOnParent);
        CHECK(layout.nodes[child].y == card.y + card.height + layout.metrics.levelGap);
    }
}

TEST_CASE("a stacked member keeps its own status and its siblings keep their gap", "[layout]") {
    // Two chains side by side under one parent, one of them edited inside.
    // Each member is still its own card with its own status, and the two
    // blocks are laid out as siblings with the ordinary gap between them.
    const auto provider = nmtreediff::makeBehaviorTreeProvider();
    const auto document = [](const char* seconds) {
        return std::string("<behaviortree version='2'><node id='r' type='Selector'>") +
               "<node id='a' type='Cooldown'><property name='seconds' value='" + seconds +
               "'/><node id='b' type='Wait'/></node>"
               "<node id='c' type='Inverter'><node id='d' type='Condition'/></node>"
               "</node></behaviortree>";
    };
    const Tree left = parse(*provider, document("1.0"));
    const Tree right = parse(*provider, document("2.0"));
    const auto model = nmtreediff::diffTrees(left, right, *provider);
    CHECK(model.modified == 1);

    const TreeLayout layout = buildLayout(left, right, model, *provider);
    const nmtreediff::NodeId selector = right.node(right.root()).children[0];
    const nmtreediff::NodeId cooldown = right.node(selector).children[0];
    const nmtreediff::NodeId inverter = right.node(selector).children[1];

    const LayoutNode& first = layout.nodes[layout.find(Side::Right, cooldown)];
    const LayoutNode& second = layout.nodes[layout.find(Side::Right, inverter)];
    CHECK(first.status == NodeStatus::Modified);
    CHECK(layout.nodes[first.children[0]].status == NodeStatus::Unchanged);
    CHECK(layout.nodes[first.children[0]].stackedOnParent);
    CHECK(first.y == second.y);
    CHECK(second.x >= first.x + first.width + layout.metrics.siblingGap);
}
