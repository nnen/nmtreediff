/// \file
/// \brief Implementation of the sample behavior-tree provider.

#include "formats/bt_xml.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "core/hash.h"
#include "formats/xml_shape.h"

namespace nmxd {

namespace {

/// \brief The element name that becomes a node.
constexpr std::string_view kNodeElement = "node";

/// \brief The element name that becomes a property of its parent node.
constexpr std::string_view kPropertyElement = "property";

/// \brief The attribute naming a folded property.
constexpr std::string_view kNameAttribute = "name";

/// \brief The attribute holding a folded property's value.
constexpr std::string_view kValueAttribute = "value";

/// \brief The attribute holding a node's stable identifier.
constexpr std::string_view kIdAttribute = "id";

/// \brief The attribute holding the kind of behaviour a node performs.
constexpr std::string_view kTypeAttribute = "type";

/// \brief The kind given to a node element with no type attribute.
constexpr std::string_view kUntypedKind = "node";

/// \brief Attribute names worth seeing first on a node card.
///
/// \remarks The identifier leads because it is what makes a node the same node
///          across versions, and it is the first thing to check when a match
///          looks wrong. Everything folded in from a `<property>` element keeps
///          document order behind these, which is the order the format's own
///          editor shows.
constexpr std::array<std::string_view, 3> kLeadingProperties{"id", "type", "name"};

/// \brief Extensions this format claims outright.
constexpr std::array<std::string_view, 2> kExtensions{".bt", ".btree"};

/// \brief The opening of the document element this format recognises.
///
/// \remarks Matched as text rather than by parsing, because scoring runs on
///          every candidate file and has to stay cheap.
constexpr std::string_view kRootTag = "<behaviortree";

/// \brief Reports whether an element is a `<property>` this provider folds.
///
/// \param element The element to test.
///
/// \returns `true` for a `<property>` element carrying a non-empty name.
///
/// \remarks A `<property>` with no name has nothing to be called, so it is
///          kept as an ordinary unrecognised element rather than folded into
///          something anonymous.
bool isFoldableProperty(const Element& element) {
    return element.name() == kPropertyElement && !element.attributeValue(kNameAttribute).empty();
}

/// \brief Reports whether an element sits inside a folded property's content.
///
/// \param element The element to test.
///
/// \returns `true` when the nearest enclosing element that is either a node
///          or a foldable property is a foldable property.
///
/// \remarks Everything inside a folded property is part of that property,
///          however it is named, because the property's content is read as a
///          value with parts and nothing inside it may be dropped. An
///          unrecognised wrapper in between is transparent, exactly as it is
///          everywhere else.
bool insidePropertyContent(const Element& element) {
    for (const Element* above = element.parent(); above != nullptr; above = above->parent()) {
        if (above->parent() == nullptr || above->name() == kNodeElement) {
            return false;
        }
        if (isFoldableProperty(*above)) {
            return true;
        }
    }
    return false;
}

/// \brief Copies an element's attributes as the parts of a property.
///
/// \param element The element whose attributes to take.
/// \param into The property to add them to.
void attributesAsParts(const Element& element, Property& into) {
    for (const Property& attribute : element.attributes()) {
        into.children.push_back(attribute);
    }
}

/// \brief Turns an element inside a folded property into one of its parts.
///
/// \param element The element, whose items are consumed.
///
/// \returns The part, named after the element, holding a part per attribute
///          and per element inside it, and the text when there is nothing
///          else.
///
/// \remarks The elements inside were themselves turned into parts when they
///          closed, so this is one level of a record built bottom up. A
///          record rather than a sequence: the parts are named, so their
///          order carries nothing.
Property asPart(Element& element) {
    Property part;
    part.name = std::string(element.name());
    part.span = element.span();

    attributesAsParts(element, part);
    for (Item& item : element.takeItems()) {
        if (!item.isNode()) {
            part.children.push_back(std::move(item.property()));
        }
    }

    if (part.children.empty()) {
        // Nothing inside but text, if anything.
        part.value = std::string(element.text());
    } else {
        // One attribute and nothing else reads as a value, not a record.
        collapseSinglePart(part);
    }
    return part;
}

/// \brief Folds one `<property>` element into a property of the node above.
///
/// \param element The `<property>` element, whose items are consumed.
///
/// \returns The property, named after the element's name attribute.
///
/// \remarks The span covers the whole element rather than an attribute,
///          because that is what a reader would expect to see highlighted for
///          a property that is written as an element.
///
///          A property whose content is elements is a property with parts.
///          Reading it as text would find nothing there and quietly lose
///          everything inside, which is the one failure this format must not
///          have.
Property foldProperty(Element& element) {
    const Property* value = element.attribute(kValueAttribute);
    if (value == nullptr && element.childCount() > 0) {
        Property nested = asPart(element);
        nested.name = std::string(element.attributeValue(kNameAttribute));
        // The name and value attributes are the element's own bookkeeping
        // rather than part of what it describes.
        nested.children.erase(std::remove_if(nested.children.begin(), nested.children.end(),
                                             [](const Property& part) {
                                                 return part.name == kNameAttribute ||
                                                        part.name == kValueAttribute;
                                             }),
                              nested.children.end());
        return nested;
    }

    Property folded;
    folded.name = std::string(element.attributeValue(kNameAttribute));
    folded.value = value != nullptr ? value->value : std::string(element.text());
    folded.span = element.span();
    return folded;
}

/// \brief Keeps an element this format does not recognise.
///
/// \param element The element that is neither a node nor a property.
///
/// \returns A property named after the element and holding a part per
///          attribute, so a wrapper carrying something of its own keeps it.
///          An element with nothing but a name still leaves a property
///          behind, because its presence is a fact about the file and its
///          removal is a change worth reporting.
///
/// \remarks Attributes only, deliberately. Whatever the element holds is
///          forwarded to the node above on its own account, so recording it
///          here as well would represent everything inside it twice.
Property foldUnknown(const Element& element) {
    Property folded;
    folded.name = std::string(element.name());
    folded.span = element.span();
    attributesAsParts(element, folded);
    collapseSinglePart(folded);
    return folded;
}

/// \brief Decides what each element of a behaviour tree becomes.
///
/// \remarks Four answers. A `<node>` is a node whose kind is its type. A
///          named `<property>` is a property of the node above. Anything
///          inside such a property is a part of it. Anything else is kept as
///          a property of the node above and whatever it holds is passed
///          through, so a wrapper such as `<children>` neither breaks the
///          tree nor disappears from it. Nothing this format fails to
///          recognise is dropped, because a diff tool that silently loses
///          content is the one thing a reviewer cannot forgive.
class BehaviorTreeShaper final : public IShaper {
public:
    void exit(Element& element, Builder& out) override {
        // The document element becomes the root node whatever it is called,
        // so that a tree always has somewhere to hang and the version
        // attribute stays visible.
        if (element.parent() == nullptr) {
            out.node(std::string(element.name()), element)
                .attributes(element)
                .adopt(element.takeItems());
            return;
        }

        if (insidePropertyContent(element)) {
            out.property(asPart(element));
            return;
        }

        if (element.name() == kNodeElement) {
            shapeNode(element, out);
            return;
        }

        if (isFoldableProperty(element)) {
            out.property(foldProperty(element));
            return;
        }

        out.property(foldUnknown(element));
        out.forward(element.takeItems());
    }

private:
    /// \brief Turns a `<node>` element into a node.
    ///
    /// \param element The element, whose items are consumed.
    /// \param out Where to emit it.
    ///
    /// \remarks The type attribute becomes the kind, which is what makes the
    ///          matcher refuse to pair a Sequence with a MoveTo and what makes
    ///          a card readable. The nodes inside become children and the
    ///          properties folded from `<property>` elements join the node's
    ///          own attributes in one list.
    static void shapeNode(Element& element, Builder& out) {
        const Property* type = element.attribute(kTypeAttribute);
        std::string kind = type == nullptr ? std::string(kUntypedKind) : type->value;
        out.node(std::move(kind), element).attributes(element).adopt(element.takeItems());
    }
};

/// \brief Treats only `<node>` elements as nodes and folds their properties in.
class BehaviorTreeProvider final : public IFormatProvider {
public:
    std::string_view name() const override { return "bt"; }
    std::string_view displayName() const override { return "Behavior tree (XML)"; }

