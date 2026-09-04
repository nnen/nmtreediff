#pragma once

// The parsed document, held in a flat arena.
//
// Nodes are addressed by index rather than pointer. That keeps a subtree's
// nodes close together in memory during matching, makes the whole tree
// trivially cheap to hand between threads, and lets a matching be stored as
// two arrays of indices rather than a map of pointers.
//
// Construction is in document order, so a parent always has a lower index than
// its children. Several passes rely on that.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/source.h"

namespace nmxd {

using NodeId = std::uint32_t;

inline constexpr NodeId kInvalidNode = 0xFFFFFFFFu;

// The name given to a leaf element's text content, so that the views and the
// matcher need one concept rather than two.
inline constexpr std::string_view kTextProperty = "#text";

struct Property {
    std::string name;
    std::string value;
    SourceSpan span;

    friend bool operator==(const Property&, const Property&) = default;
};

struct Node {
    NodeId id = kInvalidNode;
    NodeId parent = kInvalidNode;
    std::vector<NodeId> children;

    // Element name, or whatever the provider decided identifies this kind of
    // node. The matcher compares it but never interprets it.
    std::string kind;

    // Document order. Display order comes from the provider's ranking.
    std::vector<Property> properties;

    // Full extent in the source bytes, which is what links the node view to
    // the text view in both directions.
    SourceSpan span;

    std::uint64_t contentHash = 0;
    std::uint32_t depth = 0;
    std::uint32_t descendantCount = 0;

    [[nodiscard]] const Property* findProperty(std::string_view name) const noexcept;
    [[nodiscard]] bool isLeaf() const noexcept { return children.empty(); }
};

class Tree {
public:
    // The first node added becomes the root. Parent must already exist, which
    // document-order construction guarantees.
    NodeId add(NodeId parent, std::string kind, SourceSpan span);

    void addProperty(NodeId node, std::string name, std::string value, SourceSpan span = {});

    [[nodiscard]] Node& node(NodeId id) { return nodes_[id]; }
    [[nodiscard]] const Node& node(NodeId id) const { return nodes_[id]; }

    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] NodeId root() const noexcept { return root_; }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

    [[nodiscard]] const std::string& formatName() const noexcept { return formatName_; }
    void setFormatName(std::string name) { formatName_ = std::move(name); }

    // Fills in depth and descendantCount. Call once, after the last node is
    // added and before anything reads those fields.
    void finalize();

    void reserve(std::size_t count) { nodes_.reserve(count); }

private:
    std::vector<Node> nodes_;
    NodeId root_ = kInvalidNode;
    std::string formatName_;
};

}  // namespace nmxd
