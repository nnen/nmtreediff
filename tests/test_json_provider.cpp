#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/diff.h"
#include "core/hash.h"
#include "core/registry.h"
#include "core/source.h"
#include "formats/json_generic.h"

namespace fs = std::filesystem;

using nmxd::Property;
using nmxd::kValueProperty;
using nmxd::Node;
using nmxd::NodeId;
using nmxd::ParseError;
using nmxd::SourceFile;
using nmxd::Tree;

namespace {

// The provider reads a SourceFile rather than a string, so every case needs one
// built in memory. The name matters only for the extension, which is what
// sniffing looks at first.
SourceFile makeSource(std::string text, std::string name = "case.json") {
    return SourceFile::fromMemory(std::move(text), name, name);
}

Tree parseOrFail(const nmxd::IFormatProvider& provider, const SourceFile& source) {
    auto result = provider.parse(source, {});
    REQUIRE(result.ok());
    return std::move(result).value();
}

// A span is only useful if it points at the thing it claims to. Slicing the
// source with it is the only check that actually proves that.
std::string_view slice(const SourceFile& source, nmxd::SourceSpan span) {
    return source.text().substr(span.begin, span.end - span.begin);
}

const Node* childByKind(const Tree& tree, NodeId parent, std::string_view kind) {
    for (const NodeId child : tree.node(parent).children) {
        if (tree.node(child).kind == kind) {
            return &tree.node(child);
        }
    }
    return nullptr;
}

}  // namespace


TEST_CASE("the JSON provider claims JSON files", "[json]") {
    const auto provider = nmxd::makeGenericJsonProvider();

    CHECK(provider->name() == "json");

    // An extension only exists on a file that came from disk, so this one case
    // reads a real one rather than building bytes in memory.
    const auto onDisk =
        SourceFile::load(fs::path(NMXD_TESTDATA_DIR) / "sample" / "level_before.json");
    REQUIRE(onDisk.ok());
    CHECK(provider->claimsExtension(onDisk.value()));
    CHECK(provider->score(onDisk.value()) == 90);

    const auto extensions = provider->defaultExtensions();
    CHECK(std::find(extensions.begin(), extensions.end(), ".geojson") != extensions.end());

    // An unfamiliar extension still reaches the sniffing path, because a studio
    // names its asset files whatever it likes.
    CHECK(provider->score(makeSource("  \n {\"a\": 1}", "level.asset")) == 60);
    CHECK(provider->score(makeSource("[1, 2]", "level.asset")) == 60);
    CHECK(provider->score(makeSource("\xEF\xBB\xBF{}", "level.asset")) == 60);

    // Nothing that is not JSON, and in particular nothing that is XML.
    CHECK(provider->score(makeSource("<root/>", "level.asset")) == 0);
    CHECK(provider->score(makeSource("hello", "level.asset")) == 0);
}

TEST_CASE("the registry tells the two built-in formats apart", "[json][registry]") {
    const auto registry = nmxd::makeDefaultRegistry();

    // By extension, which is the path a person actually takes.
    const auto json = SourceFile::load(fs::path(NMXD_TESTDATA_DIR) / "sample" / "level_before.json");
    // An ordinary XML file, not the behaviour tree in testdata/sample, which
    // the behavior-tree provider claims on sight.
    const auto xml =
        SourceFile::load(fs::path(NMXD_TESTDATA_DIR) / "golden" / "node_inserted" / "left.xml");
    REQUIRE(json.ok());
    REQUIRE(xml.ok());

    REQUIRE(registry.resolve(json.value()) != nullptr);
    CHECK(registry.resolve(json.value())->name() == "json");
    REQUIRE(registry.resolve(xml.value()) != nullptr);
    CHECK(registry.resolve(xml.value())->name() == "xml");

    // Sniffed rather than claimed, which is the case that would go wrong if one
    // provider were greedy.
    const auto* sniffedJson = registry.resolve(makeSource("[1, 2, 3]", "level.data"));
    REQUIRE(sniffedJson != nullptr);
    CHECK(sniffedJson->name() == "json");

    const auto* sniffedXml = registry.resolve(makeSource("<?xml version=\"1.0\"?><r/>", "l.data"));
    REQUIRE(sniffedXml != nullptr);
    CHECK(sniffedXml->name() == "xml");

    // An explicit format always wins over anything the file looks like.
    const auto* forced = registry.resolve(json.value(), "xml");
    REQUIRE(forced != nullptr);
    CHECK(forced->name() == "xml");
}
TEST_CASE("objects become nodes and their scalars become properties", "[json]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto source = makeSource(R"({"name": "hut", "hp": 40, "nested": {"a": true}})");
    const Tree tree = parseOrFail(*provider, source);

    REQUIRE(tree.size() == 2);
    const Node& root = tree.node(tree.root());
    CHECK(root.kind == "$");

    // Two scalars became properties, one object became a child node, and the
    // type marker rides along with them.
    REQUIRE(root.findProperty("name") != nullptr);
    CHECK(root.findProperty("name")->value == "\"hut\"");
    REQUIRE(root.findProperty("hp") != nullptr);
    CHECK(root.findProperty("hp")->value == "40");
    CHECK(root.findProperty("nested") == nullptr);

    REQUIRE(root.children.size() == 1);
    const Node& nested = tree.node(root.children.front());
    CHECK(nested.kind == "nested");
    REQUIRE(nested.findProperty("a") != nullptr);
    CHECK(nested.findProperty("a")->value == "true");
}

