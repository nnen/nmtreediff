#pragma once

/// \file
/// \brief The document a provider reads, seen through navigation handles.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

#include "core/builder.h"
#include "core/source.h"
#include "core/tree.h"

namespace nmxd {

class Dom;
class DomNode;

/// \brief A handle on one property of a source element, or one part of such
///        a property.
///
/// \remarks A pointer into the tree the DOM stands over, which is not
///          modified while a provider shapes it, so the handle stays valid for
///          the whole pass. Properties of a source element are attributes as
///          the base format read them, plus whatever it recorded under a
///          leading `#`.
class DomProperty {
public:
    /// \brief Makes a handle on nothing.
    DomProperty() = default;

    /// \brief Reports whether this handle names anything.
    ///
    /// \returns `true` when the handle came from a document.
    [[nodiscard]] bool valid() const noexcept { return property_ != nullptr; }

    /// \brief Returns the property's name.
    ///
    /// \returns The name, or empty for an invalid handle.
    [[nodiscard]] std::string_view name() const noexcept;

    /// \brief Returns the property's value, in any form.
    ///
    /// \returns The value, or empty for a property that is only parts.
    [[nodiscard]] std::string_view value() const noexcept;

    /// \brief Returns how the property's content is shaped.
    ///
    /// \returns Scalar, record or sequence.
    [[nodiscard]] PropertyForm form() const noexcept;

    /// \brief Returns where the property sits in the source bytes.
    ///
    /// \returns The extent, or an empty span for an invalid handle.
    [[nodiscard]] SourceSpan span() const noexcept;

    /// \brief Returns how many parts the property has.
    ///
    /// \returns The part count, zero for a scalar.
    [[nodiscard]] std::size_t partCount() const noexcept;

    /// \brief Returns one part by position.
    ///
    /// \param index Zero-based, in document order.
    ///
    /// \returns The part, or an invalid handle past the end.
    [[nodiscard]] DomProperty partAt(std::size_t index) const noexcept;

    /// \brief Returns the first part with a name.
    ///
    /// \param partName The name to look for.
    ///
    /// \returns The part, or an invalid handle when there is none.
    [[nodiscard]] DomProperty part(std::string_view partName) const noexcept;

    /// \brief Returns the underlying property.
    ///
    /// \returns The property, or null for an invalid handle.
    ///
    /// \remarks For code that has to hand a whole property to the builder,
    ///          which copies it.
    [[nodiscard]] const Property* get() const noexcept { return property_; }

private:
    friend class DomNode;
    friend class DomPropertyRange;

    explicit DomProperty(const Property* property) : property_(property) {}

    const Property* property_ = nullptr;
};

/// \brief A forward range over properties or parts, optionally of one name.
class DomPropertyRange {
public:
    /// \brief Walks the range.
    class iterator {
    public:
        /// \brief Iterator traits, so the standard algorithms accept the range.
        using iterator_category = std::forward_iterator_tag;
        using value_type = DomProperty;             ///< What dereferencing yields.
        using difference_type = std::ptrdiff_t;  ///< Distance type, unused but required.
        using pointer = const DomProperty*;         ///< Pointer type, unused but required.
        using reference = DomProperty;              ///< Yielded by value: handles are cheap.

        /// \brief Makes an iterator on nothing.
        iterator() = default;

        /// \brief Makes an iterator into a property list.
        ///
        /// \param list The properties, or null for an empty range.
        /// \param index Where to start; moved past rejected entries.
        /// \param filter A name to keep to, or empty for every entry.
        iterator(const std::vector<Property>* list, std::size_t index, std::string_view filter)
            : list_(list), index_(index), filter_(filter) {
            settle();
        }

        /// \brief Returns a handle on the current property.
        ///
        /// \returns The handle, by value.
        DomProperty operator*() const { return DomProperty(&(*list_)[index_]); }

        /// \brief Advances to the next property the filter accepts.
        ///
        /// \returns This iterator.
        iterator& operator++() {
            ++index_;
            settle();
            return *this;
        }

