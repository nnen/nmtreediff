#include <catch2/catch_test_macros.hpp>

#include "core/tree.h"

using nmxd::kInvalidNode;
using nmxd::NodeId;
using nmxd::SourceSpan;
using nmxd::Tree;

TEST_CASE("an empty tree has no root", "[tree]") {
    Tree tree;
    CHECK(tree.empty());
    CHECK(tree.size() == 0);
    CHECK(tree.root() == kInvalidNode);
    tree.finalize();  // must not crash on nothing
    CHECK(tree.empty());
}

TEST_CASE("the first node added becomes the root", "[tree]") {
    Tree tree;
    const NodeId root = tree.add(kInvalidNode, "root", SourceSpan{0, 10});
    CHECK(tree.root() == root);
    CHECK(tree.node(root).parent == kInvalidNode);
    CHECK(tree.node(root).id == root);
}

TEST_CASE("children are recorded on their parent in insertion order", "[tree]") {
    Tree tree;
    const NodeId root = tree.add(kInvalidNode, "root", {});
    const NodeId a = tree.add(root, "a", {});
    const NodeId b = tree.add(root, "b", {});

    REQUIRE(tree.node(root).children.size() == 2);
    CHECK(tree.node(root).children[0] == a);
    CHECK(tree.node(root).children[1] == b);
    CHECK(tree.node(a).parent == root);
}

TEST_CASE("a parent always has a lower index than its children", "[tree]") {
    // Several passes walk the arena backwards to visit children before parents,
    // which is only sound while this holds.
    Tree tree;
    const NodeId root = tree.add(kInvalidNode, "root", {});
    const NodeId a = tree.add(root, "a", {});
    const NodeId aa = tree.add(a, "aa", {});
    const NodeId b = tree.add(root, "b", {});

    for (const auto& node : tree.nodes()) {
        if (node.parent != kInvalidNode) {
            CHECK(node.parent < node.id);
        }
    }
    CHECK(root < a);
    CHECK(a < aa);
    CHECK(aa < b);
}

TEST_CASE("finalize fills in depth and descendant counts", "[tree]") {
    Tree tree;
    const NodeId root = tree.add(kInvalidNode, "root", {});
    const NodeId a = tree.add(root, "a", {});
    const NodeId aa = tree.add(a, "aa", {});
    const NodeId ab = tree.add(a, "ab", {});
    const NodeId b = tree.add(root, "b", {});
    tree.finalize();

    CHECK(tree.node(root).depth == 0);
    CHECK(tree.node(a).depth == 1);
    CHECK(tree.node(aa).depth == 2);

    CHECK(tree.node(root).descendantCount == 4);
    CHECK(tree.node(a).descendantCount == 2);
    CHECK(tree.node(aa).descendantCount == 0);
    CHECK(tree.node(ab).descendantCount == 0);
    CHECK(tree.node(b).descendantCount == 0);
}

TEST_CASE("finalize is repeatable", "[tree]") {
    Tree tree;
    const NodeId root = tree.add(kInvalidNode, "root", {});
    tree.add(root, "a", {});
    tree.finalize();
    const auto first = tree.node(root).descendantCount;
    tree.finalize();
    CHECK(tree.node(root).descendantCount == first);
}

TEST_CASE("properties are found by name and keep insertion order", "[tree]") {
    Tree tree;
    const NodeId root = tree.add(kInvalidNode, "root", {});
    tree.addProperty(root, "b", "2", SourceSpan{4, 8});
    tree.addProperty(root, "a", "1", SourceSpan{9, 13});

    REQUIRE(tree.node(root).properties.size() == 2);
    CHECK(tree.node(root).properties[0].name == "b");

    const auto* found = tree.node(root).findProperty("a");
    REQUIRE(found != nullptr);
    CHECK(found->value == "1");
    CHECK(found->span == SourceSpan{9, 13});
    CHECK(tree.node(root).findProperty("missing") == nullptr);
}

TEST_CASE("a leaf reports itself as one", "[tree]") {
    Tree tree;
    const NodeId root = tree.add(kInvalidNode, "root", {});
    const NodeId child = tree.add(root, "child", {});
    CHECK_FALSE(tree.node(root).isLeaf());
    CHECK(tree.node(child).isLeaf());
}
