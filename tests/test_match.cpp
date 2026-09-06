#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

#include "core/diff.h"
#include "core/match.h"
#include "core/registry.h"
#include "core/source.h"
#include "formats/xml_generic.h"

using nmxd::describe;
using nmxd::DiffModel;
using nmxd::IFormatProvider;
using nmxd::kInvalidNode;
using nmxd::MatchOptions;
using nmxd::NodeId;
using nmxd::NodeStatus;
using nmxd::Side;
using nmxd::SourceFile;
using nmxd::Tree;

namespace {

// A provider that answers strong identity from an `id` attribute, standing in
// for the behavior-tree format the requirements describe. Generic XML cannot
// do this, because an `id` in arbitrary XML is not a promise of anything.
class IdentifiedXmlProvider final : public IFormatProvider {
public:
    explicit IdentifiedXmlProvider(std::unique_ptr<IFormatProvider> inner)
        : inner_(std::move(inner)) {}

    std::string_view name() const override { return "test-identified"; }
    std::string_view displayName() const override { return "XML with stable ids"; }
    std::span<const std::string_view> defaultExtensions() const override {
        return inner_->defaultExtensions();
    }
    int score(const SourceFile& source) const override { return inner_->score(source); }

    nmxd::Result<Tree, nmxd::ParseError> parse(const SourceFile& source,
                                               std::stop_token token) const override {
        return inner_->parse(source, token);
    }

    nmxd::IdentityKey identity(const Tree& tree, NodeId id) const override {
        if (const auto* property = tree.node(id).findProperty("id")) {
            return nmxd::IdentityKey{true, property->value};
        }
        return nmxd::IdentityKey{false, tree.node(id).kind};
    }

    nmxd::NodeStyle style(const Tree& tree, NodeId id) const override {
        return inner_->style(tree, id);
    }

private:
    std::unique_ptr<IFormatProvider> inner_;
};

Tree parse(const IFormatProvider& provider, const std::string& xml) {
    const auto source = SourceFile::fromMemory(xml, "test.xml");
    auto result = provider.parse(source, {});
    REQUIRE(result.ok());
    return std::move(result).value();
}

}  // namespace

TEST_CASE("an unchanged document matches every node", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const std::string xml = "<r><a x=\"1\"><b/></a><c/></r>";
    const Tree left = parse(*provider, xml);
    const Tree right = parse(*provider, xml);

    const auto result = nmxd::matchTrees(left, right, *provider);
    CHECK(result.matching.pairCount() == left.size());
    CHECK(result.quality == nmxd::MatchQuality::Full);

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.identical());
    CHECK(model.unchanged == left.size());
}

TEST_CASE("an identical subtree pairs even after it moves", "[match]") {
    // This is the second pass doing its job: the subtree is untouched, so it
    // costs one hash lookup rather than any similarity work.
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><g1><item k=\"v\"><leaf/></item></g1><g2/></r>");
    const Tree right = parse(*provider, "<r><g1/><g2><item k=\"v\"><leaf/></item></g2></r>");

    const auto model = nmxd::diffTrees(left, right, *provider);

    // The item and its leaf both survived, so neither is an addition.
    CHECK(model.added == 0);
    CHECK(model.deleted == 0);
    CHECK(model.moved >= 1);
}

TEST_CASE("a strong identity key follows a node across the document", "[match]") {
    // The behavior-tree case from the requirements: same identifier, different
    // parent, contents changed as well, and it is still the same node.
    const auto provider =
        std::make_unique<IdentifiedXmlProvider>(nmxd::makeGenericXmlProvider());

    const Tree left = parse(*provider,
                            "<r><g1><node id=\"guid-1\" speed=\"1.0\"/></g1><g2/></r>");
    const Tree right = parse(*provider,
                             "<r><g1/><g2><node id=\"guid-1\" speed=\"9.9\"/></g2></r>");

    const auto result = nmxd::matchTrees(left, right, *provider);
    CHECK(result.anchoredByIdentity >= 1);

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.added == 0);
    CHECK(model.deleted == 0);

    // Found, moved, and its changed property named.
    bool foundMovedAndModified = false;
    for (const auto& change : model.changes) {
        if (change.status == NodeStatus::Modified && change.moved &&
            change.changedProperties == std::vector<std::string>{"speed"}) {
            foundMovedAndModified = true;
        }
    }
    CHECK(foundMovedAndModified);
}

TEST_CASE("without a strong key an identical-content move is still caught", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><g1><n a=\"1\"/></g1><g2/></r>");
    const Tree right = parse(*provider, "<r><g1/><g2><n a=\"1\"/></g2></r>");

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.added == 0);
    CHECK(model.deleted == 0);
    CHECK(model.moved == 1);
}

