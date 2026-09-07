#include <catch2/catch_test_macros.hpp>

#include <any>
#include <string>
#include <string_view>
#include <vector>

#include "core/provider.h"
#include "core/shape.h"
#include "core/source.h"
#include "core/tree_shape.h"
#include "formats/xml_generic.h"
#include "formats/xml_shape.h"

// The shaping layer is judged by what a shaper can say and by what it cannot
// lose. A shaper decides at exit, with its children already decided, so a
// wrapper can be dropped while the nodes inside it are kept; and anything a
// shaper fails to mention still ends up in the tree.

using nmxd::Builder;
using nmxd::Element;
using nmxd::EnterControl;
using nmxd::Item;
using nmxd::Node;
using nmxd::Property;
using nmxd::SourceFile;
using nmxd::Tree;

namespace {

/// The document from the design discussion: nodes whose children sit under a
/// wrapper element that is not itself a node.
const char* kNested =
    "<node id=\"root\">\n"
    "  <children>\n"
    "    <child id=\"a\"/>\n"
    "    <child id=\"b\">\n"
    "      <children>\n"
    "        <child id=\"c\"/>\n"
    "      </children>\n"
    "    </child>\n"
    "  </children>\n"
    "</node>\n";

/// A provider whose only job is to lend its name and its child-order answer
/// to the driver; the shaper under test does the work.
class Harness final : public nmxd::IFormatProvider {
public:
    std::string_view name() const override { return "harness"; }
    std::string_view displayName() const override { return "Harness"; }
    std::span<const std::string_view> defaultExtensions() const override { return {}; }
    int score(const SourceFile&) const override { return 0; }
    nmxd::Result<Tree, nmxd::ParseError> parse(const SourceFile&, std::stop_token) const override {
        return nmxd::fail(nmxd::ParseError::NotWellFormed);
    }
    nmxd::IdentityKey identity(const Tree& tree, nmxd::NodeId id) const override {
        const auto& annotation = tree.annotation(id);
        return nmxd::IdentityKey{annotation.strongIdentity, annotation.identity};
    }
    nmxd::NodeStyle style(const Tree&, nmxd::NodeId) const override { return {}; }
    bool childrenOrdered(const Tree& tree, nmxd::NodeId id) const override {
        return !tree.annotation(id).childrenUnordered;
    }
};

Tree shapeOrFail(std::string_view xml, nmxd::IShaper& shaper) {
    const Harness harness;
    const auto source = SourceFile::fromMemory(std::string(xml), "test.xml");
    auto result = nmxd::shapeXmlDocument(source, shaper, harness, {});
    REQUIRE(result.ok());
    return std::move(result).value();
}

const Node* findByKind(const Tree& tree, std::string_view kind) {
    for (const auto& node : tree.nodes()) {
        if (node.kind == kind) {
            return &node;
        }
    }
    return nullptr;
}

std::string_view slice(const SourceFile& source, nmxd::SourceSpan span) {
    return source.text().substr(span.begin, span.end - span.begin);
}

/// The shaper from the design discussion, exit only: nodes are nodes, the
/// wrapper forwards what it holds, and nothing else is mentioned.
class NestedShaper final : public nmxd::IShaper {
public:
    void exit(Element& element, Builder& out) override {
        if (element.name() == "node" || element.name() == "child") {
            out.node(std::string(element.name()), element)
                .attributes(element)
                .adopt(element.takeItems());
        } else if (element.name() == "children") {
            out.forward(element.takeItems());
        }
    }
};

}  // namespace

TEST_CASE("a wrapper can be dropped while the nodes inside it are kept", "[shape]") {
    NestedShaper shaper;
    const Tree tree = shapeOrFail(kNested, shaper);

    // Four nodes and no trace of the wrapper.
    REQUIRE(tree.size() == 4);
    const Node& root = tree.node(tree.root());
    CHECK(root.kind == "node");
    REQUIRE(root.children.size() == 2);
    CHECK(root.findProperty("children") == nullptr);

    const Node& b = tree.node(root.children[1]);
    CHECK(b.findProperty("id")->value == "b");
    REQUIRE(b.children.size() == 1);
    CHECK(tree.node(b.children[0]).findProperty("id")->value == "c");
}

TEST_CASE("spans come from the element and cover it whole", "[shape]") {
    NestedShaper shaper;
    const auto source = SourceFile::fromMemory(kNested, "test.xml");
    const Tree tree = shapeOrFail(kNested, shaper);

    const Node& root = tree.node(tree.root());
    const Node& b = tree.node(root.children[1]);
    CHECK(slice(source, b.span) ==
          "<child id=\"b\">\n"
          "      <children>\n"
          "        <child id=\"c\"/>\n"
          "      </children>\n"
          "    </child>");
    CHECK(slice(source, tree.node(root.children[0]).span) == "<child id=\"a\"/>");
    CHECK(slice(source, b.findProperty("id")->span) == "id=\"b\"");

    // The root's closing tag is found past the last child, not inside it.
    CHECK(slice(source, root.span).substr(root.span.length() - 7) == "</node>");
}