        /// \brief Advances, returning the position before the step.
        ///
        /// \returns A copy of this iterator as it was.
        iterator operator++(int) {
            iterator before = *this;
            ++*this;
            return before;
        }

        /// \brief Compares two iterators by position.
        ///
        /// \param a One iterator.
        /// \param b The other.
        ///
        /// \returns `true` when both stand at the same index.
        friend bool operator==(const iterator& a, const iterator& b) {
            return a.index_ == b.index_;
        }

    private:
        /// \brief Moves past entries the filter rejects.
        void settle() {
            if (list_ == nullptr || filter_.empty()) {
                return;
            }
            while (index_ < list_->size() && (*list_)[index_].name != filter_) {
                ++index_;
            }
        }

        const std::vector<Property>* list_ = nullptr;
        std::size_t index_ = 0;
        std::string_view filter_;
    };

    /// \brief Makes a range over a property list.
    ///
    /// \param list The properties, or null for an empty range.
    /// \param filter A name to keep to, or empty for every entry.
    DomPropertyRange(const std::vector<Property>* list, std::string_view filter)
        : list_(list), filter_(filter) {}

    /// \brief Returns an iterator on the first accepted property.
    ///
    /// \returns The start of the range.
    [[nodiscard]] iterator begin() const { return iterator(list_, 0, filter_); }

    /// \brief Returns the iterator past the last property.
    ///
    /// \returns The end of the range.
    [[nodiscard]] iterator end() const {
        return iterator(list_, list_ == nullptr ? 0 : list_->size(), {});
    }

private:
    const std::vector<Property>* list_ = nullptr;
    std::string_view filter_;
};

/// \brief A forward range over the children of a source element.
class DomChildRange {
public:
    /// \brief Walks the range.
    class iterator {
    public:
        /// \brief Iterator traits, so the standard algorithms accept the range.
        using iterator_category = std::forward_iterator_tag;
        using value_type = DomNode;             ///< What dereferencing yields.
        using difference_type = std::ptrdiff_t;  ///< Distance type, unused but required.
        using pointer = const DomNode*;         ///< Pointer type, unused but required.
        using reference = DomNode;              ///< Yielded by value: handles are cheap.

        /// \brief Makes an iterator on nothing.
        iterator() = default;

        /// \brief Makes an iterator into a child list.
        ///
        /// \param dom The document the children belong to.
        /// \param ids The children's ids, or null for an empty range.
        /// \param index Where to start; moved past rejected entries.
        /// \param filter A name to keep to, or empty for every child.
        iterator(const Dom* dom, const std::vector<NodeId>* ids, std::size_t index,
                 std::string_view filter)
            : dom_(dom), ids_(ids), index_(index), filter_(filter) {
            settle();
        }

        /// \brief Returns a handle on the current child.
        ///
        /// \returns The handle, by value.
        DomNode operator*() const;

        /// \brief Advances to the next child the filter accepts.
        ///
        /// \returns This iterator.
        iterator& operator++() {
            ++index_;
            settle();
            return *this;
        }

        /// \brief Advances, returning the position before the step.
        ///
        /// \returns A copy of this iterator as it was.
        iterator operator++(int) {
            iterator before = *this;
            ++*this;
            return before;
        }

        /// \brief Compares two iterators by position.
        ///
        /// \param a One iterator.
        /// \param b The other.
        ///
        /// \returns `true` when both stand at the same index.
        friend bool operator==(const iterator& a, const iterator& b) {
            return a.index_ == b.index_;
        }

    private:
        /// \brief Moves past children the filter rejects.
        void settle();

        const Dom* dom_ = nullptr;
        const std::vector<NodeId>* ids_ = nullptr;
        std::size_t index_ = 0;
        std::string_view filter_;
    };

    /// \brief Makes a range over a child list.
    ///
    /// \param dom The document the children belong to.
    /// \param ids The children's ids, or null for an empty range.
    /// \param filter A name to keep to, or empty for every child.
    DomChildRange(const Dom* dom, const std::vector<NodeId>* ids, std::string_view filter)
        : dom_(dom), ids_(ids), filter_(filter) {}