TEST_CASE("a renamed container keeps its children through similarity", "[match]") {
    // Nothing hashes alike at the top, and there is no identity key. Only the
    // third pass can see that these are the same node.
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left =
        parse(*provider, "<r><box label=\"one\"><a/><b/><c/><d/></box></r>");
    const Tree right =
        parse(*provider, "<r><box label=\"two\"><a/><b/><c/><d/></box></r>");

    const auto result = nmxd::matchTrees(left, right, *provider);
    CHECK(result.anchoredBySimilarity >= 1);

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.added == 0);
    CHECK(model.deleted == 0);
    CHECK(model.modified == 1);
    CHECK(model.changes[0].changedProperties == std::vector<std::string>{"label"});
}

TEST_CASE("statuses are readable per node from either side", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><keep/><gone/></r>");
    const Tree right = parse(*provider, "<r><keep/><fresh/></r>");

    const auto model = nmxd::diffTrees(left, right, *provider);

    const NodeId leftGone = left.node(left.root()).children[1];
    const NodeId rightFresh = right.node(right.root()).children[1];
    CHECK(model.statusOf(Side::Left, leftGone) == NodeStatus::Deleted);
    CHECK(model.statusOf(Side::Right, rightFresh) == NodeStatus::Added);
    CHECK(model.statusOf(Side::Left, left.root()) == NodeStatus::Unchanged);
}

TEST_CASE("an added subtree reports every node in it", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r/>");
    const Tree right = parse(*provider, "<r><a><b/><c/></a></r>");

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.added == 3);
    CHECK(model.deleted == 0);
}

TEST_CASE("documents with different roots are a wholesale replacement", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<alpha><n/></alpha>");
    const Tree right = parse(*provider, "<beta><n/></beta>");

    const auto model = nmxd::diffTrees(left, right, *provider);
    CHECK(model.deleted == 2);
    CHECK(model.added == 2);
    CHECK(model.modified == 0);
}

TEST_CASE("the change list is ordered by the document, not the arena", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><a/><b/><c/><d/></r>");
    const Tree right = parse(*provider, "<r><a/><B/><c/><D/></r>");

    const auto model = nmxd::diffTrees(left, right, *provider);
    const std::string serialized = nmxd::serializeChanges(left, right, model);

    // The change to b is reported before the change to d, which is what makes
    // next-change navigation walk the file in reading order.
    const auto bAt = serialized.find("/b[0]");
    const auto dAt = serialized.find("/d[0]");
    REQUIRE(bAt != std::string::npos);
    REQUIRE(dAt != std::string::npos);
    CHECK(bAt < dAt);
}

TEST_CASE("cancelling a match reports that it stopped", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    std::string xml = "<r>";
    for (int i = 0; i < 500; ++i) {
        xml += "<n k=\"" + std::to_string(i) + "\"/>";
    }
    xml += "</r>";

    const Tree left = parse(*provider, xml);
    const Tree right = parse(*provider, xml);

    std::stop_source source;
    source.request_stop();
    const auto model = nmxd::diffTrees(left, right, *provider, source.get_token());
    CHECK(model.cancelled);
}

TEST_CASE("the size guard trims similarity and says so", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><box label=\"one\"><a/><b/></box></r>");
    const Tree right = parse(*provider, "<r><box label=\"two\"><a/><b/></box></r>");

    MatchOptions options;
    options.maxNodesForSimilarity = 1;  // far below the tree size
    const auto result = nmxd::matchTrees(left, right, *provider, {}, options);

    CHECK(result.quality == nmxd::MatchQuality::SimilarityTrimmed);
    CHECK(std::string(describe(result.quality)).find("trimmed") != std::string::npos);
}

TEST_CASE("a node path names a node by kind and position among its own kind", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree tree = parse(*provider, "<r><a/><b/><a/></r>");

    const auto& children = tree.node(tree.root()).children;
    CHECK(nmxd::nodePath(tree, tree.root()) == "/r");
    CHECK(nmxd::nodePath(tree, children[0]) == "/r/a[0]");
    CHECK(nmxd::nodePath(tree, children[1]) == "/r/b[0]");
    // The second <a> is a[1] even though a <b> sits between them.
    CHECK(nmxd::nodePath(tree, children[2]) == "/r/a[1]");
}

