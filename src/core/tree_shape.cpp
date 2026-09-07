/// \file
/// \brief Implementation of the tree shaping driver.

#include "core/tree_shape.h"

#include <vector>

#include "core/hash.h"

namespace nmxd {

namespace {

/// \brief How many nodes are opened between cancellation checks.
constexpr std::size_t kCancelCheckInterval = 1024;

/// \brief One node the walk is inside of.
struct Visit {
    NodeId id;               ///< The node itself.
    std::size_t next = 0;    ///< Index of the next child to look at.
};

}  // namespace

Result<Tree, ParseError> shapeTree(const Tree& generic, const SourceFile& source, IShaper& shaper,
                                   const IFormatProvider& provider, std::stop_token token) {
    if (generic.empty()) {
        return fail(ParseError::Empty);
    }

    DefaultShaper fallback;
    ShapeSession session(shaper, fallback, source.text());

    // The arena is walked with an explicit stack, opening a node as the walk
    // reaches it and closing it once every child has been closed.
    const auto open = [&](NodeId id) {
        const Node& node = generic.node(id);
        session.open(node.kind, node.properties, node.span);
    };

    std::vector<Visit> stack;
    std::size_t seen = 0;
    open(generic.root());
    stack.push_back(Visit{generic.root()});
    while (!stack.empty()) {
        Visit& top = stack.back();
        const Node& node = generic.node(top.id);

        if (top.next < node.children.size() && session.descend()) {
            if (++seen % kCancelCheckInterval == 0 && token.stop_requested()) {
                return fail(ParseError::Cancelled);
            }
            const NodeId child = node.children[top.next++];
            open(child);
            stack.push_back(Visit{child});
            continue;
        }

        session.close(std::string{}, SourceSpan{}, node.span.end);
        stack.pop_back();
    }

    Tree tree = session.finish(std::string(provider.name()));
    if (tree.empty()) {
        return fail(ParseError::Empty);
    }
    tree.finalize();
    computeHashes(tree, provider, token);
    if (token.stop_requested()) {
        return fail(ParseError::Cancelled);
    }
    return tree;
}

}  // namespace nmxd