TEST_CASE("an element the shaper says nothing about gets the default treatment", "[shape]") {
    // The shaper above never mentions <extra>. It must not vanish.
    NestedShaper shaper;
    const Tree tree = shapeOrFail("<node><extra note=\"kept\">text</extra></node>", shaper);

    REQUIRE(tree.size() == 2);
    const Node& extra = tree.node(tree.node(tree.root()).children[0]);
    CHECK(extra.kind == "extra");
    CHECK(extra.findProperty("note")->value == "kept");
    CHECK(extra.findProperty(nmxd::kTextProperty)->value == "text");
}

TEST_CASE("items a shaper leaves behind go up rather than away", "[shape]") {
    // A shaper that emits a property for the wrapper and forgets the items
    // inside it. They still reach the node above.
    class Forgetful final : public nmxd::IShaper {
    public:
        void exit(Element& element, Builder& out) override {
            if (element.name() == "children") {
                out.property("wrapper", "seen", element);
            } else {
                out.node(std::string(element.name()), element).adopt(element.takeItems());
            }
        }
    } shaper;

    const Tree tree = shapeOrFail(kNested, shaper);
    const Node& root = tree.node(tree.root());
    CHECK(root.children.size() == 2);
    REQUIRE(root.findProperty("wrapper") != nullptr);
}

TEST_CASE("drop is the one way to lose content", "[shape]") {
    class Dropping final : public nmxd::IShaper {
    public:
        void exit(Element& element, Builder& out) override {
            if (element.name() == "children") {
                out.drop();
            } else {
                out.node(std::string(element.name()), element).adopt(element.takeItems());
            }
        }
    } shaper;

    const Tree tree = shapeOrFail(kNested, shaper);
    CHECK(tree.size() == 1);
    CHECK(tree.node(tree.root()).properties.empty());
}

TEST_CASE("enter leaves a value that descendants read", "[shape]") {
    // Children keyed by the id of the node above them, which enter stores
    // on the node's frame and the child's exit reads back.
    class Keyed final : public nmxd::IShaper {
    public:
        void enter(Element& element, EnterControl& control) override {
            if (element.name() == "node" || element.name() == "child") {
                control.element().data() = std::string(element.attributeValue("id"));
            }
        }
        void exit(Element& element, Builder& out) override {
            if (element.name() == "children") {
                out.forward(element.takeItems());
                return;
            }
            std::string key(element.attributeValue("id"));
            for (const Element* above = element.parent(); above != nullptr;
                 above = above->parent()) {
                if (const auto* owner = std::any_cast<std::string>(&above->data())) {
                    key = *owner + "/" + key;
                    break;
                }
            }
            out.node(std::string(element.name()), element)
                .identity(key, true)
                .adopt(element.takeItems());
        }
    } shaper;

    const Harness harness;
    const Tree tree = shapeOrFail(kNested, shaper);
    const Node& root = tree.node(tree.root());
    const Node& b = tree.node(root.children[1]);
    CHECK(harness.identity(tree, b.id).value == "root/b");
    CHECK(harness.identity(tree, b.children[0]).value == "b/c");
    CHECK(harness.identity(tree, b.children[0]).strong);
}

TEST_CASE("enter can hand a subtree to the default treatment", "[shape]") {
    // The shaper wants nothing to do with <editor>, and must not be asked
    // about anything inside it, and must not lose it either.
    class Steering final : public nmxd::IShaper {
    public:
        int asked = 0;
        void enter(Element& element, EnterControl& control) override {
            if (element.name() == "editor") {
                control.useDefault();
            }
        }
        void exit(Element& element, Builder& out) override {
            ++asked;
            out.node(std::string(element.name()), element).adopt(element.takeItems());
        }
    } shaper;

    const Tree tree = shapeOrFail(
        "<node><editor><layout x=\"1\"><pane/></layout></editor><child/></node>", shaper);

    CHECK(shaper.asked == 2);  // node and child only
    REQUIRE(tree.size() == 5);
    const Node* layout = findByKind(tree, "layout");
    REQUIRE(layout != nullptr);
    CHECK(layout->findProperty("x")->value == "1");
}

