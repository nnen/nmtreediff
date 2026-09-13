/// \file
/// \brief Implementation of the handle-based tree builder.

#include "core/builder.h"

#include <algorithm>
#include <utility>

#include "core/hash.h"

namespace nmxd {

namespace {

/// \brief Names a form in a message.
///
/// \param form The form to name.
///
/// \returns A lower-case word.
[[nodiscard]] const char* formWord(PropertyForm form) noexcept {
    switch (form) {
        case PropertyForm::Scalar:
            return "scalar";
        case PropertyForm::Record:
            return "record";
        case PropertyForm::Sequence:
            return "sequence";
    }
    return "property";
}

}  // namespace

// ---- Ref ------------------------------------------------------------------

PropertyForm Ref::form() const {
    if (!valid() || isNode()) {
        return PropertyForm::Scalar;
    }
    return builder_->property(id_).form;
}

Ref Ref::child(std::string_view name) {
    if (!valid()) {
        return {};
    }
    if (isNode()) {
        if (name.empty()) {
            throw BuildError("a child node needs a kind");
        }
        return Ref(builder_, builder_->addNode(id_, name));
    }

    // Under a property the rule is the promotion table: a scalar becomes a
    // record when the part is named and a sequence when it is not, and keeps
    // its value either way. A record refuses an unnamed part; a sequence
    // takes a named one as a named item.
    TreeBuilder::BuiltProperty& parent = builder_->property(id_);
    switch (parent.form) {
        case PropertyForm::Scalar:
            parent.form = name.empty() ? PropertyForm::Sequence : PropertyForm::Record;
            break;
        case PropertyForm::Record:
            if (name.empty()) {
                throw BuildError("a part of a record needs a name");
            }
            break;
        case PropertyForm::Sequence:
            break;
    }
    return Ref(builder_, builder_->addProperty(id_, name, {}, PropertyForm::Scalar));
}

Ref Ref::property(std::string_view name, std::string_view value) {
    if (!valid()) {
        return {};
    }
    Ref made = isNode() ? Ref(builder_, builder_->addProperty(id_, name, value, PropertyForm::Scalar))
                        : child(name);
    if (!isNode()) {
        made.setValue(value);
    }
    return made;
}

Ref Ref::record(std::string_view name) {
    Ref made = property(name);
    if (made.valid()) {
        builder_->property(made.id_).form = PropertyForm::Record;
    }
    return made;
}

Ref Ref::sequence(std::string_view name) {
    Ref made = property(name);
    if (made.valid()) {
        builder_->property(made.id_).form = PropertyForm::Sequence;
    }
    return made;
}

Ref Ref::property(const Property& source) {
    if (!valid()) {
        return {};
    }
    Ref made = property(source.name, source.value);
    made.setSpan(source.span);
    if (source.form != PropertyForm::Scalar) {
        builder_->property(made.id_).form = source.form;
    }

    // Parts are copied with a worklist, so a property nested to any depth
    // costs memory rather than a call frame per level. Each entry pairs a
    // source property with the handle its parts go under.
    std::vector<std::pair<const Property*, RefId>> pending;
    if (source.hasParts()) {
        pending.emplace_back(&source, made.id_);
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

Ref& Ref::setForm(PropertyForm form) {
    if (!valid()) {
        return *this;
    }
    if (isNode()) {
        throw BuildError("a node has no form; only a property does");
    }
    TreeBuilder::BuiltProperty& property = builder_->property(id_);
    if (form == PropertyForm::Scalar && !property.parts.empty()) {
        throw BuildError("a property with parts cannot become a scalar");
    }
    property.form = form;
    return *this;
}

Ref Ref::item(std::string_view value) {
    if (!valid()) {
        return {};
    }
    if (isNode() || builder_->property(id_).form != PropertyForm::Sequence) {
        throw BuildError("an item can only be added to a sequence");
    }
    return Ref(builder_, builder_->addProperty(id_, {}, value, PropertyForm::Scalar));
}

Ref Ref::parent() const {
    if (!valid()) {
        return {};
    }
    if (isNode()) {
        const std::uint32_t up = builder_->node(id_).parent;
        return up == RefId::kNone ? Ref{} : Ref(builder_, RefId{up, RefKind::Node});
    }
    return Ref(builder_, builder_->property(id_).parent);
}

Ref Ref::owner() const {
    Ref at = *this;
    while (at.valid() && !at.isNode()) {
        at = at.parent();
    }
    return at;
}

Ref& Ref::setName(std::string_view name) {
    if (!valid()) {
        return *this;
    }
    if (isNode()) {
        if (name.empty()) {
            throw BuildError("a node needs a kind");
        }
        builder_->node(id_).kind = std::string(name);
    } else {
        builder_->property(id_).name = std::string(name);
    }
    return *this;
}

Ref& Ref::setValue(std::string_view value) {
    if (!valid()) {
        return *this;
    }
    if (isNode()) {
        throw BuildError("a node has no value; add a property instead");
    }
    builder_->property(id_).value = std::string(value);
    return *this;
}

Ref& Ref::setSpan(SourceSpan span) {
    if (!valid()) {
        return *this;
    }
    if (isNode()) {
        builder_->node(id_).span = span;
    } else {
        builder_->property(id_).span = span;
    }
    return *this;
}

Ref& Ref::setSource(DomId element) {
    if (!valid()) {
        return *this;
    }
    if (isNode()) {
        builder_->node(id_).source = element;
    } else {
        builder_->property(id_).source = element;
    }
    builder_->represent(element);
    return *this;
}

Ref& Ref::setChildrenOrdered(bool ordered) {
    if (!valid()) {
        return *this;
    }
    if (!isNode()) {
        throw BuildError("only a node has ordered or unordered children; a property has a form");
    }
    builder_->node(id_).childrenOrdered = ordered;
    return *this;
}

Ref& Ref::setIdentity(std::string_view value, Identity strength) {
    if (!valid() || !isNode()) {
        return *this;
    }
    TreeBuilder::BuiltNode& node = builder_->node(id_);
    node.annotation.identity = std::string(value);
    node.annotation.strongIdentity = strength == Identity::Strong;
    node.annotated = true;
    return *this;
}

Ref& Ref::setTitle(std::string_view title, std::string_view subtitle) {
    if (!valid() || !isNode()) {
        return *this;
    }
    TreeBuilder::BuiltNode& node = builder_->node(id_);
    node.annotation.title = std::string(title);
    node.annotation.subtitle = std::string(subtitle);
    node.annotated = true;
    return *this;
}

Ref& Ref::setAccent(std::uint32_t rgb) {
    if (!valid() || !isNode()) {
        return *this;
    }
    TreeBuilder::BuiltNode& node = builder_->node(id_);
    node.annotation.accent = rgb;
    node.annotated = true;
    return *this;
}

// ---- TreeBuilder ----------------------------------------------------------

TreeBuilder::TreeBuilder(std::string formatName, std::stop_token token)
    : formatName_(std::move(formatName)), token_(std::move(token)) {}

Ref TreeBuilder::root(std::string_view kind, SourceSpan span) {
    if (!nodes_.empty()) {
        throw BuildError("the tree already has a root");
    }
    if (kind.empty()) {
        throw BuildError("the root needs a kind");
    }
    const RefId id = addNode(RefId{}, kind);
    nodes_[id.index].span = span;
    return Ref(this, id);
}

Ref TreeBuilder::at(RefId id) {
    return holds(id) ? Ref(this, id) : Ref{};
}

Ref TreeBuilder::rootRef() {
    return nodes_.empty() ? Ref{} : Ref(this, RefId{0, RefKind::Node});
}

std::vector<DomId> TreeBuilder::unrepresented(std::size_t sourceCount) const {
    std::vector<DomId> missing;
    for (std::size_t i = 0; i < sourceCount; ++i) {
        if (i >= represented_.size() || !represented_[i]) {
            missing.push_back(static_cast<DomId>(i));
        }
    }
    return missing;
}

Result<Tree, ParseError> TreeBuilder::finish() {
    if (nodes_.empty()) {
        return fail(ParseError::Empty);
    }
    if (token_.stop_requested()) {
        return fail(ParseError::Cancelled);
    }

    Tree tree;
    tree.setFormatName(formatName_);
    tree.reserve(nodes_.size());

    const std::vector<NodeId> placed = placeNodes(tree);
    placeProperties(tree, placed);

    // Annotations and ordering travel with the renumbering. Only a node that
    // was told something carries an annotation, so a tree nobody annotated
    // has no side table at all.
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        BuiltNode& built = nodes_[i];
        Node& node = tree.node(placed[i]);
        node.childrenOrdered = built.childrenOrdered;
        if (built.annotated) {
            tree.annotate(placed[i], std::move(built.annotation));
        }
    }

    // A failure's owner was a handle; now it is a node. A failure under a
    // property is charged to the node that property belongs to.
    std::vector<ShapeFailure> failures;
    failures.reserve(failures_.size());
    for (PendingFailure& pending : failures_) {
        ShapeFailure failure;
        failure.span = pending.span;
        failure.message = std::move(pending.message);
        failure.detail = std::move(pending.detail);
        const Ref owner = at(pending.owner).owner();
        if (owner.valid()) {
            failure.owner = placed[owner.id().index];
        }
        failures.push_back(std::move(failure));
    }
    tree.setFailures(std::move(failures));
    tree.setUnrepresented(std::move(unrepresented_));

    nodes_.clear();
    properties_.clear();
    failures_.clear();

    tree.finalize();
    computeHashes(tree, token_);
    if (token_.stop_requested()) {
        return fail(ParseError::Cancelled);
    }
    return tree;
}

TreeBuilder::BuiltNode& TreeBuilder::node(RefId id) {
    return nodes_[id.index];
}

TreeBuilder::BuiltProperty& TreeBuilder::property(RefId id) {
    return properties_[id.index];
}

bool TreeBuilder::holds(RefId id) const noexcept {
    const std::size_t count = id.kind == RefKind::Node ? nodes_.size() : properties_.size();
    return id.index < count;
}

RefId TreeBuilder::addNode(RefId parent, std::string_view kind) {
    const auto index = static_cast<std::uint32_t>(nodes_.size());
    BuiltNode built;
    built.kind = std::string(kind);
    built.parent = parent.index;
    nodes_.push_back(std::move(built));
    if (parent.index != RefId::kNone) {
        nodes_[parent.index].children.push_back(index);
    }
    return RefId{index, RefKind::Node};
}

RefId TreeBuilder::addProperty(RefId parent, std::string_view name, std::string_view value,
                               PropertyForm form) {
    const auto index = static_cast<std::uint32_t>(properties_.size());
    BuiltProperty built;
    built.name = std::string(name);
    built.value = std::string(value);
    built.form = form;
    built.parent = parent;
    properties_.push_back(std::move(built));
    if (parent.kind == RefKind::Node) {
        nodes_[parent.index].properties.push_back(index);
    } else {
        properties_[parent.index].parts.push_back(index);
    }
    return RefId{index, RefKind::Property};
}

bool TreeBuilder::represents(DomId element) const noexcept {
    return element < represented_.size() && represented_[element];
}

void TreeBuilder::recordFailure(RefId owner, SourceSpan span, std::string message,
                                std::string detail) {
    failures_.push_back(PendingFailure{owner, span, std::move(message), std::move(detail)});
}

void TreeBuilder::represent(DomId element) {
    if (element == kInvalidDom) {
        return;
    }
    if (element >= represented_.size()) {
        represented_.resize(static_cast<std::size_t>(element) + 1, false);
    }
    represented_[element] = true;
}

std::vector<NodeId> TreeBuilder::placeNodes(Tree& tree) const {
    std::vector<NodeId> placed(nodes_.size(), kInvalidNode);

    // Pre-order with an explicit stack. Children are pushed in reverse so
    // they pop in call order, which is the order the tree's child lists must
    // have. A parent is always added before its children and a node's
    // descendants land in one contiguous run after it, which is what every
    // pass over the arena relies on.
    std::vector<std::uint32_t> stack;
    stack.push_back(0);
    while (!stack.empty()) {
        const std::uint32_t index = stack.back();
        stack.pop_back();
        const BuiltNode& built = nodes_[index];
        const NodeId parent = built.parent == RefId::kNone ? kInvalidNode : placed[built.parent];
        placed[index] = tree.add(parent, built.kind, built.span);
        for (std::size_t i = built.children.size(); i-- > 0;) {
            stack.push_back(built.children[i]);
        }
    }
    return placed;
}

void TreeBuilder::placeProperties(Tree& tree, const std::vector<NodeId>& placed) {
    // A part is always created after the property it belongs to, so walking
    // the arena backwards assembles every part before the property that
    // holds it. Each finished property is moved into its parent's list, and a
    // property whose parent is a node is attached to the tree.
    std::vector<Property> assembled(properties_.size());
    for (std::size_t i = properties_.size(); i-- > 0;) {
        BuiltProperty& built = properties_[i];
        Property& property = assembled[i];
        property.name = std::move(built.name);
        property.value = std::move(built.value);
        property.form = built.form;
        property.span = built.span;
        property.children.reserve(built.parts.size());
        for (const std::uint32_t part : built.parts) {
            property.children.push_back(std::move(assembled[part]));
        }
    }

    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        for (const std::uint32_t index : nodes_[i].properties) {
            tree.addProperty(placed[i], std::move(assembled[index]));
        }
    }
}

}  // namespace nmxd
