#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "core/builder.h"
#include "core/hash.h"

using nmxd::BuildError;
using nmxd::Identity;
using nmxd::Node;
using nmxd::NodeId;
using nmxd::ParseError;
using nmxd::Property;
using nmxd::PropertyForm;
using nmxd::Ref;
using nmxd::RefId;
using nmxd::RefKind;
using nmxd::Tree;
using nmxd::TreeBuilder;

namespace {

Tree finishOrFail(TreeBuilder& builder) {
    auto result = builder.finish();
    REQUIRE(result.ok());
    return std::move(result).value();
}

/// The shape every construction-order test builds: a root with three
/// children, the middle one holding two of its own, the last one holding one.
/// Every node carries its name as a property so a wrong placement shows.
struct Plan {
    std::string kind;
    std::string parent;  // empty for the root
};

const std::vector<Plan> kPlan{
    {"root", ""}, {"a", "root"}, {"b", "root"}, {"c", "root"},
    {"b1", "b"},  {"b2", "b"},   {"c1", "c"},   {"b2x", "b2"},
};

/// Builds kPlan in the order given, which must list a parent before its
/// children. Sibling order is the order siblings appear in \p order.
Tree buildInOrder(const std::vector<std::size_t>& order) {
    TreeBuilder builder("test");
    std::vector<Ref> made(kPlan.size());
    for (const std::size_t i : order) {
        const Plan& plan = kPlan[i];
        if (plan.parent.empty()) {
            made[i] = builder.root(plan.kind);
        } else {
            const auto parentIndex = static_cast<std::size_t>(
                std::find_if(kPlan.begin(), kPlan.end(),
                             [&](const Plan& p) { return p.kind == plan.parent; }) -
                kPlan.begin());
            made[i] = made[parentIndex].child(plan.kind);
        }
        made[i].property("name", plan.kind);
    }
    return finishOrFail(builder);
}

std::vector<std::string> kindsInArenaOrder(const Tree& tree) {
    std::vector<std::string> kinds;
    for (const Node& node : tree.nodes()) {
        kinds.push_back(node.kind);
    }
    return kinds;
}

}  // namespace

TEST_CASE("depth-first construction yields document order", "[builder]") {
    const Tree tree = buildInOrder({0, 1, 2, 4, 5, 7, 3, 6});
    CHECK(kindsInArenaOrder(tree) ==
          std::vector<std::string>{"root", "a", "b", "b1", "b2", "b2x", "c", "c1"});

    // The arena invariants every pass relies on: a parent has a lower index
    // than its children, and descendants are contiguous.
    for (const Node& node : tree.nodes()) {
        for (const NodeId child : node.children) {
            CHECK(child > node.id);
        }
    }
    const Node& b = tree.node(2);
    CHECK(b.descendantCount == 3);
    CHECK(tree.node(3).parent == b.id);
    CHECK(tree.node(5).depth == 3);
}

TEST_CASE("build order is free: breadth-first and shuffled builds match depth-first",
          "[builder]") {
    // Sibling order is the order of child() calls on one parent, and nothing
    // else. So any order that lists a parent before its children, and keeps
    // siblings in the same relative order, builds the same tree.
    const Tree depthFirst = buildInOrder({0, 1, 2, 4, 5, 7, 3, 6});
    const Tree breadthFirst = buildInOrder({0, 1, 2, 3, 4, 5, 6, 7});
    const Tree odd = buildInOrder({0, 1, 2, 3, 6, 4, 5, 7});

    CHECK(kindsInArenaOrder(breadthFirst) == kindsInArenaOrder(depthFirst));
    CHECK(kindsInArenaOrder(odd) == kindsInArenaOrder(depthFirst));
    CHECK(breadthFirst.node(0).contentHash == depthFirst.node(0).contentHash);
    CHECK(odd.node(0).contentHash == depthFirst.node(0).contentHash);
}