TEST_CASE("scalar values are kept exactly as written", "[json]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto source = makeSource(R"({"a": 1.0, "b": 1, "c": "1", "d": "\u00e9", "e": null})");
    const Tree tree = parseOrFail(*provider, source);
    const Node& root = tree.node(tree.root());

    // A diff tool that normalised these would be deciding for the reader that a
    // change is not worth seeing. Quoting distinguishes the string from the
    // number, and the escape is left spelled the way the author spelled it.
    CHECK(root.findProperty("a")->value == "1.0");
    CHECK(root.findProperty("b")->value == "1");
    CHECK(root.findProperty("c")->value == "\"1\"");
    CHECK(root.findProperty("d")->value == "\"\\u00e9\"");
    CHECK(root.findProperty("e")->value == "null");
}

TEST_CASE("an array of scalars becomes one property", "[json]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto source = makeSource(R"({"tags": ["a", "b"], "rooms": [{"n": 1}]})");
    const Tree tree = parseOrFail(*provider, source);

    // A list of scalars is one thing with parts, so it reads as one line rather
    // than as a subtree of anonymous items.
    const Node& root = tree.node(tree.root());
    const Property* tags = root.findProperty("tags");
    REQUIRE(tags != nullptr);
    CHECK(tags->ordered);
    REQUIRE(tags->children.size() == 2);
    CHECK(tags->children[0].value == "\"a\"");
    CHECK(tags->children[1].value == "\"b\"");
    // Parts of a sequence have no names, because a position in a list is not a
    // name.
    CHECK(tags->children[0].name.empty());

    // An array holding an object stays a node, because an object has named
    // fields a reader will want matched against their counterparts.
    CHECK(childByKind(tree, tree.root(), "tags") == nullptr);
    const Node* rooms = childByKind(tree, tree.root(), "rooms");
    REQUIRE(rooms != nullptr);
    REQUIRE(rooms->children.size() == 1);
    const Node& room = tree.node(rooms->children.front());
    CHECK(room.kind == "item");
    CHECK(room.findProperty("n") != nullptr);
}

TEST_CASE("an array of arrays of scalars is one property with parts", "[json]") {
    // A four by four transform matrix is this shape, and it is the case nested
    // properties were asked for: one property, not sixteen anonymous nodes four
    // levels deep.
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto source = makeSource(R"({"matrix": [[1, 0], [0, 1]]})");
    const Tree tree = parseOrFail(*provider, source);

    CHECK(tree.size() == 1);
    const Property* matrix = tree.node(tree.root()).findProperty("matrix");
    REQUIRE(matrix != nullptr);
    CHECK(matrix->ordered);
    REQUIRE(matrix->children.size() == 2);

    const Property& row = matrix->children.front();
    CHECK(row.ordered);
    REQUIRE(row.children.size() == 2);
    CHECK(row.children[0].value == "1");
    CHECK(row.children[1].value == "0");
}

TEST_CASE("node spans slice back to the source", "[json][span]") {
    const std::string text = R"({
  "settings": { "fog": true },
  "tags": [ "a", "bb" ]
})";
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto source = makeSource(text);
    const Tree tree = parseOrFail(*provider, source);

    // The root covers the whole document, with no trailing newline caught up in
    // it.
    CHECK(slice(source, tree.node(tree.root()).span) == text);

    // A member's span starts at its key, so highlighting it in the text view
    // shows what the node is called and not only what it holds.
    const Node* settings = childByKind(tree, tree.root(), "settings");
    REQUIRE(settings != nullptr);
    CHECK(slice(source, settings->span) == R"("settings": { "fog": true })");

    // An array of scalars is a property, and its span covers the key and the
    // whole list, which is what a reader expects highlighted when the list
    // changes.
    const Property* tags = tree.node(tree.root()).findProperty("tags");
    REQUIRE(tags != nullptr);
    CHECK(slice(source, tags->span) == R"("tags": [ "a", "bb" ])");

    // Each part keeps a span of its own, so a changed element can still be
    // pointed at.
    REQUIRE(tags->children.size() == 2);
    CHECK(slice(source, tags->children[0].span) == "\"a\"");
    CHECK(slice(source, tags->children[1].span) == "\"bb\"");

    // A property span covers the key, the colon and the value, which is what a
    // reader would expect to see highlighted for a changed member.
    const Property* fog = settings->findProperty("fog");
    REQUIRE(fog != nullptr);
    CHECK(slice(source, fog->span) == R"("fog": true)");
}