TEST_CASE("a hundred thousand nodes match inside the milestone budget", "[match][budget][.slow]") {
    const auto provider = nmxd::makeGenericXmlProvider();

    const auto generate = [](int tweakEvery) {
        std::string xml = "<tree>";
        for (int i = 0; i < 33000; ++i) {
            xml += "<node id=\"n" + std::to_string(i) + "\">";
            xml += "<property name=\"speed\" value=\"";
            xml += (tweakEvery > 0 && i % tweakEvery == 0) ? "9.9" : "1.0";
            xml += "\"/><property name=\"waypoint\" value=\"wp\"/></node>";
        }
        xml += "</tree>";
        return xml;
    };

    const Tree left = parse(*provider, generate(0));
    const Tree right = parse(*provider, generate(300));
    INFO("nodes " << left.size());
    CHECK(left.size() >= 99000);

    const auto started = std::chrono::steady_clock::now();
    const auto model = nmxd::diffTrees(left, right, *provider);
    const double millis =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();

    INFO("match took " << millis << " ms, changed " << model.changedNodes());
    CHECK(model.quality == nmxd::MatchQuality::Full);
    CHECK(model.modified == 110);
    CHECK(millis < 2000.0);
}

TEST_CASE("a tripped size guard still gives a real diff", "[match]") {
    // The guard used to skip root pairing along with the similarity pass, which
    // turned a one-property edit in a large file into a total replacement: the
    // worst possible answer for exactly the file a reader most needs help with.
    const auto provider = nmxd::makeGenericXmlProvider();

    const auto generate = [](const char* speed) {
        std::string xml = "<tree>";
        for (int i = 0; i < 200; ++i) {
            xml += "<node id=\"n" + std::to_string(i) + "\">";
            xml += "<property name=\"speed\" value=\"";
            xml += (i == 100) ? speed : "1.0";
            xml += "\"/></node>";
        }
        xml += "</tree>";
        return xml;
    };

    const Tree left = parse(*provider, generate("1.0"));
    const Tree right = parse(*provider, generate("9.9"));

    MatchOptions options;
    options.maxNodesForSimilarity = 1;  // far below the tree size, so it trips
    const auto model = nmxd::diffTrees(left, right, *provider, {}, options);

    CHECK(model.quality == nmxd::MatchQuality::SimilarityTrimmed);

    // Everything unchanged still pairs through the second pass, and the roots
    // pair outside the guard, so almost nothing is reported.
    CHECK(model.added <= 2);
    CHECK(model.deleted <= 2);
    CHECK(model.unchanged > 390);
}

TEST_CASE("the roots pair even when nothing else does", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><a x=\"1\"/></r>");
    const Tree right = parse(*provider, "<r><b y=\"2\"/></r>");

    const auto result = nmxd::matchTrees(left, right, *provider);
    CHECK(result.matching.toRight(left.root()) == right.root());
}

TEST_CASE("a node's change is reachable without scanning the change list", "[match]") {
    // The details panel asks this per visible node per frame, so it has to be a
    // lookup rather than a search.
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><item speed=\"1.0\" name=\"a\"/><quiet/></r>");
    const Tree right = parse(*provider, "<r><item speed=\"1.4\" name=\"a\"/><quiet/></r>");

    const auto model = nmxd::diffTrees(left, right, *provider);

    const NodeId rightItem = right.node(right.root()).children[0];
    const NodeId leftItem = left.node(left.root()).children[0];

    const auto* change = model.changeFor(Side::Right, rightItem);
    REQUIRE(change != nullptr);
    CHECK(change->status == NodeStatus::Modified);
    CHECK(change->changedProperties == std::vector<std::string>{"speed"});

    // Both sides of one pair lead to the same record, so the panel can ask from
    // whichever side it happens to be showing.
    CHECK(model.changeFor(Side::Left, leftItem) == change);
}

TEST_CASE("an unchanged node has no change to look up", "[match]") {
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><item speed=\"1.0\"/><quiet/></r>");
    const Tree right = parse(*provider, "<r><item speed=\"1.4\"/><quiet/></r>");

    const auto model = nmxd::diffTrees(left, right, *provider);

    const NodeId quiet = right.node(right.root()).children[1];
    CHECK(model.changeFor(Side::Right, quiet) == nullptr);

    // And an id past the end is a question rather than a crash, because the
    // panel may still be holding a selection from the previous snapshot.
    CHECK(model.changeFor(Side::Right, static_cast<NodeId>(right.size() + 10)) == nullptr);
    CHECK(model.changeFor(Side::Left, kInvalidNode) == nullptr);
}

TEST_CASE("a property present on only one side is reported as changed", "[match]") {
    // The details panel shows one side, so a property that was taken away has
    // no row of its own. It can only be drawn if the diff names it.
    const auto provider = nmxd::makeGenericXmlProvider();
    const Tree left = parse(*provider, "<r><item keep=\"1\" doomed=\"yes\"/></r>");
    const Tree right = parse(*provider, "<r><item keep=\"1\" fresh=\"new\"/></r>");

    const auto model = nmxd::diffTrees(left, right, *provider);
    const NodeId item = right.node(right.root()).children[0];

    const auto* change = model.changeFor(Side::Right, item);
    REQUIRE(change != nullptr);
    CHECK(change->changedProperties == std::vector<std::string>{"doomed", "fresh"});
}