TEST_CASE("properties are assembled from a flat arena in call order", "[builder]") {
    TreeBuilder builder("test");
    Ref root = builder.root("r");
    // Interleave work on two records so their parts are not contiguous in
    // the arena, which is what a queued walk does.
    Ref transform = root.record("transform");
    Ref colour = root.record("colour");
    transform.property("x", "1");
    colour.property("r", "255");
    Ref position = transform.record("position");
    colour.property("g", "0");
    position.property("z", "9");
    transform.property("y", "2");

    const Tree tree = finishOrFail(builder);
    const Node& node = tree.node(tree.root());
    REQUIRE(node.properties.size() == 2);

    const Property& t = node.properties[0];
    CHECK(t.name == "transform");
    CHECK(t.form == PropertyForm::Record);
    REQUIRE(t.children.size() == 3);
    CHECK(t.children[0].name == "x");
    CHECK(t.children[1].name == "position");
    CHECK(t.children[1].form == PropertyForm::Record);
    REQUIRE(t.children[1].children.size() == 1);
    CHECK(t.children[1].children[0].value == "9");
    CHECK(t.children[2].name == "y");

    const Property& c = node.properties[1];
    REQUIRE(c.children.size() == 2);
    CHECK(c.children[0].name == "r");
    CHECK(c.children[1].name == "g");
}

TEST_CASE("child() follows the promotion table", "[builder]") {
    TreeBuilder builder("test");
    Ref root = builder.root("r");

    SECTION("a node needs a kind") {
        CHECK_THROWS_AS(root.child(), BuildError);
        CHECK(root.child("k").isNode());
    }

    SECTION("a named part promotes a scalar to a record and keeps its value") {
        Ref speed = root.property("speed", "1.0");
        CHECK(speed.form() == PropertyForm::Scalar);
        Ref range = speed.child("range");
        CHECK(speed.form() == PropertyForm::Record);
        CHECK_FALSE(range.isNode());
        range.setValue("0..1");

        const Tree tree = finishOrFail(builder);
        const Property& p = tree.node(tree.root()).properties[0];
        CHECK(p.value == "1.0");
        CHECK(p.form == PropertyForm::Record);
        REQUIRE(p.children.size() == 1);
        CHECK(p.children[0].name == "range");
        CHECK(p.children[0].value == "0..1");
    }

    SECTION("an unnamed part promotes a scalar to a sequence") {
        Ref tags = root.property("tags", "keep");
        tags.child().setValue("a");
        tags.child().setValue("b");
        CHECK(tags.form() == PropertyForm::Sequence);

        const Tree tree = finishOrFail(builder);
        const Property& p = tree.node(tree.root()).properties[0];
        CHECK(p.value == "keep");
        CHECK(p.form == PropertyForm::Sequence);
        REQUIRE(p.children.size() == 2);
        CHECK(p.children[0].name.empty());
        CHECK(p.children[1].value == "b");
    }

    SECTION("a record refuses an unnamed part and a sequence takes a named item") {
        Ref rec = root.record("rec");
        CHECK_THROWS_AS(rec.child(), BuildError);
        Ref seq = root.sequence("seq");
        Ref named = seq.child("first");
        CHECK(named.valid());
        seq.item("second");
        CHECK_THROWS_AS(rec.item("x"), BuildError);
        CHECK_THROWS_AS(root.item("x"), BuildError);

        const Tree tree = finishOrFail(builder);
        const Property& s = tree.node(tree.root()).properties[1];
        REQUIRE(s.children.size() == 2);
        CHECK(s.children[0].name == "first");
        CHECK(s.children[1].name.empty());
        CHECK(s.children[1].value == "second");
    }
}

TEST_CASE("repeated names are two properties, in call order", "[builder]") {
    TreeBuilder builder("test");
    Ref root = builder.root("r");
    root.property("tag", "a");
    root.property("other", "x");
    root.property("tag", "b");

    const Tree tree = finishOrFail(builder);
    const Node& node = tree.node(tree.root());
    REQUIRE(node.properties.size() == 3);
    CHECK(node.properties[0].value == "a");
    CHECK(node.properties[2].name == "tag");
    CHECK(node.properties[2].value == "b");
}

