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

/// \brief What shape a property's content has.
///
/// \remarks Three forms rather than a flag, because a flag saying how parts
///          compare says nothing when there are none: an empty array, an empty
///          record and an empty string would be one property. The form is part
///          of the hash, so they are three.
enum class PropertyForm : std::uint8_t {
    /// \brief A value and nothing else. Never has parts.
    Scalar,
    /// \brief Named parts whose order means nothing. Compared as a set.
    ///
    /// \remarks A transform, a colour, a bounding box: one thing with named
    ///          fields, so reordering the fields is not a change.
    Record,
    /// \brief Positional parts, named or not. Compared in order.
    ///
    /// \remarks A list of tags: a position in a list is not a name, so
    ///          reordering the items is a change. The same distinction a node
    ///          carries through IFormatProvider::childrenOrdered(), one level
    ///          further down.
    Sequence,
};

/// \brief A named value attached to a node.
struct Property {
    /// \brief The property's name.
    ///
    /// \remarks Names may repeat. Two properties of one node called `tag` are
    ///          two properties, and everything that compares a property list
    ///          treats it as a multiset for that reason. An item of a sequence
    ///          may have no name at all.
    std::string name;

    /// \brief The property's value, in any form.
    ///
    /// \remarks A record or a sequence may carry a value as well as parts. A
    ///          folded `<property name="speed" value="1.0">` with elements
    ///          inside it is one thing with both, and choosing one half would
    ///          drop the other.
    std::string value;

    /// \brief How the content is shaped. See PropertyForm.
    PropertyForm form = PropertyForm::Scalar;

    /// \brief Where the property sits in the source bytes.
    SourceSpan span;

    /// \brief The property's parts. Always empty for a scalar.
    ///
    /// \remarks Flattening a structured value into names like
    ///          `transform.position.x` would turn one changed number into a
    ///          changed string with a made-up name, so the shape the file had is
    ///          kept instead.
    std::vector<Property> children;

    Property() = default;
    Property(const Property&) = default;
    Property(Property&&) noexcept = default;
    Property& operator=(const Property&) = default;
    Property& operator=(Property&&) noexcept = default;

    /// \brief Destroys the property and its parts without recursing.
    ///
    /// \remarks The compiler's destructor would destroy the parts vector,
    ///          which destroys each part, which destroys its parts vector,
    ///          one call frame per level. A property nested a few thousand
    ///          levels deep would then take the process down on its way out.
    ///          This one pulls every descendant up into one flat list first,
    ///          so each destruction is shallow.
    ///
    ///          Copying is still the compiler's and still recursive. Nothing
    ///          in the tool copies a deep property; a property is moved.
    ~Property();

    /// \brief Reports whether this property has parts.
    ///
    /// \returns `true` when the property holds at least one part.
    [[nodiscard]] bool hasParts() const noexcept { return !children.empty(); }

    /// \brief Reports whether the parts are positional.
    ///
    /// \returns `true` for a sequence.
    [[nodiscard]] bool ordered() const noexcept { return form == PropertyForm::Sequence; }

    /// \brief Finds the first part with a name.
    ///
    /// \param partName The name to look for.
    ///
    /// \returns A pointer to the first part of that name, or `nullptr` when
    ///          there is none. Invalidated by any change to the property.
    [[nodiscard]] const Property* findPart(std::string_view partName) const noexcept;
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

    /// \brief Finds the first property with a name.
    ///
    /// \param name The property name to look for.
    ///
    /// \returns A pointer to the first property of that name, or `nullptr`
    ///          when this node has none. Invalidated by any change to the node.
    ///
    /// \remarks Names may repeat, and this answers the one question every
    ///          caller of it asks: the `id`, the `type`, the `#text`. Anything
    ///          that has to see every property of a name walks the list.
    [[nodiscard]] const Property* findProperty(std::string_view name) const noexcept;

    /// \brief Reports whether this node has children.
    ///
    /// \returns `true` when the node has no children.
    [[nodiscard]] bool isLeaf() const noexcept { return children.empty(); }
};

/// \brief What a provider worked out about a node while parsing it.
///
/// \remarks Never part of matching. The hasher walks a node's kind, its
///          properties and its children, and this is none of those, so nothing
///          here can change which nodes pair up.
///
///          It exists for a provider whose answers are expensive to produce.
///          A scripted provider has to cross into an interpreter to decide what
///          a node is called, and doing that per visible card per frame would
///          not work at all, so it answers once while the document is open and
///          leaves the answers here. A provider that computes cheaply never
///          touches this and pays nothing for it.
struct NodeAnnotation {
    /// \brief The identity key's value, or empty for none.
    std::string identity;
    /// \brief Whether that key may be matched across arbitrary distance.
    bool strongIdentity = false;
    /// \brief The first line of the node's card, or empty to use the kind.
    std::string title;
    /// \brief The second line of the node's card, or empty for none.
    std::string subtitle;
    /// \brief The provider's own colour as 0xRRGGBB, or zero for none.
    ///
    /// \remarks Packed rather than a Color, because Color is declared with
    ///          the provider interface and that interface is built on this file.
    std::uint32_t accent = 0;
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

    /// \brief Attaches a property that already has its parts.
    ///
    /// \param node The node to attach it to.
    /// \param property The property, parts and all.
    ///
    /// \remarks The other overload builds a property from a name and a value,
    ///          which is every property a format without structure produces.
    ///          This one takes a property that was assembled first, which is how
    ///          a nested or array property arrives.
    void addProperty(NodeId node, Property property);

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

    /// \brief Returns what a provider recorded about one node.
    ///
    /// \param id The node to look up.
    ///
    /// \returns The annotation, or an empty one when this tree has none.
    ///
    /// \remarks A tree whose provider annotated nothing carries no
    ///          annotations at all, so the common case costs no memory rather
    ///          than an empty record per node.
    [[nodiscard]] const NodeAnnotation& annotation(NodeId id) const {
        static const NodeAnnotation kNone;
        return id < annotations_.size() ? annotations_[id] : kNone;
    }

    /// \brief Records what a provider worked out about one node.
    ///
    /// \param id The node the annotation belongs to.
    /// \param annotation What was worked out.
    ///
    /// \remarks Called while parsing, before the tree is handed to anything
    ///          else. Grows the side table to fit, so nodes may be annotated in
    ///          any order.
    void annotate(NodeId id, NodeAnnotation annotation) {
        if (id >= annotations_.size()) {
            annotations_.resize(static_cast<std::size_t>(id) + 1);
        }
        annotations_[id] = std::move(annotation);
    }

    /// \brief Reports whether any node in this tree carries an annotation.
    ///
    /// \returns `true` when a provider recorded something.
    [[nodiscard]] bool annotated() const noexcept { return !annotations_.empty(); }

private:
    std::vector<Node> nodes_;

    /// \brief Provider working, indexed by NodeId. Empty when unused.
    std::vector<NodeAnnotation> annotations_;
    NodeId root_ = kInvalidNode;
    std::string formatName_;
};

}  // namespace nmxd
