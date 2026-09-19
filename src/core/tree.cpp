/// \file
/// \brief Implementation of the node arena.

#include "core/tree.h"

#include <algorithm>
#include <iterator>

namespace nmtreediff {

namespace {

/// \brief Finds the first property in a list with a name.
///
/// \param list The properties to search.
/// \param name The name to look for.
///
/// \returns The first match, or `nullptr`.
const Property* firstNamed(const std::vector<Property>& list, std::string_view name) noexcept {
    const auto it = std::find_if(list.begin(), list.end(),
                                 [name](const Property& p) { return p.name == name; });
    return it == list.end() ? nullptr : &*it;
}

}  // namespace

Property::~Property() {
    if (children.empty()) {
        return;
    }

    // Flatten first, destroy second. Every part is moved into one list, and
    // any parts it has are moved onto the end of that same list, until nothing
    // in it has parts. The list then destroys shallow properties one by one.
    std::vector<Property> pending = std::move(children);
    for (std::size_t i = 0; i < pending.size(); ++i) {
        if (pending[i].children.empty()) {
            continue;
        }
        std::vector<Property> parts = std::move(pending[i].children);
        pending.insert(pending.end(), std::make_move_iterator(parts.begin()),
                       std::make_move_iterator(parts.end()));
    }
}

const Property* Property::findPart(std::string_view partName) const noexcept {
    return firstNamed(children, partName);
}

const Property* Node::findProperty(std::string_view name) const noexcept {
    return firstNamed(properties, name);
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
    Property property;
    property.name = std::move(name);
    property.value = std::move(value);
    property.span = span;
    nodes_[node].properties.push_back(std::move(property));
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

}  // namespace nmtreediff