TEST_CASE("enter can keep a subtree opaque", "[shape]") {
    class Opaque final : public nmxd::IShaper {
    public:
        void enter(Element& element, EnterControl& control) override {
            if (element.name() == "editor") {
                control.opaque();
            }
        }
        void exit(Element& element, Builder& out) override {
            out.node(std::string(element.name()), element).adopt(element.takeItems());
        }
    } shaper;

    const char* xml = "<node><editor><layout x=\"1\"><pane/></layout></editor><child/></node>";
    const auto source = SourceFile::fromMemory(xml, "test.xml");
    const Tree tree = shapeOrFail(xml, shaper);

    REQUIRE(tree.size() == 2);
    const Node& root = tree.node(tree.root());
    const Property* editor = root.findProperty("editor");
    REQUIRE(editor != nullptr);
    CHECK(editor->value == "<editor><layout x=\"1\"><pane/></layout></editor>");
    CHECK(slice(source, editor->span) == editor->value);
    CHECK(slice(source, root.span) == xml);
}

TEST_CASE("a node can say its children are unordered", "[shape]") {
    class Unordered final : public nmxd::IShaper {
    public:
        void exit(Element& element, Builder& out) override {
            out.node(std::string(element.name()), element)
                .orderedChildren(element.name() != "set")
                .adopt(element.takeItems());
        }
    } shaper;

    const Harness harness;
    const Tree tree = shapeOrFail("<set><a/><b/></set>", shaper);
    CHECK_FALSE(harness.childrenOrdered(tree, tree.root()));
    CHECK(harness.childrenOrdered(tree, tree.node(tree.root()).children[0]));
}

TEST_CASE("a root that becomes several things still hangs from one node", "[shape]") {
    // The document element forwards everything, so nothing stands for it.
    // The tree still needs a root, and it is named after the document.
    class Flattening final : public nmxd::IShaper {
    public:
        void exit(Element& element, Builder& out) override {
            if (element.parent() == nullptr) {
                out.property("version", std::string(element.attributeValue("version")), element);
                out.forward(element.takeItems());
            } else {
                out.node(std::string(element.name()), element).adopt(element.takeItems());
            }
        }
    } shaper;

    const Tree tree = shapeOrFail("<doc version=\"3\"><a/><b/></doc>", shaper);
    const Node& root = tree.node(tree.root());
    CHECK(root.kind == "doc");
    CHECK(root.children.size() == 2);
    CHECK(root.findProperty("version")->value == "3");
}

TEST_CASE("the default treatment reads exactly like generic XML", "[shape]") {
    // What generic XML is now. Held against the provider so that the two can
    // never drift apart.
    nmxd::DefaultShaper shaper;
    const char* xml = "<r a=\"1\"><x>t</x><y b='2'><z/></y></r>";
    const Tree shaped = shapeOrFail(xml, shaper);

    const auto provider = nmxd::makeGenericXmlProvider();
    auto generic = provider->parse(SourceFile::fromMemory(xml, "test.xml"), {});
    REQUIRE(generic.ok());

    REQUIRE(shaped.size() == generic.value().size());
    for (nmxd::NodeId id = 0; id < shaped.size(); ++id) {
        CHECK(shaped.node(id).kind == generic.value().node(id).kind);
        CHECK(shaped.node(id).span == generic.value().node(id).span);
        CHECK(shaped.node(id).properties.size() == generic.value().node(id).properties.size());
    }
}

TEST_CASE("a tree another provider built can be shaped the same way", "[shape]") {
    // The path a JSON-based script takes: the base reads the file and the
    // shaper reads the base's tree. The wrapper rule works there too.
    const auto provider = nmxd::makeGenericXmlProvider();
    const auto source = SourceFile::fromMemory(kNested, "test.xml");
    auto generic = provider->parse(source, {});
    REQUIRE(generic.ok());

    NestedShaper shaper;
    const Harness harness;
    auto reshaped = nmxd::shapeTree(generic.value(), source, shaper, harness, {});
    REQUIRE(reshaped.ok());

    const Tree& tree = reshaped.value();
    REQUIRE(tree.size() == 4);
    CHECK(tree.formatName() == "harness");
    CHECK(tree.node(tree.root()).children.size() == 2);
    CHECK(slice(source, tree.node(tree.node(tree.root()).children[0]).span) ==
          "<child id=\"a\"/>");
}

TEST_CASE("a deep document does not overflow the walk", "[shape]") {
    std::string xml;
    constexpr int kDepth = 20000;
    for (int i = 0; i < kDepth; ++i) {
        xml += "<d>";
    }
    for (int i = 0; i < kDepth; ++i) {
        xml += "</d>";
    }
    nmxd::DefaultShaper shaper;
    const Tree tree = shapeOrFail(xml, shaper);
    CHECK(tree.size() == kDepth);
}
