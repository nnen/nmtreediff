#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include "core/dom.h"
#include "core/shape.h"
#include "core/source.h"
#include "formats/xml_generic.h"

using nmtreediff::Dom;
using nmtreediff::DomNode;
using nmtreediff::ParseError;
using nmtreediff::Ref;
using nmtreediff::ShapeContext;
using nmtreediff::SourceFile;
using nmtreediff::SourceSpan;
using nmtreediff::Tree;
using nmtreediff::TreeBuilder;

namespace {

Tree parseXml(const std::string& xml) {
    const auto provider = nmtreediff::makeGenericXmlProvider();
    const auto source = SourceFile::fromMemory(xml, "t.xml");
    auto result = provider->parse(source, {});
    REQUIRE(result.ok());
    return std::move(result).value();
}

/// A shape that copies the document one to one, queueing one job per
/// element the way a script would.
void copyElement(ShapeContext& context, const DomNode& element, Ref into) {
    for (const DomNode child : element.children()) {
        context.next([child, into](ShapeContext& inner) mutable {
            Ref made = into.child(child);
            for (const auto property : child.properties()) {
                made.property(property);
            }
            copyElement(inner, child, made);
        }, child.id(), into.id());
    }
}

std::vector<std::string> kindsInOrder(const Tree& tree) {
    std::vector<std::string> kinds;
    for (const auto& node : tree.nodes()) {
        kinds.push_back(node.kind);
    }
    return kinds;
}

}  // namespace

TEST_CASE("a queued walk builds the same tree whichever way it drains", "[shape]") {
    const Tree source = parseXml("<r><a x=\"1\"><a1/><a2/></a><b/><c><c1><c11/></c1></c></r>");
    const Dom dom(source);
    const std::vector<std::string> expected = kindsInOrder(source);

    auto shapeWith = [&](bool depthFirst) {
        return nmtreediff::shapeTree(dom, "copy", [&](ShapeContext& context) {
            Ref root = context.out().root(dom.root().name());
            root.setSource(dom.root().id());
            for (const DomNode child : dom.root().children()) {
                auto job = [child, root](ShapeContext& inner) mutable {
                    Ref made = root.child(child);
                    copyElement(inner, child, made);
                };
                if (depthFirst) {
                    context.next(job, child.id(), root.id());
                } else {
                    context.later(job, child.id(), root.id());
                }
            }
        });
    };

    auto depthFirst = shapeWith(true);
    auto breadthFirst = shapeWith(false);
    REQUIRE(depthFirst.ok());
    REQUIRE(breadthFirst.ok());
    CHECK(kindsInOrder(depthFirst.value()) == expected);
    CHECK(kindsInOrder(breadthFirst.value()) == expected);
    CHECK(depthFirst.value().node(0).contentHash == breadthFirst.value().node(0).contentHash);
    CHECK(depthFirst.value().failures().empty());
    CHECK(depthFirst.value().unrepresented().empty());
}

TEST_CASE("later appends and next prepends", "[shape]") {
    const Tree source = parseXml("<r/>");
    const Dom dom(source);
    TreeBuilder out("t");
    ShapeContext context(dom, out);

    std::vector<int> order;
    context.later([&](ShapeContext&) { order.push_back(1); });
    context.later([&](ShapeContext& inner) {
        order.push_back(2);
        inner.next([&](ShapeContext&) { order.push_back(3); });
        inner.later([&](ShapeContext&) { order.push_back(5); });
    });
    context.later([&](ShapeContext&) { order.push_back(4); });
    CHECK(context.pending() == 3);
    context.drain();
    CHECK(order == std::vector<int>{1, 2, 3, 4, 5});
    CHECK(context.pending() == 0);
}

TEST_CASE("a failing job is recorded against its element and owner, and the pass goes on",
          "[shape]") {
    const Tree source = parseXml("<r><good/><bad/><after/></r>");
    const Dom dom(source);

    auto result = nmtreediff::shapeTree(dom, "t", [&](ShapeContext& context) {
        Ref root = context.out().root("r");
        root.setSource(dom.root().id());
        for (const DomNode child : dom.root().children()) {
            context.next([child, root](ShapeContext&) mutable {
                if (child.name() == "bad") {
                    throw std::runtime_error("no bad elements here");
                }
                root.child(child);
            }, child.id(), root.id());
        }
    });
    REQUIRE(result.ok());
    const Tree& tree = result.value();

    // The other two children were built; the bad one is missing and named.
    CHECK(kindsInOrder(tree) == std::vector<std::string>{"r", "good", "after"});
    REQUIRE(tree.failures().size() == 1);
    CHECK(tree.failures()[0].message == "no bad elements here");
    CHECK(tree.failures()[0].owner == tree.root());
    CHECK(tree.failures()[0].span == dom.root().childAt(1).span());

    // And the dropped element's bytes are reported, the kept ones are not.
    REQUIRE(tree.unrepresented().size() == 1);
    CHECK(tree.unrepresented()[0] == dom.root().childAt(1).span());
}

TEST_CASE("a pass that builds nothing is a failure, and a cancelled one is cancelled", "[shape]") {
    const Tree source = parseXml("<r/>");
    const Dom dom(source);

    auto failed = nmtreediff::shapeTree(dom, "t", [](ShapeContext&) {
        throw std::runtime_error("before the root");
    });
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error() == ParseError::ShapeFailed);

    auto empty = nmtreediff::shapeTree(dom, "t", [](ShapeContext&) {});
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error() == ParseError::Empty);

    std::stop_source stop;
    auto cancelled = nmtreediff::shapeTree(dom, "t", [&](ShapeContext& context) {
        context.out().root("r");
        context.later([&](ShapeContext&) { FAIL("the drain ran a job after the stop"); });
        stop.request_stop();
    }, stop.get_token());
    REQUIRE_FALSE(cancelled.ok());
    CHECK(cancelled.error() == ParseError::Cancelled);
}

TEST_CASE("a dropped wrapper's bytes exclude the elements kept inside it", "[shape]") {
    // <wrap> is dropped but <kept> inside it is not, so only the wrapper's
    // own tags are unrepresented, and they come out as two spans around the
    // kept element rather than one covering it. The space after the closing
    // tag belongs to <r>, which is represented, so <gone> stays its own span
    // rather than merging with </wrap>.
    const std::string xml = "<r><wrap><kept/></wrap> <gone><deeper/></gone></r>";
    const Tree source = parseXml(xml);
    const Dom dom(source);

    auto result = nmtreediff::shapeTree(dom, "t", [&](ShapeContext& context) {
        Ref root = context.out().root("r");
        root.setSource(dom.root().id());
        root.child(dom.root().firstChild().firstChild());  // kept
    });
    REQUIRE(result.ok());
    const auto& spans = result.value().unrepresented();

    const SourceSpan wrap = dom.root().childAt(0).span();
    const SourceSpan kept = dom.root().childAt(0).firstChild().span();
    const SourceSpan gone = dom.root().childAt(1).span();
    REQUIRE(spans.size() == 3);
    CHECK(spans[0] == SourceSpan{wrap.begin, kept.begin});
    CHECK(spans[1] == SourceSpan{kept.end, wrap.end});
    CHECK(spans[2] == gone);
    CHECK(xml.substr(spans[0].begin, spans[0].end - spans[0].begin) == "<wrap>");
    CHECK(xml.substr(spans[1].begin, spans[1].end - spans[1].begin) == "</wrap>");
}
