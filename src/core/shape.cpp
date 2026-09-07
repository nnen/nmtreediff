/// \file
/// \brief Implementation of the shaping layer.

#include "core/shape.h"

#include <algorithm>
#include <utility>

namespace nmxd {

// ------------------------------------------------------------------ Element

const Property* Element::attribute(std::string_view name) const noexcept {
    for (const Property& attribute : attributes_) {
        if (attribute.name == name) {
            return &attribute;
        }
    }
    return nullptr;
}

std::string_view Element::attributeValue(std::string_view name) const noexcept {
    const Property* found = attribute(name);
    return found == nullptr ? std::string_view{} : std::string_view(found->value);
}

Element* Element::ancestor(std::string_view name) noexcept {
    for (Element* above = parent_; above != nullptr; above = above->parent_) {
        if (above->name_ == name) {
            return above;
        }
    }
    return nullptr;
}

const Element* Element::ancestor(std::string_view name) const noexcept {
    for (const Element* above = parent_; above != nullptr; above = above->parent_) {
        if (above->name_ == name) {
            return above;
        }
    }
    return nullptr;
}

Items Element::takeItems() noexcept {
    Items taken = std::move(items_);
    items_.clear();
    return taken;
}

// -------------------------------------------------------------- NodeBuilder

NodeBuilder& NodeBuilder::attributes(const Element& element) {
    PendingNode& node = pending();
    for (const Property& attribute : element.attributes()) {
        node.properties.push_back(attribute);
    }
    return *this;
}

NodeBuilder& NodeBuilder::text(const Element& element) {
    if (!element.text().empty()) {
        property(std::string(kTextProperty), std::string(element.text()), element.textSpan());
    }
    return *this;
}

NodeBuilder& NodeBuilder::property(std::string name, std::string value, SourceSpan span) {
    Property property;
    property.name = std::move(name);
    property.value = std::move(value);
    property.span = span;
    pending().properties.push_back(std::move(property));
    return *this;
}

NodeBuilder& NodeBuilder::property(Property property) {
    pending().properties.push_back(std::move(property));
    return *this;
}

NodeBuilder& NodeBuilder::adopt(Items&& items) {
    for (Item& item : items) {
        adopt(std::move(item));
    }
    items.clear();
    return *this;
}

NodeBuilder& NodeBuilder::adopt(Item&& item) {
    PendingNode& node = pending();
    if (item.isNode()) {
        node.children.push_back(std::move(item.node()));
    } else {
        node.properties.push_back(std::move(item.property()));
    }
    return *this;
}

NodeBuilder& NodeBuilder::kind(std::string kind) {
    pending().kind = std::move(kind);
    return *this;
}

NodeBuilder& NodeBuilder::identity(std::string value, bool strong) {
    PendingNode& node = pending();
    node.annotation.identity = std::move(value);
    node.annotation.strongIdentity = strong;
    node.annotated = true;
    return *this;
}

NodeBuilder& NodeBuilder::title(std::string title, std::string subtitle) {
    PendingNode& node = pending();
    node.annotation.title = std::move(title);
    node.annotation.subtitle = std::move(subtitle);
    node.annotated = true;
    return *this;
}

NodeBuilder& NodeBuilder::orderedChildren(bool ordered) {
    PendingNode& node = pending();
    node.annotation.childrenUnordered = !ordered;
    node.annotated = true;
    return *this;
}

PendingNode& NodeBuilder::pending() { return (*target_)[index_].node(); }

// ------------------------------------------------------------------ Builder

NodeBuilder Builder::node(std::string kind, const Element& source) {
    return node(std::move(kind), source.span());
}

NodeBuilder Builder::node(std::string kind, SourceSpan span) {
    Item item;
    PendingNode& node = item.value.emplace<PendingNode>();
    node.kind = std::move(kind);
    node.span = span;
    target_->push_back(std::move(item));
    touched_ = true;
    return NodeBuilder(*target_, target_->size() - 1);
}

void Builder::property(std::string name, std::string value, const Element& source) {
    property(std::move(name), std::move(value), source.span());
}

void Builder::property(std::string name, std::string value, SourceSpan span) {
    Property property;
    property.name = std::move(name);
    property.value = std::move(value);
    property.span = span;
    this->property(std::move(property));
}

void Builder::property(Property property) {
    Item item;
    item.value.emplace<Property>(std::move(property));
    target_->push_back(std::move(item));
    touched_ = true;
}

void Builder::forward(Items&& items) {
    for (Item& item : items) {
        target_->push_back(std::move(item));
    }
    items.clear();
    touched_ = true;
}

void Builder::forward(Item&& item) {
    target_->push_back(std::move(item));
    touched_ = true;
}

void Builder::defaultTreatment(Element& element) {
    session_->defaultShaper().exit(element, *this);
}

// ------------------------------------------------------------ DefaultShaper

void DefaultShaper::exit(Element& element, Builder& out) {
    // Attributes first and text last, which is the order the generic XML
    // format has always listed them in, so nothing in the corpus moves.
    out.node(std::string(element.name()), element)
        .attributes(element)
        .text(element)
        .adopt(element.takeItems());
}

// ------------------------------------------------------------- ShapeSession

ShapeSession::ShapeSession(IShaper& shaper, std::string_view text)
    : shaper_(shaper), text_(text) {}

Element& ShapeSession::open(std::string name, std::vector<Property> attributes,
                            SourceSpan span) {
    Element* parent = current();

    // A deque keeps every open element where it is when another is pushed,
    // which is what lets a child hold a plain pointer to its parent.
    Element& element = frames_.emplace_back();
    element.name_ = std::move(name);
    element.attributes_ = std::move(attributes);
    element.span_ = span;
    element.parent_ = parent;
    element.serial_ = nextSerial_++;

    if (parent != nullptr) {
        element.depth_ = parent->depth_ + 1;
        element.index_ = parent->childCount_++;
        // A subtree taken away from the shaper stays away all the way down.
        element.mode_ = parent->mode_;
    } else {
        rootName_ = element.name_;
    }

    if (element.mode_ == ShapeMode::Shaped) {
        EnterControl control(element);
        shaper_.enter(element, control);
    }
    return element;
}

bool ShapeSession::descend() const noexcept {
    return !frames_.empty() && frames_.back().mode_ != ShapeMode::Opaque;
}

void ShapeSession::close(std::string text, SourceSpan textSpan, std::uint32_t spanEnd) {
    Element& element = frames_.back();
    element.text_ = std::move(text);
    element.textSpan_ = textSpan;
    element.span_.end = spanEnd;
    element.closed_ = true;
    if (element.parent_ == nullptr) {
        rootSpan_ = element.span_;
    }

    shapeClosed(element);

    if (element.parent_ != nullptr) {
        element.parent_->lastChildEnd_ = spanEnd;
    }
    frames_.pop_back();
}

Element* ShapeSession::current() noexcept {
    return frames_.empty() ? nullptr : &frames_.back();
}

Element* ShapeSession::frameAt(std::uint32_t depth, std::uint64_t serial) noexcept {
    if (depth >= frames_.size()) {
        return nullptr;
    }
    Element& element = frames_[depth];
    return element.serial_ == serial ? &element : nullptr;
}

void ShapeSession::shapeClosed(Element& element) {
    Builder out(targetFor(element), *this);

    switch (element.mode_) {
        case ShapeMode::Opaque: {
            // Nothing inside was visited, so the raw bytes are all there is.
            const SourceSpan span = element.span_;
            const std::size_t begin = std::min<std::size_t>(span.begin, text_.size());
            const std::size_t end = std::min<std::size_t>(span.end, text_.size());
            out.property(std::string(element.name_),
                         std::string(text_.substr(begin, end > begin ? end - begin : 0)), span);
            break;
        }
        case ShapeMode::Default:
            default_.exit(element, out);
            break;
        case ShapeMode::Shaped:
            shaper_.exit(element, out);
            // An exit that said nothing did not mean to lose the element.
            if (!out.touched()) {
                default_.exit(element, out);
            }
            break;
    }

    // Whatever the shaper left behind still belongs to somebody. It goes up
    // to the parent, unless the shaper said outright that it does not.
    if (out.dropped()) {
        element.items_.clear();
    } else if (!element.items_.empty()) {
        out.forward(element.takeItems());
    }
}

Items& ShapeSession::targetFor(Element& element) noexcept {
    return element.parent_ == nullptr ? rootItems_ : element.parent_->items_;
}

Tree ShapeSession::finish(std::string formatName) {
    if (rootName_.empty()) {
        return Tree{};
    }

    PendingNode root;
    if (rootItems_.size() == 1 && rootItems_.front().isNode()) {
        root = std::move(rootItems_.front().node());
    } else {
        // The document element did not become exactly one node, so a root
        // standing for the document holds whatever it did become.
        root.kind = rootName_;
        root.span = rootSpan_;
        for (Item& item : rootItems_) {
            if (item.isNode()) {
                root.children.push_back(std::move(item.node()));
            } else {
                root.properties.push_back(std::move(item.property()));
            }
        }
    }
    rootItems_.clear();
    return materialize(std::move(root), std::move(formatName));
}

// -------------------------------------------------------------- materialize

Tree materialize(PendingNode&& root, std::string formatName) {
    Tree tree;
    tree.setFormatName(std::move(formatName));

    /// \brief One staged node waiting to be written, and where it hangs.
    struct Visit {
        PendingNode* node;
        NodeId parent;
    };

    // An explicit stack rather than recursion, so a deep document cannot
    // overflow anything here. Children are pushed last to first, which makes
    // the pops come out in document order and keeps every subtree contiguous.
    std::vector<Visit> stack;
    stack.push_back(Visit{&root, kInvalidNode});
    while (!stack.empty()) {
        const Visit visit = stack.back();
        stack.pop_back();

        PendingNode& node = *visit.node;
        const NodeId id = tree.add(visit.parent, std::move(node.kind), node.span);
        for (Property& property : node.properties) {
            tree.addProperty(id, std::move(property));
        }
        if (node.annotated) {
            tree.annotate(id, std::move(node.annotation));
        }
        for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
            stack.push_back(Visit{&*child, id});
        }
    }
    return tree;
}

}  // namespace nmxd