    /// \brief Returns an iterator on the first accepted child.
    ///
    /// \returns The start of the range.
    [[nodiscard]] iterator begin() const { return iterator(dom_, ids_, 0, filter_); }

    /// \brief Returns the iterator past the last child.
    ///
    /// \returns The end of the range.
    [[nodiscard]] iterator end() const {
        return iterator(dom_, ids_, ids_ == nullptr ? 0 : ids_->size(), {});
    }

private:
    const Dom* dom_ = nullptr;
    const std::vector<NodeId>* ids_ = nullptr;
    std::string_view filter_;
};

/// \brief A handle on one element of the document a provider read.
///
/// \remarks An index into the tree the base format produced, with navigation
///          mapped onto what Node already carries. Nothing is built alongside
///          that tree: the DOM is a view, and a million-element document is
///          one copy in memory during shaping rather than two.
///
///          There is no traversal here on purpose. Navigation is by handle and
///          the provider owns the walk; see ShapeContext for how to queue one
///          rather than recurse.
class DomNode {
public:
    /// \brief Makes a handle on nothing.
    DomNode() = default;

    /// \brief Reports whether this handle names anything.
    ///
    /// \returns `true` when the handle came from a document.
    [[nodiscard]] bool valid() const noexcept { return dom_ != nullptr && id_ != kInvalidDom; }

    /// \brief Returns the element's id, which a builder handle records as its
    ///        source.
    ///
    /// \returns The id, or kInvalidDom for an invalid handle.
    [[nodiscard]] DomId id() const noexcept { return id_; }

    /// \brief Returns the element's name as the base format read it.
    ///
    /// \returns The name, or empty for an invalid handle.
    ///
    /// \remarks The tag name for XML. For JSON the member key, `$` for the
    ///          outermost value and `item` for an element of an array.
    [[nodiscard]] std::string_view name() const noexcept;

    /// \brief Returns the element's extent in the source bytes.
    ///
    /// \returns The span, or an empty one for an invalid handle.
    [[nodiscard]] SourceSpan span() const noexcept;

    /// \brief Returns the element's text content.
    ///
    /// \returns The value of its `#text` property, or empty when it has none.
    [[nodiscard]] std::string_view text() const noexcept;

    /// \brief Returns how deep the element sits.
    ///
    /// \returns The depth, with the root at zero.
    [[nodiscard]] std::uint32_t depth() const noexcept;

    /// \brief Returns the enclosing element.
    ///
    /// \returns The parent, or an invalid handle for the root.
    [[nodiscard]] DomNode parent() const noexcept;

    /// \brief Returns the first child.
    ///
    /// \returns The child, or an invalid handle for a leaf.
    [[nodiscard]] DomNode firstChild() const noexcept;

    /// \brief Returns the last child.
    ///
    /// \returns The child, or an invalid handle for a leaf.
    [[nodiscard]] DomNode lastChild() const noexcept;

    /// \brief Returns the next sibling.
    ///
    /// \returns The sibling, or an invalid handle for the last child.
    [[nodiscard]] DomNode nextSibling() const noexcept;

    /// \brief Returns the previous sibling.
    ///
    /// \returns The sibling, or an invalid handle for the first child.
    [[nodiscard]] DomNode prevSibling() const noexcept;

    /// \brief Returns how many children the element has.
    ///
    /// \returns The child count, zero for a leaf.
    [[nodiscard]] std::size_t childCount() const noexcept;

    /// \brief Returns one child by position.
    ///
    /// \param index Zero-based, in document order.
    ///
    /// \returns The child, or an invalid handle past the end.
    [[nodiscard]] DomNode childAt(std::size_t index) const noexcept;

    /// \brief Returns the children, in document order.
    ///
    /// \returns A range over every child.
    [[nodiscard]] DomChildRange children() const noexcept;

    /// \brief Returns the first child with a name.
    ///
    /// \param childName The name to look for.
    ///
    /// \returns The child, or an invalid handle when there is none.
    [[nodiscard]] DomNode child(std::string_view childName) const noexcept;

