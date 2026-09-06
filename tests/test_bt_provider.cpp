#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/diff.h"
#include "core/registry.h"
#include "core/source.h"
#include "formats/bt_xml.h"
#include "formats/xml_generic.h"

namespace fs = std::filesystem;

using nmxd::Node;
using nmxd::NodeId;
using nmxd::ParseError;
using nmxd::Property;
using nmxd::SourceFile;
using nmxd::Tree;

namespace {

// The format from the requirements: a document element holding <node> elements
// with id, type and name attributes, and <property> children that describe
// them.
const char* kPatrol =
    "<behaviortree version=\"2\">\n"
    "  <node id=\"a1b2\" type=\"Sequence\" name=\"Patrol\">\n"
    "    <property name=\"interruptible\" value=\"true\"/>\n"
    "    <node id=\"c3d4\" type=\"MoveTo\" name=\"Go to waypoint\">\n"
    "      <property name=\"speed\" value=\"1.0\"/>\n"
    "    </node>\n"
    "  </node>\n"
    "</behaviortree>\n";

// The name is both the label and the path, because these cases are about
// what a provider makes of a file with that name.
SourceFile makeSource(std::string text, std::string name = "case.bt") {
    return SourceFile::fromMemory(std::move(text), name, name);
}

Tree parseOrFail(const nmxd::IFormatProvider& provider, const SourceFile& source) {
    auto result = provider.parse(source, {});
    REQUIRE(result.ok());
    return std::move(result).value();
}

std::string_view slice(const SourceFile& source, nmxd::SourceSpan span) {
    return source.text().substr(span.begin, span.end - span.begin);
}

const Node* findByKind(const Tree& tree, std::string_view kind) {
    for (const auto& node : tree.nodes()) {
        if (node.kind == kind) {
            return &node;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("only node elements become nodes", "[bt]") {
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(*provider, makeSource(kPatrol));

    // The document element, plus one node per <node>. The two <property>
    // elements did not become anything, which is the whole point: read as
    // generic XML the same file is six nodes.
    REQUIRE(tree.size() == 3);
    CHECK(tree.node(tree.root()).kind == "behaviortree");

    const Node* sequence = findByKind(tree, "Sequence");
    REQUIRE(sequence != nullptr);

    // A card reads "Sequence", not "node". The kind comes from the type
    // attribute, so the matcher will also refuse to pair a Sequence with a
    // MoveTo.
    const Node* moveTo = findByKind(tree, "MoveTo");
    REQUIRE(moveTo != nullptr);
    CHECK(moveTo->parent == sequence->id);
}

TEST_CASE("property elements fold into the node they describe", "[bt]") {
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(*provider, makeSource(kPatrol));

    const Node* sequence = findByKind(tree, "Sequence");
    REQUIRE(sequence != nullptr);

    // The node's own attributes and the folded property sit in one list, so
    // nothing above the provider learns that the format writes them two
    // different ways.
    REQUIRE(sequence->findProperty("id") != nullptr);
    CHECK(sequence->findProperty("id")->value == "a1b2");
    CHECK(sequence->findProperty("type")->value == "Sequence");
    CHECK(sequence->findProperty("name")->value == "Patrol");
    REQUIRE(sequence->findProperty("interruptible") != nullptr);
    CHECK(sequence->findProperty("interruptible")->value == "true");

    const Node* moveTo = findByKind(tree, "MoveTo");
    REQUIRE(moveTo != nullptr);
    REQUIRE(moveTo->findProperty("speed") != nullptr);
    CHECK(moveTo->findProperty("speed")->value == "1.0");
}

TEST_CASE("a property with no value attribute takes the element text", "[bt]") {
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(
        *provider,
        makeSource("<behaviortree>\n"
                   "  <node id=\"a\" type=\"Say\">\n"
                   "    <property name=\"line\">Halt, who goes there?</property>\n"
                   "  </node>\n"
                   "</behaviortree>\n"));

    const Node* say = findByKind(tree, "Say");
    REQUIRE(say != nullptr);
    REQUIRE(say->findProperty("line") != nullptr);
    CHECK(say->findProperty("line")->value == "Halt, who goes there?");
}

TEST_CASE("a property with no name is kept under the element's own name", "[bt]") {
    // It cannot be folded in as a name and value pair, because there is nothing
    // to call it. It is not dropped either: a diff tool that silently loses
    // content is the one thing a reviewer cannot forgive. So it keeps the name
    // the file gave it, which a reader can match against the file.
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(*provider,
                                  makeSource("<behaviortree>\n"
                                             "  <node id=\"a\" type=\"Wait\">\n"
                                             "    <property value=\"1\"/>\n"
                                             "  </node>\n"
                                             "</behaviortree>\n"));

    const Node* wait = findByKind(tree, "Wait");
    REQUIRE(wait != nullptr);
    CHECK(wait->findProperty("") == nullptr);

    const Property* kept = wait->findProperty("property");
    REQUIRE(kept != nullptr);
    CHECK(kept->value == "1");
    CHECK(wait->properties.size() == 3);  // id, type, and the element itself
}

TEST_CASE("a wrapper element is walked through rather than represented", "[bt]") {
    // A format that nests its nodes under <children> should still produce the
    // tree the author drew, not one flattened by an element the provider does
    // not recognise.
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(*provider,
                                  makeSource("<behaviortree>\n"
                                             "  <node id=\"a\" type=\"Sequence\">\n"
                                             "    <children>\n"
                                             "      <node id=\"b\" type=\"MoveTo\"/>\n"
                                             "    </children>\n"
                                             "  </node>\n"
                                             "</behaviortree>\n"));

    REQUIRE(tree.size() == 3);
    const Node* sequence = findByKind(tree, "Sequence");
    const Node* moveTo = findByKind(tree, "MoveTo");
    REQUIRE(sequence != nullptr);
    REQUIRE(moveTo != nullptr);
    CHECK(moveTo->parent == sequence->id);
}

TEST_CASE("the identifier is a strong key", "[bt][identity]") {
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(*provider, makeSource(kPatrol));

    const Node* moveTo = findByKind(tree, "MoveTo");
    REQUIRE(moveTo != nullptr);

    const nmxd::IdentityKey key = provider->identity(tree, moveTo->id);
    CHECK(key.strong);
    CHECK(key.value == "c3d4");

    // Generic XML looks at the same attribute and refuses to promise anything
    // about it, because there an "id" might be a colour swatch name.
    const auto generic = nmxd::makeGenericXmlProvider();
    const Tree genericTree = parseOrFail(*generic, makeSource(kPatrol));
    CHECK_FALSE(generic->identity(genericTree, genericTree.root()).strong);
}

TEST_CASE("a node with no identifier anchors nothing", "[bt][identity]") {
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(*provider,
                                  makeSource("<behaviortree>\n"
                                             "  <node type=\"Wait\"/>\n"
                                             "</behaviortree>\n"));

    const Node* wait = findByKind(tree, "Wait");
    REQUIRE(wait != nullptr);
    const nmxd::IdentityKey key = provider->identity(tree, wait->id);
    CHECK_FALSE(key.strong);
    CHECK(key.value.empty());
}

TEST_CASE("a node follows its identifier across the tree", "[bt][identity]") {
    // The milestone this provider exists to prove. The node moves to a
    // different parent and changes nothing else, and it has to be one move
    // rather than a deletion next to an addition.
    const char* before =
        "<behaviortree>\n"
        "  <node id=\"root\" type=\"Selector\">\n"
        "    <node id=\"one\" type=\"Sequence\">\n"
        "      <node id=\"guid-7\" type=\"AimAt\">\n"
        "        <property name=\"target\" value=\"nearest\"/>\n"
        "      </node>\n"
        "    </node>\n"
        "    <node id=\"two\" type=\"Sequence\"/>\n"
        "  </node>\n"
        "</behaviortree>\n";
    const char* after =
        "<behaviortree>\n"
        "  <node id=\"root\" type=\"Selector\">\n"
        "    <node id=\"one\" type=\"Sequence\"/>\n"
        "    <node id=\"two\" type=\"Sequence\">\n"
        "      <node id=\"guid-7\" type=\"AimAt\">\n"
        "        <property name=\"target\" value=\"nearest\"/>\n"
        "      </node>\n"
        "    </node>\n"
        "  </node>\n"
        "</behaviortree>\n";

    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree left = parseOrFail(*provider, makeSource(before));
    const Tree right = parseOrFail(*provider, makeSource(after));

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.moved == 1);
    CHECK(model.added == 0);
    CHECK(model.deleted == 0);
    CHECK(model.modified == 0);
}

TEST_CASE("an identified node survives changing its type", "[bt][identity]") {
    // Kind mismatch stops every structural heuristic, so without the strong key
    // this would read as one node deleted and another added. The identifier is
    // what turns it into the edit it actually is.
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree left = parseOrFail(
        *provider, makeSource("<behaviortree><node id=\"x\" type=\"Sequence\"/></behaviortree>"));
    const Tree right = parseOrFail(
        *provider, makeSource("<behaviortree><node id=\"x\" type=\"Selector\"/></behaviortree>"));

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.modified == 1);
    CHECK(model.added == 0);
    CHECK(model.deleted == 0);

    REQUIRE(model.changes.size() == 1);
    CHECK(model.changes.front().changedProperties == std::vector<std::string>{"type"});
}

TEST_CASE("spans slice back to the source", "[bt][span]") {
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const auto source = makeSource(kPatrol);
    const Tree tree = parseOrFail(*provider, source);

    // A node covers its start tag through its closing tag, so selecting it in
    // the node view highlights the whole behaviour in the text view.
    const Node* moveTo = findByKind(tree, "MoveTo");
    REQUIRE(moveTo != nullptr);
    CHECK(slice(source, moveTo->span) ==
          "<node id=\"c3d4\" type=\"MoveTo\" name=\"Go to waypoint\">\n"
          "      <property name=\"speed\" value=\"1.0\"/>\n"
          "    </node>");

    // A folded property points at the element it came from rather than at an
    // attribute, because that is what a reader would expect to see highlighted.
    const Property* speed = moveTo->findProperty("speed");
    REQUIRE(speed != nullptr);
    CHECK(slice(source, speed->span) == "<property name=\"speed\" value=\"1.0\"/>");

    // An attribute of the node itself still points at the attribute.
    const Property* identifier = moveTo->findProperty("id");
    REQUIRE(identifier != nullptr);
    CHECK(slice(source, identifier->span) == "id=\"c3d4\"");
}

TEST_CASE("the identifier sorts first among properties", "[bt]") {
    // It is what makes a node the same node across versions, so it is the first
    // thing to check when a match looks wrong.
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(*provider, makeSource(kPatrol));

    const Node* sequence = findByKind(tree, "Sequence");
    REQUIRE(sequence != nullptr);
    const auto order = nmxd::propertyDisplayOrder(*provider, tree, sequence->id);
    REQUIRE(order.size() == 4);
    CHECK(sequence->properties[order[0]].name == "id");
    CHECK(sequence->properties[order[1]].name == "type");
    CHECK(sequence->properties[order[2]].name == "name");
    CHECK(sequence->properties[order[3]].name == "interruptible");
}

TEST_CASE("a card is titled by behaviour and subtitled by name", "[bt]") {
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(*provider, makeSource(kPatrol));

    const Node* moveTo = findByKind(tree, "MoveTo");
    REQUIRE(moveTo != nullptr);
    const nmxd::NodeStyle style = provider->style(tree, moveTo->id);
    CHECK(style.title == "MoveTo");
    CHECK(style.subtitle == "Go to waypoint");

    // Two behaviours of the same type are painted alike and two of different
    // types are not, on both sides of a diff.
    const Node* sequence = findByKind(tree, "Sequence");
    REQUIRE(sequence != nullptr);
    CHECK_FALSE(provider->style(tree, sequence->id).accent == style.accent);
}

TEST_CASE("the document element decides the format", "[bt][registry]") {
    const auto provider = nmxd::makeBehaviorTreeProvider();

    // A behaviour tree saved as .xml is still a behaviour tree, and scoring on
    // the document element is what makes that work. Generic XML claims .xml at
    // 90, so anything lower here would lose.
    CHECK(provider->score(makeSource(kPatrol, "patrol.xml")) == 95);
    CHECK(provider->score(makeSource(kPatrol, "patrol.bt")) == 95);
    CHECK(provider->score(makeSource("<node id=\"a\"/>", "patrol.bt")) == 85);
    CHECK(provider->score(makeSource("<other/>", "patrol.xml")) == 0);
    CHECK(provider->score(makeSource("{\"a\": 1}", "patrol.json")) == 0);
}

TEST_CASE("the registry routes a behaviour tree to its own provider", "[bt][registry]") {
    const auto registry = nmxd::makeDefaultRegistry();

    const auto* forBt = registry.resolve(makeSource(kPatrol, "patrol.xml"));
    REQUIRE(forBt != nullptr);
    CHECK(forBt->name() == "bt");

    // And an ordinary XML file is untouched by its arrival.
    const auto* forXml = registry.resolve(makeSource("<root><a/></root>", "thing.xml"));
    REQUIRE(forXml != nullptr);
    CHECK(forXml->name() == "xml");

    // Reading the same document the generic way stays one option away, which is
    // what someone does when a provider is getting it wrong.
    const auto* forced = registry.resolve(makeSource(kPatrol, "patrol.xml"), "xml");
    REQUIRE(forced != nullptr);
    CHECK(forced->name() == "xml");
}

TEST_CASE("malformed behaviour trees are refused", "[bt]") {
    const auto provider = nmxd::makeBehaviorTreeProvider();

    const auto refuses = [&provider](std::string text) {
        auto result = provider->parse(makeSource(std::move(text)), {});
        REQUIRE_FALSE(result.ok());
        return result.error();
    };

    CHECK(refuses("<behaviortree>") == ParseError::NotWellFormed);
    CHECK(refuses("") == ParseError::Empty);
    CHECK(refuses("\xFF\xFE<\x00") == ParseError::UnsupportedEncoding);
}

TEST_CASE("reading one document two ways gives two answers", "[bt]") {
    // The corpus holds this document twice on purpose. Here it is asserted
    // rather than described: the collapsed reading is smaller, and the noise it
    // drops is the property elements.
    const auto registry = nmxd::makeDefaultRegistry();
    const auto source = makeSource(kPatrol, "patrol.bt");

    const auto* bt = registry.resolve(source);
    const auto* generic = registry.byName("xml");
    REQUIRE(bt != nullptr);
    REQUIRE(generic != nullptr);

    const Tree collapsed = parseOrFail(*bt, source);
    const Tree verbatim = parseOrFail(*generic, source);

    CHECK(collapsed.size() == 3);
    CHECK(verbatim.size() == 5);
}

TEST_CASE("an unrecognised element is kept as a property with parts", "[bt]") {
    // Nothing this format does not recognise may be dropped, and an element
    // that carries several attributes is one thing with parts rather than a
    // handful of loose names.
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(
        *provider,
        makeSource("<behaviortree>\n"
                   "  <node id=\"a\" type=\"MoveTo\">\n"
                   "    <transform x=\"1\" y=\"2\" z=\"3\"/>\n"
                   "  </node>\n"
                   "</behaviortree>\n"));

    const Node* move = findByKind(tree, "MoveTo");
    REQUIRE(move != nullptr);

    const Property* transform = move->findProperty("transform");
    REQUIRE(transform != nullptr);
    CHECK(transform->hasParts());
    CHECK_FALSE(transform->ordered);  // a record, so reordering it is not a change
    REQUIRE(transform->children.size() == 3);
    CHECK(transform->children[0].name == "x");
    CHECK(transform->children[0].value == "1");
    CHECK(transform->children[2].name == "z");
}

TEST_CASE("a wrapper keeps both itself and the nodes inside it", "[bt]") {
    // The case the rule turns on. Swallowing the nodes would be simpler and
    // would lose them; dropping the wrapper would lose what it carried.
    const auto provider = nmxd::makeBehaviorTreeProvider();
    const Tree tree = parseOrFail(
        *provider,
        makeSource("<behaviortree>\n"
                   "  <node id=\"a\" type=\"Sequence\">\n"
                   "    <children policy=\"all\">\n"
                   "      <node id=\"b\" type=\"Wait\"/>\n"
                   "    </children>\n"
                   "  </node>\n"
                   "</behaviortree>\n"));

    // The node inside surfaced where it belongs, under the sequence.
    const Node* sequence = findByKind(tree, "Sequence");
    const Node* wait = findByKind(tree, "Wait");
    REQUIRE(sequence != nullptr);
    REQUIRE(wait != nullptr);
    CHECK(wait->parent == sequence->id);

    // And the wrapper itself is still there, carrying what it said.
    const Property* wrapper = sequence->findProperty("children");
    REQUIRE(wrapper != nullptr);
    CHECK(wrapper->value == "all");
}