TEST_CASE("spans survive a byte order mark", "[json][span]") {
    // Windows tools write one, it is not part of the document, and a span that
    // ignored it would point three bytes short of the truth.
    const std::string text = "\xEF\xBB\xBF{\"a\": 1}";
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto source = makeSource(text);
    const Tree tree = parseOrFail(*provider, source);

    CHECK(slice(source, tree.node(tree.root()).span) == "{\"a\": 1}");
    CHECK(slice(source, tree.node(tree.root()).findProperty("a")->span) == "\"a\": 1");
}

TEST_CASE("object members are unordered and array elements are not", "[json][order]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto source = makeSource(R"({"list": [1, 2], "rooms": [{"n": 1}], "map": {"x": 1}})");
    const Tree tree = parseOrFail(*provider, source);

    const Node* rooms = childByKind(tree, tree.root(), "rooms");
    const Node* map = childByKind(tree, tree.root(), "map");
    REQUIRE(rooms != nullptr);
    REQUIRE(map != nullptr);

    // The hook that generic XML never exercises, and the reason JSON ships
    // before the provider interface is published.
    CHECK(provider->childrenOrdered(tree, rooms->id));
    CHECK_FALSE(provider->childrenOrdered(tree, map->id));
    CHECK_FALSE(provider->childrenOrdered(tree, tree.root()));

    // The same distinction one level down: a list of scalars is a sequence, so
    // its parts are positional, while a node's properties never are.
    const Property* list = tree.node(tree.root()).findProperty("list");
    REQUIRE(list != nullptr);
    CHECK(list->ordered);
}

TEST_CASE("reordering a list is a change and reordering a record is not",
          "[json][order]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const Tree listBefore = parseOrFail(*provider, makeSource(R"({"tags": ["a", "b"]})"));
    const Tree listAfter = parseOrFail(*provider, makeSource(R"({"tags": ["b", "a"]})"));
    CHECK_FALSE(nmxd::diffTrees(listBefore, listAfter, *provider).identical());

    const Tree recordBefore = parseOrFail(*provider, makeSource(R"({"m": {"x": 1, "y": 2}})"));
    const Tree recordAfter = parseOrFail(*provider, makeSource(R"({"m": {"y": 2, "x": 1}})"));
    CHECK(nmxd::diffTrees(recordBefore, recordAfter, *provider).identical());
}

TEST_CASE("reordering object members is not a change", "[json][order]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto before = makeSource(R"({"a": {"p": 1}, "b": {"q": 2}})");
    const auto after = makeSource(R"({"b": {"q": 2}, "a": {"p": 1}})");

    const Tree left = parseOrFail(*provider, before);
    const Tree right = parseOrFail(*provider, after);

    // The root hashes the same both ways round, which is what makes the whole
    // document match without any comparison work at all.
    CHECK(left.node(left.root()).contentHash == right.node(right.root()).contentHash);

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.identical());
}

TEST_CASE("reordering array elements is a move", "[json][order]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto before = makeSource(R"({"list": [{"p": 1}, {"q": 2}]})");
    const auto after = makeSource(R"({"list": [{"q": 2}, {"p": 1}]})");

    const Tree left = parseOrFail(*provider, before);
    const Tree right = parseOrFail(*provider, after);

    CHECK(left.node(left.root()).contentHash != right.node(right.root()).contentHash);

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK_FALSE(model.identical());
    CHECK(model.moved > 0);
    CHECK(model.added == 0);
    CHECK(model.deleted == 0);
}

TEST_CASE("an empty object and an empty array are not the same node", "[json]") {
    // They would hash identically without the type marker, and a diff tool
    // reporting no change here would be quietly wrong.
    const auto provider = nmxd::makeGenericJsonProvider();
    const Tree left = parseOrFail(*provider, makeSource(R"({"a": {}})"));
    const Tree right = parseOrFail(*provider, makeSource(R"({"a": []})"));

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK_FALSE(model.identical());
}