    std::span<const std::string_view> defaultExtensions() const override { return kExtensions; }

    int score(const SourceFile& source) const override {
        // The document element is a far better signal than the extension, so a
        // behaviour tree saved as .xml is still read as one. Scoring it above
        // the generic XML provider's claim on .xml is the whole point of
        // ranked sniffing rather than an extension table.
        const std::string_view head =
            source.text().substr(0, std::min<std::size_t>(512, source.size()));
        if (head.find(kRootTag) != std::string_view::npos) {
            return 95;
        }
        if (claimsExtension(source)) {
            return 85;
        }
        return 0;
    }

    Result<Tree, ParseError> parse(const SourceFile& source, std::stop_token token) const override {
        BehaviorTreeShaper shaper;
        return shapeXmlDocument(source, shaper, *this, token);
    }

    IdentityKey identity(const Tree& tree, NodeId id) const override {
        // The whole reason this format exists as its own provider. An `id` here
        // is a GUID the editor generated, not a name someone typed, so two
        // nodes carrying the same one are the same node however far apart they
        // have moved. That is a promise generic XML cannot make about an
        // attribute called `id`, which is why it returns a weak key instead.
        const Node& node = tree.node(id);
        if (const Property* identifier = node.findProperty(kIdAttribute)) {
            if (!identifier->value.empty()) {
                return IdentityKey{true, identifier->value};
            }
        }
        return IdentityKey{};
    }

    NodeStyle style(const Tree& tree, NodeId id) const override {
        const Node& node = tree.node(id);

        NodeStyle style;
        // The kind is already the type attribute, so a card reads "Sequence"
        // rather than "node".
        style.title = node.kind;
        if (const Property* label = node.findProperty(kNameAttribute)) {
            style.subtitle = label->value;
        }

        // Colour by behaviour type, so every Selector in a document looks alike
        // and a MoveTo looks different, consistently between runs and between
        // the two sides of a diff.
        const std::uint64_t h = hashBytes(node.kind);
        style.accent = Color{static_cast<std::uint8_t>(110 + (h & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 8) & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 16) & 0x3F)), 255};
        return style;
    }

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        return rankFromList(kLeadingProperties, propertyName);
    }

    bool childrenOrdered(const Tree& tree, NodeId id) const override {
        (void)tree;
        (void)id;
        // Sibling order in a behaviour tree is execution order, so moving a
        // child is a change to what the agent does.
        return true;
    }

    GraphDirection graphDirection() const override {
        // A behaviour tree is deep and narrow: a selector with a handful of
        // sequences under it, each a chain of leaves. Read top down that is a
        // tall column nobody can see at once, and left to right it reads like
        // the execution order it describes.
        return GraphDirection::LeftToRight;
    }
};

}  // namespace

std::unique_ptr<IFormatProvider> makeBehaviorTreeProvider() {
    return std::make_unique<BehaviorTreeProvider>();
}

}  // namespace nmxd
