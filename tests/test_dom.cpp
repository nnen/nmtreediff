#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "core/dom.h"
#include "core/source.h"
#include "formats/bt_xml.h"
#include "formats/json_generic.h"
#include "formats/xml_generic.h"

using nmtreediff::Dom;
using nmtreediff::DomNode;
using nmtreediff::DomProperty;
using nmtreediff::PropertyForm;
using nmtreediff::SourceFile;
using nmtreediff::Tree;

namespace {

Tree parse(const nmtreediff::IFormatProvider& provider, const std::string& text, const char* name) {
    const auto source = SourceFile::fromMemory(text, name);
    auto result = provider.parse(source, {});
    REQUIRE(result.ok());
    return std::move(result).value();
}

}  // namespace

TEST_CASE("the DOM navigates the tree the base format produced", "[dom]") {
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree tree = parse(*provider, "<r a=\"1\"><x/><y k=\"v\"><z/></y><w/></r>", "t.xml");
    const Dom dom(tree);

    CHECK(dom.size() == 5);
    CHECK(dom.baseFormat() == "xml");

    const DomNode root = dom.root();
    REQUIRE(root.valid());
    CHECK(root.name() == "r");
    CHECK(root.depth() == 0);
    CHECK_FALSE(root.parent().valid());
    CHECK(root.childCount() == 3);

    const DomNode x = root.firstChild();
    const DomNode y = x.nextSibling();
    const DomNode w = root.lastChild();
    CHECK(x.name() == "x");
    CHECK(y.name() == "y");
    CHECK(w.name() == "w");
    CHECK(y.nextSibling() == w);
    CHECK(w.prevSibling() == y);
    CHECK_FALSE(w.nextSibling().valid());
    CHECK_FALSE(x.prevSibling().valid());
    CHECK(y.parent() == root);
    CHECK(y.firstChild().name() == "z");
    CHECK(y.firstChild().depth() == 2);
    CHECK_FALSE(x.firstChild().valid());
    CHECK(root.childAt(1) == y);
    CHECK_FALSE(root.childAt(3).valid());

    std::vector<std::string> names;
    for (const DomNode child : root.children()) {
        names.emplace_back(child.name());
    }
    CHECK(names == std::vector<std::string>{"x", "y", "w"});

    CHECK(dom.at(y.id()) == y);
    CHECK_FALSE(dom.at(99).valid());
    CHECK(y.span().begin < y.firstChild().span().begin);
}

TEST_CASE("the DOM exposes every property, repeats included, and the shortcut", "[dom]") {
    // The behaviour-tree format folds repeated <property> elements into
    // properties of one name, so this is where repeats come from in practice.
    const auto provider = nmtreediff::makeBehaviorTreeProvider();
    const Tree tree = parse(*provider,
                            "<behaviortree><node id=\"n1\" type=\"Wait\">"
                            "<property name=\"tag\" value=\"a\"/>"
                            "<property name=\"tag\" value=\"b\"/>"
                            "</node></behaviortree>",
                            "t.bt");
    const Dom dom(tree);
    const DomNode node = dom.root().firstChild();

    CHECK(node.propertyCount() == 4);  // id, type, tag, tag
    CHECK(node.attribute("type") == "Wait");
    CHECK_FALSE(node.attribute("missing").has_value());
    CHECK(node.property("tag").value() == "a");
    CHECK_FALSE(node.property("missing").valid());
    CHECK_FALSE(node.propertyAt(4).valid());

    std::vector<std::string> tags;
    for (const DomProperty tag : node.properties("tag")) {
        tags.emplace_back(tag.value());
    }
    CHECK(tags == std::vector<std::string>{"a", "b"});

    std::size_t all = 0;
    for (const DomProperty property : node.properties()) {
        (void)property;
        ++all;
    }
    CHECK(all == 4);
}

TEST_CASE("the DOM finds children by name, first and every one", "[dom]") {
    // A record-style document where the same element name repeats among
    // siblings, which is where a lookup by name has to say which one it means.
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree tree = parse(*provider,
                            "<node><id>n1</id><child k=\"a\"/><type>Wait</type>"
                            "<child k=\"b\"/><child k=\"c\"/></node>",
                            "t.xml");
    const Dom dom(tree);
    const DomNode node = dom.root();

    CHECK(node.child("id").text() == "n1");
    CHECK(node.child("child").attribute("k") == "a");
    CHECK_FALSE(node.child("missing").valid());
    CHECK_FALSE(DomNode{}.child("id").valid());

    std::vector<std::string> keys;
    for (const DomNode child : node.children("child")) {
        keys.emplace_back(*child.attribute("k"));
    }
    CHECK(keys == std::vector<std::string>{"a", "b", "c"});

    std::size_t none = 0;
    for (const DomNode child : node.children("missing")) {
        (void)child;
        ++none;
    }
    CHECK(none == 0);

    std::size_t all = 0;
    for (const DomNode child : node.children()) {
        (void)child;
        ++all;
    }
    CHECK(all == 5);
}

TEST_CASE("the DOM reads a property's form and parts", "[dom]") {
    const auto provider = nmtreediff::makeGenericJsonProvider();
    const Tree tree = parse(*provider, R"({"m": [[1, 2], [3]], "s": "x"})", "t.json");
    const Dom dom(tree);
    const DomNode root = dom.root();

    const DomProperty m = root.property("m");
    REQUIRE(m.valid());
    CHECK(m.form() == PropertyForm::Sequence);
    CHECK(m.partCount() == 2);
    CHECK(m.partAt(0).form() == PropertyForm::Sequence);
    CHECK(m.partAt(0).partAt(1).value() == "2");
    CHECK(m.partAt(1).partAt(0).value() == "3");
    CHECK_FALSE(m.partAt(2).valid());
    CHECK(m.value().empty());

    const DomProperty s = root.property("s");
    CHECK(s.form() == PropertyForm::Scalar);
    CHECK(s.value() == "\"x\"");
    CHECK(s.partCount() == 0);
    CHECK(s.span().end > s.span().begin);
}

TEST_CASE("text content is reachable from the element", "[dom]") {
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const Tree tree = parse(*provider, "<r><t>hello</t><e/></r>", "t.xml");
    const Dom dom(tree);
    CHECK(dom.root().firstChild().text() == "hello");
    CHECK(dom.root().lastChild().text().empty());
    CHECK(dom.root().text().empty());
}
