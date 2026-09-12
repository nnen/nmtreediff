/// \file
/// \brief Implementation of the document view.

#include "core/dom.h"

namespace nmxd {

// ---- DomProperty ----------------------------------------------------------

std::string_view DomProperty::name() const noexcept {
    return valid() ? std::string_view(property_->name) : std::string_view{};
}

std::string_view DomProperty::value() const noexcept {
    return valid() ? std::string_view(property_->value) : std::string_view{};
}

PropertyForm DomProperty::form() const noexcept {
    return valid() ? property_->form : PropertyForm::Scalar;
}

SourceSpan DomProperty::span() const noexcept {
    return valid() ? property_->span : SourceSpan{};
}

std::size_t DomProperty::partCount() const noexcept {
    return valid() ? property_->children.size() : 0;
}

DomProperty DomProperty::partAt(std::size_t index) const noexcept {
    if (!valid() || index >= property_->children.size()) {
        return {};
    }
    return DomProperty(&property_->children[index]);
}

DomProperty DomProperty::part(std::string_view partName) const noexcept {
    return valid() ? DomProperty(property_->findPart(partName)) : DomProperty{};
}

// ---- DomChildRange --------------------------------------------------------

DomNode DomChildRange::iterator::operator*() const {
    return DomNode(dom_, (*ids_)[index_]);
}

// ---- DomNode --------------------------------------------------------------

const Node& DomNode::node() const noexcept {
    return dom_->tree_->node(id_);
}

std::string_view DomNode::name() const noexcept {
    return valid() ? std::string_view(node().kind) : std::string_view{};
}

SourceSpan DomNode::span() const noexcept {
    return valid() ? node().span : SourceSpan{};
}

std::string_view DomNode::text() const noexcept {
    if (!valid()) {
        return {};
    }
    const Property* text = node().findProperty(kTextProperty);
    return text != nullptr ? std::string_view(text->value) : std::string_view{};
}

std::uint32_t DomNode::depth() const noexcept {
    return valid() ? node().depth : 0;
}

DomNode DomNode::parent() const noexcept {
    if (!valid() || node().parent == kInvalidNode) {
        return {};
    }
    return DomNode(dom_, node().parent);
}

DomNode DomNode::firstChild() const noexcept {
    return childAt(0);
}

DomNode DomNode::lastChild() const noexcept {
    return valid() && !node().children.empty() ? childAt(node().children.size() - 1) : DomNode{};
}

DomNode DomNode::nextSibling() const noexcept {
    const DomNode up = parent();
    return up.valid() ? up.childAt(dom_->siblingIndex(id_) + 1) : DomNode{};
}

DomNode DomNode::prevSibling() const noexcept {
    const DomNode up = parent();
    if (!up.valid()) {
        return {};
    }
    const std::uint32_t index = dom_->siblingIndex(id_);
    return index == 0 ? DomNode{} : up.childAt(index - 1);
}

std::size_t DomNode::childCount() const noexcept {
    return valid() ? node().children.size() : 0;
}

DomNode DomNode::childAt(std::size_t index) const noexcept {
    if (!valid() || index >= node().children.size()) {
        return {};
    }
    return DomNode(dom_, node().children[index]);
}

DomChildRange DomNode::children() const noexcept {
    return DomChildRange(dom_, valid() ? &node().children : nullptr);
}

std::size_t DomNode::propertyCount() const noexcept {
    return valid() ? node().properties.size() : 0;
}

DomProperty DomNode::propertyAt(std::size_t index) const noexcept {
    if (!valid() || index >= node().properties.size()) {
        return {};
    }
    return DomProperty(&node().properties[index]);
}

DomProperty DomNode::property(std::string_view propertyName) const noexcept {
    return valid() ? DomProperty(node().findProperty(propertyName)) : DomProperty{};
}

std::optional<std::string_view> DomNode::attribute(std::string_view propertyName) const noexcept {
    const DomProperty found = property(propertyName);
    if (!found.valid()) {
        return std::nullopt;
    }
    return found.value();
}

DomPropertyRange DomNode::properties() const noexcept {
    return DomPropertyRange(valid() ? &node().properties : nullptr, {});
}

DomPropertyRange DomNode::properties(std::string_view propertyName) const noexcept {
    return DomPropertyRange(valid() ? &node().properties : nullptr, propertyName);
}

// ---- Ref overloads that take a DOM handle ---------------------------------

Ref Ref::child(const DomNode& element) {
    if (!valid() || !element.valid()) {
        return {};
    }
    Ref made = child(element.name());
    made.setSpan(element.span());
    made.setSource(element.id());
    return made;
}

Ref Ref::property(const DomProperty& source) {
    if (!valid() || !source.valid()) {
        return {};
    }
    const Property& top = *source.get();
    Ref made = property(top.name, top.value);
    made.setSpan(top.span);
    if (top.form != PropertyForm::Scalar) {
        builder_->property(made.id_).form = top.form;
    }

    // Parts are copied breadth by breadth with a worklist, so a property
    // nested to any depth costs memory rather than a call frame per level.
    // Each entry pairs a source property with the handle its parts go under.
    std::vector<std::pair<const Property*, RefId>> pending;
    if (top.hasParts()) {
        pending.emplace_back(&top, made.id_);
    }
    while (!pending.empty()) {
        const auto [from, into] = pending.back();
        pending.pop_back();
        for (const Property& part : from->children) {
            const RefId copied = builder_->addProperty(into, part.name, part.value, part.form);
            builder_->property(copied).span = part.span;
            if (part.hasParts()) {
                pending.emplace_back(&part, copied);
            }
        }
    }
    return made;
}

// ---- Dom ------------------------------------------------------------------

Dom::Dom(const Tree& tree) : tree_(&tree), siblingIndex_(tree.size(), 0) {
    // One pass over every child list, so a sibling step is a lookup.
    for (const Node& node : tree.nodes()) {
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            siblingIndex_[node.children[i]] = static_cast<std::uint32_t>(i);
        }
    }
}

DomNode Dom::root() const noexcept {
    return tree_->empty() ? DomNode{} : DomNode(this, tree_->root());
}

DomNode Dom::at(DomId id) const noexcept {
    return id < tree_->size() ? DomNode(this, id) : DomNode{};
}

}  // namespace nmxd
