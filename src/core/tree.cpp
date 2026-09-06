/// \file
/// \brief Implementation of the node arena.

#include "core/tree.h"

#include <algorithm>

namespace nmxd {

const Property* Node::findProperty(std::string_view name) const noexcept {
    const auto it = std::find_if(properties.begin(), properties.end(),
                                 [name](const Property& p) { return p.name == name; });
    return it == properties.end() ? nullptr : &*it;
}

NodeId Tree::add(NodeId parent, std::string kind, SourceSpan span) {
    const auto id = static_cast<NodeId>(nodes_.size());

    Node node;
    node.id = id;
    node.parent = parent;
    node.kind = std::move(kind);
    node.span = span;
    nodes_.push_back(std::move(node));

    if (parent == kInvalidNode) {
        if (root_ == kInvalidNode) {
            root_ = id;
        }
    } else {
        nodes_[parent].children.push_back(id);
    }
    return id;
}

void Tree::addProperty(NodeId node, std::string name, std::string value, SourceSpan span) {
    nodes_[node].properties.push_back(Property{std::move(name), std::move(value), span});
}

void Tree::addProperty(NodeId node, Property property) {
    nodes_[node].properties.push_back(std::move(property));
}

void Tree::finalize() {
    if (nodes_.empty()) {
        return;
    }

    // Forward for depth, since a parent always precedes its children.
    for (auto& node : nodes_) {
        node.depth = (node.parent == kInvalidNode) ? 0 : nodes_[node.parent].depth + 1;
        node.descendantCount = 0;
    }

    // Backward for descendant counts, for the same reason read the other way:
    // every child has been totalled by the time its parent is reached.
    for (std::size_t i = nodes_.size(); i-- > 0;) {
        const Node& node = nodes_[i];
        if (node.parent != kInvalidNode) {
            nodes_[node.parent].descendantCount += node.descendantCount + 1;
        }
    }
}

}  // namespace nmxd