TEST_CASE("a document holding one scalar is still a tree", "[json]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const Tree tree = parseOrFail(*provider, makeSource("  42  "));

    REQUIRE(tree.size() == 1);
    const Node& root = tree.node(tree.root());
    CHECK(root.kind == "$");
    REQUIRE(root.findProperty(kValueProperty) != nullptr);
    CHECK(root.findProperty(kValueProperty)->value == "42");
}

TEST_CASE("malformed JSON is refused", "[json]") {
    const auto provider = nmxd::makeGenericJsonProvider();

    const auto refuses = [&provider](std::string text) {
        auto result = provider->parse(makeSource(std::move(text)), {});
        REQUIRE_FALSE(result.ok());
        return result.error();
    };

    CHECK(refuses("{\"a\": 1") == ParseError::NotWellFormed);
    CHECK(refuses("{a: 1}") == ParseError::NotWellFormed);
    CHECK(refuses("{\"a\": 1,}") == ParseError::NotWellFormed);

    // Comments and trailing commas do turn up in game configuration and are not
    // JSON. Refusing them is a decision, not an oversight.
    CHECK(refuses("{\"a\": 1} // note") == ParseError::NotWellFormed);

    // On Demand parsing is lazy, so a value nobody reads is never checked. The
    // provider reads every scalar for exactly this case.
    CHECK(refuses("{\"a\": 1.2.3}") == ParseError::NotWellFormed);
    CHECK(refuses("{\"a\": tru}") == ParseError::NotWellFormed);

    // A second document appended to the first.
    CHECK(refuses("{\"a\": 1} {\"b\": 2}") == ParseError::NotWellFormed);

    CHECK(refuses("") == ParseError::Empty);
    CHECK(refuses("   \n  ") == ParseError::Empty);
    CHECK(refuses("\xFF\xFE{\x00\"\x00") == ParseError::UnsupportedEncoding);
}

TEST_CASE("duplicate keys are both kept", "[json]") {
    // JSON permits them and real files contain them. Dropping one would hide a
    // difference the reader came to see.
    const auto provider = nmxd::makeGenericJsonProvider();
    const Tree tree = parseOrFail(*provider, makeSource(R"({"a": 1, "a": 2})"));

    const Node& root = tree.node(tree.root());
    int count = 0;
    for (const auto& property : root.properties) {
        if (property.name == "a") {
            ++count;
        }
    }
    CHECK(count == 2);
}

TEST_CASE("a JSON tree hashes the same twice", "[json][hash]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const auto source = makeSource(R"({"a": [1, {"b": "c"}], "d": {"e": null}})");

    const Tree first = parseOrFail(*provider, source);
    const Tree second = parseOrFail(*provider, source);

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        const auto id = static_cast<NodeId>(i);
        CHECK(first.node(id).contentHash == second.node(id).contentHash);
    }
}

TEST_CASE("the JSON provider styles nodes distinguishably", "[json]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const Tree tree = parseOrFail(*provider, makeSource(R"({"enemies": [{"id": "orc"}]})"));

    const Node* enemies = childByKind(tree, tree.root(), "enemies");
    REQUIRE(enemies != nullptr);
    const nmxd::NodeStyle arrayStyle = provider->style(tree, enemies->id);
    CHECK(arrayStyle.title == "enemies");
    CHECK(arrayStyle.subtitle == "array");

    // An element card shows the identifying member rather than the word "item",
    // which is all its kind could offer.
    const Node& element = tree.node(enemies->children.front());
    const nmxd::NodeStyle elementStyle = provider->style(tree, element.id);
    CHECK(elementStyle.title == "item");
    CHECK(elementStyle.subtitle == "\"orc\"");

    // Elements borrow their parent's key for colour, so two arrays in one file
    // are not painted the same.
    CHECK(elementStyle.accent == arrayStyle.accent);
}

TEST_CASE("the synthetic properties sort last", "[json]") {
    const auto provider = nmxd::makeGenericJsonProvider();
    const Tree tree = parseOrFail(*provider, makeSource(R"({"hp": 1, "name": "hut", "id": "a"})"));

    const auto order = nmxd::propertyDisplayOrder(*provider, tree, tree.root());
    REQUIRE(order.size() == 4);
    const Node& root = tree.node(tree.root());
    CHECK(root.properties[order[0]].name == "id");
    CHECK(root.properties[order[1]].name == "name");
    CHECK(root.properties[order[2]].name == "hp");
    CHECK(root.properties[order[3]].name == "#type");
}
