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
