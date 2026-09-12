#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/hash.h"
#include "core/registry.h"
#include "core/source.h"
#include "formats/xml_generic.h"

using nmxd::hashBytes;
using nmxd::Node;
using nmxd::SourceFile;
using nmxd::Tree;

namespace {

std::stop_token neverStopped() {
    static std::stop_source source;
    return source.get_token();
}

Tree parse(const nmxd::IFormatProvider& provider, const std::string& xml) {
    const auto source = SourceFile::fromMemory(xml, "test.xml");
    auto result = provider.parse(source, neverStopped());
    REQUIRE(result.ok());
    return std::move(result).value();
}

std::uint64_t rootHash(const nmxd::IFormatProvider& provider, const std::string& xml) {
    const Tree tree = parse(provider, xml);
    return tree.node(tree.root()).contentHash;
}

}  // namespace

TEST_CASE("hashing bytes is deterministic and separates content", "[hash]") {
    CHECK(hashBytes("alpha") == hashBytes("alpha"));
    CHECK(hashBytes("alpha") != hashBytes("beta"));
    CHECK(hashBytes("alpha") != hashBytes("alph"));
    CHECK(hashBytes("") == hashBytes(""));
}

TEST_CASE("the same document hashes the same every run", "[hash]") {
    // The matcher pairs subtrees by hash, so an unstable hash would change what
    // the user is shown between two runs on identical input.
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = "<root><a x=\"1\"><b/></a><c>text</c></root>";

    const std::uint64_t first = rootHash(*provider, xml);
    for (int run = 0; run < 8; ++run) {
        CHECK(rootHash(*provider, xml) == first);
    }
}

TEST_CASE("every node gets a hash", "[hash]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree tree = parse(*provider, "<root><a/><b><c/></b></root>");
    for (const Node& node : tree.nodes()) {
        INFO("node " << node.kind);
        CHECK(node.contentHash != 0);
    }
}

TEST_CASE("identical subtrees in different places hash alike", "[hash]") {
    // This is what lets the second matching pass pair an unchanged subtree
    // wherever it has moved to.
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree tree = parse(*provider, "<root><wrap><item k=\"1\"/></wrap><item k=\"1\"/></root>");

    const Node& root = tree.node(tree.root());
    const Node& nested = tree.node(tree.node(root.children[0]).children[0]);
    const Node& direct = tree.node(root.children[1]);

    CHECK(nested.kind == "item");
    CHECK(direct.kind == "item");
    CHECK(nested.contentHash == direct.contentHash);
}

TEST_CASE("attribute order does not change the hash", "[hash]") {
    // Matching treats properties as an unordered set, so a reordered attribute
    // list must not read as a change.
    const auto provider = nmxd::makeGenericXmlProvider();
    CHECK(rootHash(*provider, "<n a=\"1\" b=\"2\"/>") == rootHash(*provider, "<n b=\"2\" a=\"1\"/>"));
}

TEST_CASE("attribute values and names do change the hash", "[hash]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::uint64_t base = rootHash(*provider, "<n a=\"1\"/>");
    CHECK(rootHash(*provider, "<n a=\"2\"/>") != base);
    CHECK(rootHash(*provider, "<n z=\"1\"/>") != base);
    CHECK(rootHash(*provider, "<m a=\"1\"/>") != base);
}

TEST_CASE("child order changes the hash when the format says order matters", "[hash]") {
    // XML element order is meaningful, so these are different documents.
    const auto provider = nmxd::makeGenericXmlProvider();
    CHECK(rootHash(*provider, "<r><a/><b/></r>") != rootHash(*provider, "<r><b/><a/></r>"));
}

TEST_CASE("whitespace between elements does not change the hash", "[hash]") {
    // Formatting churn is exactly what a tree diff is supposed to see past.
    const auto provider = nmxd::makeGenericXmlProvider();
    CHECK(rootHash(*provider, "<r><a/><b/></r>") ==
          rootHash(*provider, "<r>\n    <a/>\n    <b/>\n</r>\n"));
}

TEST_CASE("a name and a value that swap places still differ", "[hash]") {
    // Folding a pair together without a separator would make these collide.
    const auto provider = nmxd::makeGenericXmlProvider();
    CHECK(rootHash(*provider, "<n ab=\"c\"/>") != rootHash(*provider, "<n a=\"bc\"/>"));
}

TEST_CASE("an empty record, an empty sequence and an empty scalar are three properties",
          "[hash][form]") {
    // A flag saying how parts compare says nothing when there are none, so
    // before properties had a form these were one hash and "tags": [] turning
    // into "tags": "" could never be reported.
    nmxd::Property scalar;
    scalar.name = "tags";
    nmxd::Property record = scalar;
    record.form = nmxd::PropertyForm::Record;
    nmxd::Property sequence = scalar;
    sequence.form = nmxd::PropertyForm::Sequence;

    CHECK(nmxd::hashProperty(scalar) != nmxd::hashProperty(record));
    CHECK(nmxd::hashProperty(scalar) != nmxd::hashProperty(sequence));
    CHECK(nmxd::hashProperty(record) != nmxd::hashProperty(sequence));
}

TEST_CASE("a record's value is part of its hash", "[hash][form]") {
    // A record may carry a value as well as parts, so changing the value with
    // the parts untouched is a change.
    nmxd::Property part;
    part.name = "min";
    part.value = "0";

    nmxd::Property before;
    before.name = "speed";
    before.value = "1.0";
    before.form = nmxd::PropertyForm::Record;
    before.children.push_back(part);

    nmxd::Property after = before;
    after.value = "2.0";

    CHECK(nmxd::hashProperty(before) != nmxd::hashProperty(after));
}

TEST_CASE("a record's parts hash as a set and a sequence's in order", "[hash][form]") {
    nmxd::Property a;
    a.name = "a";
    a.value = "1";
    nmxd::Property b;
    b.name = "b";
    b.value = "2";

    nmxd::Property record;
    record.name = "p";
    record.form = nmxd::PropertyForm::Record;
    record.children = {a, b};
    nmxd::Property recordSwapped = record;
    recordSwapped.children = {b, a};
    CHECK(nmxd::hashProperty(record) == nmxd::hashProperty(recordSwapped));

    nmxd::Property sequence = record;
    sequence.form = nmxd::PropertyForm::Sequence;
    nmxd::Property sequenceSwapped = recordSwapped;
    sequenceSwapped.form = nmxd::PropertyForm::Sequence;
    CHECK(nmxd::hashProperty(sequence) != nmxd::hashProperty(sequenceSwapped));
}

TEST_CASE("a property nested thousands of levels deep hashes without recursing", "[hash][deep]") {
    // Generated data reaches this and hand-authored data does not. The walk
    // is an explicit stack, so depth costs memory rather than the process.
    constexpr int kDepth = 20000;
    nmxd::Property deep;
    deep.name = "leaf";
    deep.value = "x";
    for (int level = 0; level < kDepth; ++level) {
        nmxd::Property wrapper;
        wrapper.name = "wrap";
        wrapper.form = nmxd::PropertyForm::Record;
        wrapper.children.push_back(std::move(deep));
        deep = std::move(wrapper);
    }

    const std::uint64_t h = nmxd::hashProperty(deep);
    CHECK(h != 0);
    CHECK(h == nmxd::hashProperty(deep));
}
