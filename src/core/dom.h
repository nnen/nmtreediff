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
    [[nodiscard]] bool valid() const noexcept { return property_ != nullptr; }

    /// \brief Returns the property's name.
    [[nodiscard]] std::string_view name() const noexcept;

    /// \brief Returns the property's value, in any form.
    [[nodiscard]] std::string_view value() const noexcept;

    /// \brief Returns how the property's content is shaped.
    [[nodiscard]] PropertyForm form() const noexcept;

    /// \brief Returns where the property sits in the source bytes.
    [[nodiscard]] SourceSpan span() const noexcept;

    /// \brief Returns how many parts the property has.
    [[nodiscard]] std::size_t partCount() const noexcept;

    /// \brief Returns one part by position.
    ///
    /// \param index Zero-based, in document order.
    ///
    /// \returns The part, or an invalid handle past the end.
    [[nodiscard]] DomProperty partAt(std::size_t index) const noexcept;

    /// \brief Returns the first part with a name.
    ///
    /// \returns The part, or an invalid handle when there is none.
    [[nodiscard]] DomProperty part(std::string_view partName) const noexcept;

    /// \brief Returns the underlying property.
    ///
    /// \remarks For code that has to hand a whole property to the builder,
    ///          which copies it. Null for an invalid handle.
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
        using iterator_category = std::forward_iterator_tag;
        using value_type = DomProperty;
        using difference_type = std::ptrdiff_t;
        using pointer = const DomProperty*;
        using reference = DomProperty;

        iterator() = default;
        iterator(const std::vector<Property>* list, std::size_t index, std::string_view filter)
            : list_(list), index_(index), filter_(filter) {
            settle();
        }

        DomProperty operator*() const { return DomProperty(&(*list_)[index_]); }
        iterator& operator++() {
            ++index_;
            settle();
            return *this;
        }
        iterator operator++(int) {
            iterator before = *this;
            ++*this;
            return before;
        }
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

    DomPropertyRange(const std::vector<Property>* list, std::string_view filter)
        : list_(list), filter_(filter) {}

    [[nodiscard]] iterator begin() const { return iterator(list_, 0, filter_); }
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
        using iterator_category = std::forward_iterator_tag;
        using value_type = DomNode;
        using difference_type = std::ptrdiff_t;
        using pointer = const DomNode*;
        using reference = DomNode;

        iterator() = default;
        iterator(const Dom* dom, const std::vector<NodeId>* ids, std::size_t index,
                 std::string_view filter)
            : dom_(dom), ids_(ids), index_(index), filter_(filter) {
            settle();
        }

        DomNode operator*() const;
        iterator& operator++() {
            ++index_;
            settle();
            return *this;
        }
        iterator operator++(int) {
            iterator before = *this;
            ++*this;
            return before;
        }
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

    DomChildRange(const Dom* dom, const std::vector<NodeId>* ids, std::string_view filter)
        : dom_(dom), ids_(ids), filter_(filter) {}

    [[nodiscard]] iterator begin() const { return iterator(dom_, ids_, 0, filter_); }
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
    [[nodiscard]] bool valid() const noexcept { return dom_ != nullptr && id_ != kInvalidDom; }

    /// \brief Returns the element's id, which a builder handle records as its
    ///        source.
    [[nodiscard]] DomId id() const noexcept { return id_; }

    /// \brief Returns the element's name as the base format read it.
    ///
    /// \remarks The tag name for XML. For JSON the member key, `$` for the
    ///          outermost value and `item` for an element of an array.
    [[nodiscard]] std::string_view name() const noexcept;

    /// \brief Returns the element's extent in the source bytes.
    [[nodiscard]] SourceSpan span() const noexcept;

    /// \brief Returns the element's text content.
    ///
    /// \returns The value of its `#text` property, or empty when it has none.
    [[nodiscard]] std::string_view text() const noexcept;

    /// \brief Returns how deep the element sits, with the root at zero.
    [[nodiscard]] std::uint32_t depth() const noexcept;

    /// \brief Returns the enclosing element, or an invalid handle for the
    ///        root.
    [[nodiscard]] DomNode parent() const noexcept;

    /// \brief Returns the first child, or an invalid handle for a leaf.
    [[nodiscard]] DomNode firstChild() const noexcept;

    /// \brief Returns the last child, or an invalid handle for a leaf.
    [[nodiscard]] DomNode lastChild() const noexcept;

    /// \brief Returns the next sibling, or an invalid handle for the last.
    [[nodiscard]] DomNode nextSibling() const noexcept;

    /// \brief Returns the previous sibling, or an invalid handle for the
    ///        first.
    [[nodiscard]] DomNode prevSibling() const noexcept;

    /// \brief Returns how many children the element has.
    [[nodiscard]] std::size_t childCount() const noexcept;

    /// \brief Returns one child by position.
    ///
    /// \returns The child, or an invalid handle past the end.
    [[nodiscard]] DomNode childAt(std::size_t index) const noexcept;

    /// \brief Returns the children, in document order.
    [[nodiscard]] DomChildRange children() const noexcept;

    /// \brief Returns the first child with a name.
    ///
    /// \returns The child, or an invalid handle when there is none.
    [[nodiscard]] DomNode child(std::string_view childName) const noexcept;

    /// \brief Returns every child with a name, in document order.
    [[nodiscard]] DomChildRange children(std::string_view childName) const noexcept;

    /// \brief Returns how many properties the element has, repeats included.
    [[nodiscard]] std::size_t propertyCount() const noexcept;

    /// \brief Returns one property by position, in document order.
    ///
    /// \returns The property, or an invalid handle past the end.
    [[nodiscard]] DomProperty propertyAt(std::size_t index) const noexcept;

    /// \brief Returns the first property with a name.
    ///
    /// \returns The property, or an invalid handle when there is none.
    [[nodiscard]] DomProperty property(std::string_view propertyName) const noexcept;

    /// \brief Returns the value of the first property with a name.
    ///
    /// \returns The value, or nothing when there is no such property.
    ///
    /// \remarks The shortcut for the common case. It is honest for a scalar
    ///          and for a record or sequence that carries a value; a property
    ///          that is only parts answers with an empty string.
    [[nodiscard]] std::optional<std::string_view> attribute(std::string_view propertyName) const noexcept;

    /// \brief Returns every property, repeats included, in document order.
    [[nodiscard]] DomPropertyRange properties() const noexcept;

    /// \brief Returns every property with a name, in document order.
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
    [[nodiscard]] DomNode root() const noexcept;

    /// \brief Returns an element by id.
    ///
    /// \returns The element, or an invalid handle for an id the document does
    ///          not have.
    [[nodiscard]] DomNode at(DomId id) const noexcept;

    /// \brief Returns how many elements the document has.
    [[nodiscard]] std::size_t size() const noexcept { return tree_->size(); }

    /// \brief Returns the name of the format that read the document.
    [[nodiscard]] std::string_view baseFormat() const noexcept { return tree_->formatName(); }

    /// \brief Returns the tree this is a view over.
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
