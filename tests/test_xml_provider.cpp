#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "core/provider.h"
#include "core/registry.h"
#include "core/source.h"
#include "formats/xml_generic.h"

using nmxd::kInvalidNode;
using nmxd::kTextProperty;
using nmxd::Node;
using nmxd::NodeId;
using nmxd::ParseError;
using nmxd::SourceFile;
using nmxd::Tree;

namespace {

const char* kBehaviorTree =
    "<behaviortree version=\"2\">\n"
    "  <node id=\"a1b2\" type=\"Sequence\" name=\"Patrol\">\n"
    "    <property name=\"interruptible\" value=\"true\"/>\n"
    "    <node id=\"c3d4\" type=\"MoveTo\">\n"
    "      <property name=\"speed\" value=\"1.0\"/>\n"
    "    </node>\n"
    "  </node>\n"
    "</behaviortree>\n";

std::stop_token neverStopped() {
    static std::stop_source source;
    return source.get_token();
}

Tree parseOrFail(const std::string& xml, const nmxd::IFormatProvider& provider) {
    const auto source = SourceFile::fromMemory(xml, "test.xml");
    auto parsed = provider.parse(source, neverStopped());
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

}  // namespace

TEST_CASE("every element becomes a node and every attribute a property", "[xml]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree tree = parseOrFail(kBehaviorTree, *provider);

    REQUIRE(tree.size() == 5);  // root, two nodes, two properties

    const Node& root = tree.node(tree.root());
    CHECK(root.kind == "behaviortree");
    CHECK(root.parent == kInvalidNode);
    CHECK(root.depth == 0);
    CHECK(root.descendantCount == 4);
    REQUIRE(root.properties.size() == 1);
    CHECK(root.properties[0].name == "version");
    CHECK(root.properties[0].value == "2");

    REQUIRE(root.children.size() == 1);
    const Node& outer = tree.node(root.children[0]);
    CHECK(outer.kind == "node");
    CHECK(outer.depth == 1);
    CHECK(outer.children.size() == 2);
    REQUIRE(outer.findProperty("id") != nullptr);
    CHECK(outer.findProperty("id")->value == "a1b2");
}

TEST_CASE("spans slice back to exactly the element that produced them", "[xml]") {
    // This is what links the two views. If a span is off by a byte, clicking a
    // node scrolls the text view to the wrong place.
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = kBehaviorTree;
    const auto source = SourceFile::fromMemory(xml, "test.xml");
    const Tree tree = parseOrFail(xml, *provider);

    for (const Node& node : tree.nodes()) {
        const auto slice = source.slice(node.span);
        INFO("node " << node.id << " kind " << node.kind << " slice [" << slice << "]");

        REQUIRE_FALSE(slice.empty());
        CHECK(slice.front() == '<');
        CHECK(slice.back() == '>');

        // The slice opens with this element's own tag.
        CHECK(slice.substr(1, node.kind.size()) == node.kind);

        // And it closes either with its own end tag or as a self-closing tag.
        const std::string closing = "</" + node.kind + ">";
        const bool selfClosed = slice.size() >= 2 && slice.substr(slice.size() - 2) == "/>";
        const bool properlyClosed =
            slice.size() >= closing.size() && slice.substr(slice.size() - closing.size()) == closing;
        CHECK((selfClosed || properlyClosed));
    }
}

TEST_CASE("a child span sits inside its parent span", "[xml]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree tree = parseOrFail(kBehaviorTree, *provider);

    for (const Node& node : tree.nodes()) {
        for (const NodeId childId : node.children) {
            const Node& child = tree.node(childId);
            INFO("child " << child.kind << " of " << node.kind);
            CHECK(child.span.begin >= node.span.begin);
            CHECK(child.span.end <= node.span.end);
        }
    }
}

TEST_CASE("an attribute span slices back to the attribute", "[xml]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = kBehaviorTree;
    const auto source = SourceFile::fromMemory(xml, "test.xml");
    const Tree tree = parseOrFail(xml, *provider);

    const Node& outer = tree.node(tree.node(tree.root()).children[0]);
    const nmxd::Property* id = outer.findProperty("id");
    REQUIRE(id != nullptr);
    CHECK(source.slice(id->span) == "id=\"a1b2\"");
}

TEST_CASE("a greater-than inside an attribute does not end the tag", "[xml]") {
    const std::string xml = "<root><item expr=\"a &gt; b\" note='x>y'/></root>";
    const auto provider = nmxd::makeGenericXmlProvider();
    const auto source = SourceFile::fromMemory(xml, "test.xml");
    const Tree tree = parseOrFail(xml, *provider);

    REQUIRE(tree.size() == 2);
    const Node& item = tree.node(tree.node(tree.root()).children[0]);
    CHECK(source.slice(item.span) == "<item expr=\"a &gt; b\" note='x>y'/>");
    CHECK(item.findProperty("note")->value == "x>y");
}

TEST_CASE("leaf text becomes a property, mixed content does not", "[xml]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree tree = parseOrFail("<root><leaf>hello</leaf><mixed>text<child/></mixed></root>",
                                  *provider);

