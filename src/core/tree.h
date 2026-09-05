#pragma once

/// \file
/// \brief The parsed document: nodes and their properties, held in a flat
///        arena.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/source.h"

namespace nmxd {

/// \brief Identifies a node within one Tree.
///
/// \remarks An index into the arena, not a pointer. Two trees number their
///          nodes independently, so an id is only meaningful alongside the tree
///          it came from.
using NodeId = std::uint32_t;

/// \brief The id that means "no node".
inline constexpr NodeId kInvalidNode = 0xFFFFFFFFu;

/// \brief The property name given to a leaf element's text content.
///
/// \remarks Folding text into the property list means the views and the matcher
///          need one concept rather than two.
inline constexpr std::string_view kTextProperty = "#text";

/// \brief The property name given to a node whose content is a single scalar.
///
/// \remarks The counterpart of kTextProperty for a format whose leaf carries a
///          value rather than text, such as an element of a JSON array. Named
///          here rather than in one provider so that two formats cannot pick
///          clashing names for the same idea.
inline constexpr std::string_view kValueProperty = "#value";

/// \brief A named value attached to a node.
struct Property {
    /// \brief The property name, unique within its node.
    std::string name;
    /// \brief The property value as written in the source.
    std::string value;
    /// \brief Where the property sits in the source bytes.
    SourceSpan span;

    /// \brief Compares two properties for equality.
    friend bool operator==(const Property&, const Property&) = default;
};

/// \brief One node of a parsed document.
struct Node {
    /// \brief This node's own id.
    NodeId id = kInvalidNode;
    /// \brief The parent's id, or kInvalidNode for the root.
    NodeId parent = kInvalidNode;
    /// \brief Child ids in document order.
    std::vector<NodeId> children;

    /// \brief What kind of node this is.
    ///
    /// \remarks An element name, or whatever the provider decided identifies
    ///          this kind of node. The matcher compares it but never interprets
    ///          it.
    std::string kind;

    /// \brief The node's properties in document order.
    ///
    /// \remarks Display order comes from the provider's ranking instead. Order
    ///          here is never used for matching.
    std::vector<Property> properties;

    /// \brief The node's full extent in the source bytes.
    SourceSpan span;

    /// \brief A hash of this node's whole subtree.
    ///
    /// \remarks Filled in by computeHashes(). Zero until then.
    std::uint64_t contentHash = 0;

    /// \brief Distance from the root, which is at depth zero.
    ///
    /// \remarks Filled in by Tree::finalize().
    std::uint32_t depth = 0;

    /// \brief How many nodes sit below this one.
    ///
    /// \remarks Filled in by Tree::finalize().
    std::uint32_t descendantCount = 0;

    /// \brief Finds a property by name.
    ///
    /// \param name The property name to look for.
    ///
    /// \returns A pointer to the property, or `nullptr` when this node has no
    ///          property of that name. Invalidated by any change to the node.
    [[nodiscard]] const Property* findProperty(std::string_view name) const noexcept;

    /// \brief Reports whether this node has children.
    ///
    /// \returns `true` when the node has no children.
    [[nodiscard]] bool isLeaf() const noexcept { return children.empty(); }
};

/// \brief A parsed document, held as a flat arena of nodes.
///
/// \remarks Nodes are addressed by index rather than by pointer. That keeps a
///          subtree's nodes close together in memory during matching, makes a
///          finished tree trivially cheap to hand between threads, and lets a
///          matching be two arrays of indices rather than a map of pointers.
///
///          Construction is expected to be depth-first in document order, which
///          gives two invariants several passes rely on: a parent always has a
///          lower index than its children, and a node's descendants occupy a
///          contiguous run of indices immediately after it.
class Tree {
public:
    /// \brief Appends a node to the arena.
    ///
    /// \param parent The parent's id, or kInvalidNode to create the root.
    /// \param kind What kind of node this is.
    /// \param span The node's extent in the source bytes.
    ///
    /// \returns The new node's id.
    ///
    /// \remarks The first node added becomes the root. The parent must already
    ///          exist, which depth-first construction guarantees.
    NodeId add(NodeId parent, std::string kind, SourceSpan span);

    /// \brief Attaches a property to a node.
    ///
    /// \param node The node to attach to.
    /// \param name The property name.
    /// \param value The property value.
    /// \param span Where the property sits in the source bytes.
    void addProperty(NodeId node, std::string name, std::string value, SourceSpan span = {});

    /// \brief Accesses a node by id.
    ///
    /// \param id The node to access.
    ///
    /// \returns A reference to the node. Undefined for an id this tree did not
    ///          issue.
    [[nodiscard]] Node& node(NodeId id) { return nodes_[id]; }

    /// \copydoc node(NodeId)
    [[nodiscard]] const Node& node(NodeId id) const { return nodes_[id]; }

    /// \brief Returns every node in arena order.
    ///
    /// \returns The nodes, which are in document order because construction is
    ///          depth-first.
    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return nodes_; }

    /// \brief Returns the root node's id.
    ///
    /// \returns The root, or kInvalidNode when the tree is empty.
    [[nodiscard]] NodeId root() const noexcept { return root_; }

    /// \brief Returns the number of nodes.
    ///
    /// \returns The node count.
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    /// \brief Reports whether the tree holds any nodes.
    ///
    /// \returns `true` when the tree is empty.
    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

    /// \brief Returns the name of the format that produced this tree.
    ///
    /// \returns The provider name, empty when none was recorded.
    [[nodiscard]] const std::string& formatName() const noexcept { return formatName_; }

    /// \brief Records which format produced this tree.
    ///
    /// \param name The provider name.
    void setFormatName(std::string name) { formatName_ = std::move(name); }

    /// \brief Fills in Node::depth and Node::descendantCount for every node.
    ///
    /// \remarks Call once, after the last node is added and before anything
    ///          reads those fields. Calling it again is harmless and produces
    ///          the same result.
    void finalize();

    /// \brief Reserves arena space for an expected node count.
    ///
    /// \param count The number of nodes to make room for.
    void reserve(std::size_t count) { nodes_.reserve(count); }

private:
    std::vector<Node> nodes_;
    NodeId root_ = kInvalidNode;
    std::string formatName_;
};

}  // namespace nmxd