TEST_CASE("what a handle records travels with the renumbering", "[builder]") {
    TreeBuilder builder("test");
    Ref root = builder.root("r");
    Ref late = root.child("late");
    Ref early = root.child("early");
    early.setIdentity("guid-1", Identity::Strong)
        .setTitle("Early", "sub")
        .setAccent(0x102030)
        .setStacked();
    late.setChildrenOrdered(false);
    CHECK_THROWS_AS(root.setValue("no"), BuildError);
    CHECK_THROWS_AS(root.property("p").setChildrenOrdered(true), BuildError);

    const Tree tree = finishOrFail(builder);
    CHECK(tree.annotated());
    const NodeId earlyId = tree.node(tree.root()).children[1];
    const NodeId lateId = tree.node(tree.root()).children[0];
    CHECK(tree.annotation(earlyId).identity == "guid-1");
    CHECK(tree.annotation(earlyId).strongIdentity);
    CHECK(tree.annotation(earlyId).title == "Early");
    CHECK(tree.annotation(earlyId).subtitle == "sub");
    CHECK(tree.annotation(earlyId).accent == 0x102030u);
    CHECK(tree.annotation(earlyId).stacked);
    CHECK_FALSE(tree.annotation(lateId).stacked);
    CHECK(tree.annotation(lateId).identity.empty());
    CHECK_FALSE(tree.node(lateId).childrenOrdered);
    CHECK(tree.node(earlyId).childrenOrdered);
}

TEST_CASE("a handle finds its parent and its owning node", "[builder]") {
    TreeBuilder builder("test");
    Ref root = builder.root("r");
    Ref child = root.child("c");
    Ref rec = child.record("rec");
    Ref part = rec.property("part", "v");

    CHECK_FALSE(root.parent().valid());
    CHECK(child.parent().id() == root.id());
    CHECK(part.parent().id() == rec.id());
    CHECK(part.owner().id() == child.id());
    CHECK(child.owner().id() == child.id());
    CHECK(builder.at(part.id()).id() == part.id());
    CHECK_FALSE(builder.at(RefId{99, RefKind::Property}).valid());
}

TEST_CASE("no root is an empty document and a signalled token is a cancellation", "[builder]") {
    {
        TreeBuilder builder("test");
        auto result = builder.finish();
        REQUIRE_FALSE(result.ok());
        CHECK(result.error() == ParseError::Empty);
    }
    {
        std::stop_source source;
        TreeBuilder builder("test", source.get_token());
        builder.root("r");
        source.request_stop();
        auto result = builder.finish();
        REQUIRE_FALSE(result.ok());
        CHECK(result.error() == ParseError::Cancelled);
    }
    {
        TreeBuilder builder("test");
        builder.root("r");
        CHECK_THROWS_AS(builder.root("again"), BuildError);
    }
}

TEST_CASE("a tree and a property nested thousands of levels deep finish without recursing",
          "[builder][deep]") {
    // Generated data reaches this depth and hand-authored data does not,
    // which is why no sample catches it. Every walk in the builder is an
    // explicit stack, so this costs memory rather than the process.
    constexpr int kDepth = 50000;
    TreeBuilder builder("test");
    Ref node = builder.root("root");
    Ref part = node.record("deep");
    for (int level = 0; level < kDepth; ++level) {
        node = node.child("n");
        part = part.child("p");
    }
    part.setValue("bottom");

    const Tree tree = finishOrFail(builder);
    CHECK(tree.size() == static_cast<std::size_t>(kDepth) + 1);
    CHECK(tree.node(static_cast<NodeId>(kDepth)).depth == static_cast<std::uint32_t>(kDepth));
    CHECK(tree.node(tree.root()).descendantCount == static_cast<std::uint32_t>(kDepth));
    CHECK(tree.node(tree.root()).contentHash != 0);
    CHECK(tree.node(tree.root()).properties[0].form == PropertyForm::Record);
}