    const Node& root = tree.node(tree.root());
    const Node& leaf = tree.node(root.children[0]);
    REQUIRE(leaf.findProperty(kTextProperty) != nullptr);
    CHECK(leaf.findProperty(kTextProperty)->value == "hello");

    const Node& mixed = tree.node(root.children[1]);
    CHECK(mixed.findProperty(kTextProperty) == nullptr);
}

TEST_CASE("malformed and empty documents are refused with a reason", "[xml]") {
    const auto provider = nmxd::makeGenericXmlProvider();

    const auto broken = SourceFile::fromMemory("<root><unclosed></root>", "bad.xml");
    const auto brokenResult = provider->parse(broken, neverStopped());
    REQUIRE_FALSE(brokenResult.ok());
    CHECK(brokenResult.error() == ParseError::NotWellFormed);

    const auto empty = SourceFile::fromMemory("", "empty.xml");
    const auto emptyResult = provider->parse(empty, neverStopped());
    REQUIRE_FALSE(emptyResult.ok());
    CHECK(emptyResult.error() == ParseError::Empty);

    // UTF-16 would make every offset wrong, so it is refused rather than
    // parsed into a tree whose spans point at nothing.
    const auto utf16 = SourceFile::fromMemory(std::string("\xFF\xFE<\0r\0o\0o\0t\0/\0>\0", 16),
                                              "utf16.xml");
    const auto utf16Result = provider->parse(utf16, neverStopped());
    REQUIRE_FALSE(utf16Result.ok());
    CHECK(utf16Result.error() == ParseError::UnsupportedEncoding);
}

TEST_CASE("identity is a hint, never an anchor, for generic XML", "[xml]") {
    // An `id` attribute in arbitrary XML might be a stable key or might be a
    // colour swatch name. A format that knows its own schema says strong.
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree tree = parseOrFail(kBehaviorTree, *provider);

    const Node& outer = tree.node(tree.node(tree.root()).children[0]);
    const auto key = provider->identity(tree, outer.id);
    CHECK_FALSE(key.strong);
    CHECK(key.value == "node#a1b2");
}

TEST_CASE("style is stable and identifies the node", "[xml]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree tree = parseOrFail(kBehaviorTree, *provider);

    const Node& outer = tree.node(tree.node(tree.root()).children[0]);
    const auto style = provider->style(tree, outer.id);
    CHECK(style.title == "node");
    CHECK(style.subtitle == "a1b2");

    // Same element name, same colour, so the two sides of a diff agree.
    const auto again = provider->style(tree, outer.id);
    CHECK(style.accent == again.accent);
    CHECK(provider->style(tree, tree.root()).accent != style.accent);
}

TEST_CASE("property order is presentation and puts identity first", "[xml]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree tree = parseOrFail("<root><n zeta=\"1\" name=\"second\" id=\"first\">body</n></root>",
                                  *provider);

    const Node& n = tree.node(tree.node(tree.root()).children[0]);
    const auto order = nmxd::propertyDisplayOrder(*provider, tree, n.id);
    REQUIRE(order.size() == 4);

    CHECK(n.properties[order[0]].name == "id");
    CHECK(n.properties[order[1]].name == "name");
    CHECK(n.properties[order[2]].name == "zeta");
    CHECK(n.properties[order[3]].name == kTextProperty);
}

TEST_CASE("the registry sniffs, honours an override and reports a bad name", "[registry]") {
    const auto registry = nmxd::makeDefaultRegistry();
    REQUIRE(registry.size() >= 1);

    const auto byExtension = SourceFile::fromMemory("<root/>", "");
    const auto sniffed = SourceFile::fromMemory("<?xml version=\"1.0\"?><root/>", "asset.bt");
    CHECK(registry.resolve(sniffed) != nullptr);
    CHECK(registry.resolve(sniffed)->name() == "xml");

    bool unknown = false;
    CHECK(registry.resolve(byExtension, "xml", &unknown) != nullptr);
    CHECK_FALSE(unknown);

    CHECK(registry.resolve(byExtension, "no-such-format", &unknown) == nullptr);
    CHECK(unknown);
}

TEST_CASE("a file with no clue still resolves to the fallback", "[registry]") {
    const auto registry = nmxd::makeDefaultRegistry();
    const auto plain = SourceFile::fromMemory("just some words", "notes.txt");
    const auto* provider = registry.resolve(plain);
    REQUIRE(provider != nullptr);
    CHECK(provider->name() == "xml");
}