    /// \brief Returns every child with a name, in document order.
    ///
    /// \param childName The name to keep to.
    ///
    /// \returns A range over the children so named.
    [[nodiscard]] DomChildRange children(std::string_view childName) const noexcept;

    /// \brief Returns how many properties the element has, repeats included.
    ///
    /// \returns The property count.
    [[nodiscard]] std::size_t propertyCount() const noexcept;

    /// \brief Returns one property by position, in document order.
    ///
    /// \param index Zero-based, in document order.
    ///
    /// \returns The property, or an invalid handle past the end.
    [[nodiscard]] DomProperty propertyAt(std::size_t index) const noexcept;

    /// \brief Returns the first property with a name.
    ///
    /// \param propertyName The name to look for.
    ///
    /// \returns The property, or an invalid handle when there is none.
    [[nodiscard]] DomProperty property(std::string_view propertyName) const noexcept;

    /// \brief Returns the value of the first property with a name.
    ///
    /// \param propertyName The name to look for.
    ///
    /// \returns The value, or nothing when there is no such property.
    ///
    /// \remarks The shortcut for the common case. It is honest for a scalar
    ///          and for a record or sequence that carries a value; a property
    ///          that is only parts answers with an empty string.
    [[nodiscard]] std::optional<std::string_view> attribute(std::string_view propertyName) const noexcept;

    /// \brief Returns every property, repeats included, in document order.
    ///
    /// \returns A range over every property.
    [[nodiscard]] DomPropertyRange properties() const noexcept;

    /// \brief Returns every property with a name, in document order.
    ///
    /// \param propertyName The name to keep to.
    ///
    /// \returns A range over the properties so named.
    [[nodiscard]] DomPropertyRange properties(std::string_view propertyName) const noexcept;

    /// \brief Compares two handles for identity.
    friend bool operator==(const DomNode& a, const DomNode& b) noexcept {
        return a.dom_ == b.dom_ && a.id_ == b.id_;
    }

private:
    friend class Dom;
    friend class DomChildRange::iterator;

    DomNode(const Dom* dom, DomId id) : dom_(dom), id_(id) {}

    [[nodiscard]] const Node& node() const noexcept;

    const Dom* dom_ = nullptr;
    DomId id_ = kInvalidDom;
};

/// \brief The document a provider read, ready to be shaped.
///
/// \remarks Owns nothing but a view. The tree it stands over must outlive it,
///          which the default parse() arranges by holding both for the length
///          of the shaping pass.
class Dom {
public:
    /// \brief Views a tree as a document.
    ///
    /// \param tree The tree a base format produced. Must outlive the view.
    explicit Dom(const Tree& tree);

    /// \brief Returns the outermost element.
    ///
    /// \returns The root, or an invalid handle for an empty document.
    [[nodiscard]] DomNode root() const noexcept;

    /// \brief Returns an element by id.
    ///
    /// \param id The element's id.
    ///
    /// \returns The element, or an invalid handle for an id the document does
    ///          not have.
    [[nodiscard]] DomNode at(DomId id) const noexcept;

    /// \brief Returns how many elements the document has.
    ///
    /// \returns The element count.
    [[nodiscard]] std::size_t size() const noexcept { return tree_->size(); }

    /// \brief Returns the name of the format that read the document.
    ///
    /// \returns The base format's name, `xml` or `json` for the built-ins.
    [[nodiscard]] std::string_view baseFormat() const noexcept { return tree_->formatName(); }

    /// \brief Returns the tree this is a view over.
    ///
    /// \returns The tree given to the constructor.
    [[nodiscard]] const Tree& tree() const noexcept { return *tree_; }

private:
    friend class DomNode;

    /// \brief Returns an element's position among its siblings.
    [[nodiscard]] std::uint32_t siblingIndex(DomId id) const noexcept { return siblingIndex_[id]; }

    const Tree* tree_;

    /// \brief Each element's position in its parent's child list.
    ///
    /// \remarks Computed once so that nextSibling() is a lookup rather than a
    ///          search of the parent's list, which would make walking a wide
    ///          element's children by sibling quadratic.
    std::vector<std::uint32_t> siblingIndex_;
};

}  // namespace nmxd
