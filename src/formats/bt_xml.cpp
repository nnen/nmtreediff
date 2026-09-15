/// \file
/// \brief Implementation of the sample behavior-tree provider.

#include "formats/bt_xml.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/builder.h"
#include "core/dom.h"
#include "core/shape.h"
#include "formats/xml_generic.h"

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

/// \brief The node types that decorate exactly one node under them.
///
/// \remarks A decorator wraps the node it sits over rather than choosing
///          among children, so the two read as one thing and the node view
///          draws them stacked. The list is this sample format's own; a format
///          whose files say so in an attribute would read that instead.
constexpr std::array<std::string_view, 5> kDecoratorTypes{"Inverter", "Repeater", "Cooldown",
                                                          "Succeeder", "Limit"};

/// \brief Attribute names worth seeing first on a node card.
///
/// \remarks The identifier leads because it is what makes a node the same node
///          across versions, and it is the first thing to check when a match
///          looks wrong. Everything folded in from a `<property>` element keeps
///          document order behind these, which is the order the format's own
///          editor shows.
constexpr std::array<std::string_view, 3> kLeadingProperties{"id", "type", "name"};

/// \brief The attribute a card's second line comes from.
constexpr std::array<std::string_view, 1> kSubtitleProperties{"name"};

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
/// \returns `true` for a `<property>` element carrying a non-empty name
///          attribute.
///
/// \remarks A `<property>` with no name has nothing to be called, so it is
///          walked through as an ordinary container rather than folded into
///          something anonymous.
bool isFoldableProperty(const DomNode& element) {
    if (element.name() != kPropertyElement) {
        return false;
    }
    const auto name = element.attribute(kNameAttribute);
    return name.has_value() && !name->empty();
}

/// \brief Reports whether a property of the source element is an attribute.
///
/// \param property The property to test.
///
/// \returns `false` for the text content the XML reader records under
///          `#text`, and `true` for everything else.
bool isAttribute(const DomProperty& property) {
    return property.name() != kTextProperty;
}

/// \brief Copies an element's attributes onto a handle.
///
/// \param into The node or property to copy onto.
/// \param element The element whose attributes to copy.
void copyAttributes(Ref& into, const DomNode& element) {
    for (const DomProperty property : element.properties()) {
        if (isAttribute(property)) {
            into.property(property);
        }
    }
}

/// \brief Starts a property standing for an element.
///
/// \param element The element to represent.
/// \param name What to call the property.
///
/// \returns The property with a part per attribute and no other parts yet.
Property startProperty(const DomNode& element, std::string name) {
    Property property;
    property.name = std::move(name);
    property.span = element.span();
    for (const DomProperty attribute : element.properties()) {
        if (!isAttribute(attribute)) {
            continue;
        }
        Property part;
        part.name = std::string(attribute.name());
        part.value = std::string(attribute.value());
        part.span = attribute.span();
        property.children.push_back(std::move(part));
    }
    return property;
}

/// \brief Settles a property's form once its parts are all in.
///
/// \param property The property to settle.
/// \param element The element it stands for.
///
/// \remarks An element with one attribute and nothing else reads as a value
///          rather than as a record with one field, which keeps the common
///          case a single line. An element with nothing inside but text is
///          that text. Anything else is a record, never a sequence: these
///          parts are named, so their order carries nothing.
void settleProperty(Property& property, const DomNode& element) {
    if (property.children.empty()) {
        property.value = std::string(element.text());
        return;
    }
    if (property.children.size() == 1 && !property.children.front().hasParts()) {
        property.value = std::move(property.children.front().value);
        property.children.clear();
        return;
    }
    property.form = PropertyForm::Record;
}

/// \brief Turns an element and everything inside it into one property.
///
/// \param element The element to represent.
/// \param name What to call the resulting property.
///
/// \returns The property. Its parts are the element's attributes and its
///          child elements, each turned into a property the same way.
///
/// \remarks Post-order with an explicit stack: a frame holds an element and
///          the property it is becoming, takes its children one at a time,
///          and is settled and handed to the frame below once the last child
///          is in. A property nested to any depth costs memory rather than
///          the process.
Property foldSubtree(const DomNode& element, std::string name) {
    struct Frame {
        DomNode element;
        std::size_t next = 0;
        Property property;
    };

    std::vector<Frame> stack;
    stack.push_back(Frame{element, 0, startProperty(element, std::move(name))});
    while (true) {
        Frame& frame = stack.back();
        if (frame.next < frame.element.childCount()) {
            const DomNode child = frame.element.childAt(frame.next++);
            stack.push_back(Frame{child, 0, startProperty(child, std::string(child.name()))});
            continue;
        }

        settleProperty(frame.property, frame.element);
        Property done = std::move(frame.property);
        stack.pop_back();
        if (stack.empty()) {
            return done;
        }
        stack.back().property.children.push_back(std::move(done));
    }
}

/// \brief Treats only `<node>` elements as nodes and folds their properties in.
///
/// \remarks A shape over the generic XML reading. The XML provider does the
///          bytes; this says what the elements mean, in one pass over the
///          document in arena order, which is pre-order, so a node's handle
///          always exists before anything under it is placed.
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

    Result<Tree, ParseError> read(const SourceFile& source, std::stop_token token) const override {
        // The bytes are XML, and generic XML already reads them as written.
        return xml_->read(source, std::move(token));
    }

    void shape(ShapeContext& context) const override {
        const Dom& dom = context.dom();
        TreeBuilder& out = context.out();
        if (dom.size() == 0) {
            return;
        }

        // Where each element's children attach: the node the element became,
        // or, for an element that became a property, the node above it.
        std::vector<RefId> owners(dom.size());

        // A folded property represents its whole subtree at once, so the
        // walk skips past everything inside it. Arena order is pre-order, so
        // that subtree is the contiguous run after the element.
        DomId skipThrough = 0;

        for (DomId id = 0; id < dom.size(); ++id) {
            if (id != 0 && id <= skipThrough) {
                continue;
            }
            const DomNode element = dom.at(id);
            const DomNode parent = element.parent();

            if (!parent.valid()) {
                // The document element becomes the root node whatever it is
                // called, so that a tree always has somewhere to hang and the
                // version attribute stays visible.
                Ref root = out.root(element.name(), element.span());
                root.setSource(id);
                copyAttributes(root, element);
                owners[id] = root.id();
                continue;
            }

            Ref owner = out.at(owners[parent.id()]);
            if (element.name() == kNodeElement) {
                owners[id] = addNode(owner, element).id();
            } else if (isFoldableProperty(element)) {
                skipThrough = id + dom.tree().node(id).descendantCount;
                foldProperty(out, owner, element, skipThrough);
            } else {
                // Not a node and not a property pair, but nothing is dropped:
                // the element becomes a property of the node above it, and the
                // walk carries on inside so any nodes it holds still surface
                // where they belong. Keeping only one of those two would lose
                // either the wrapper or its contents.
                foldUnknown(owner, element);
                owners[id] = owner.id();
            }
        }
    }

    std::span<const std::string_view> subtitleProperties() const override {
        return kSubtitleProperties;
    }

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        return rankFromList(kLeadingProperties, propertyName);
    }

    GraphDirection graphDirection() const override {
        // A behaviour tree is deep and narrow: a selector with a handful of
        // sequences under it, each a chain of leaves. Read top down that is a
        // tall column nobody can see at once, and left to right it reads like
        // the execution order it describes.
        return GraphDirection::LeftToRight;
    }

private:
    /// \brief Adds one `<node>` element as a node.
    ///
    /// \param owner The node above it.
    /// \param element The element to convert.
    ///
    /// \returns The new node.
    ///
    /// \remarks The type attribute becomes the kind, which is what makes the
    ///          matcher refuse to pair a Sequence with a MoveTo and what makes
    ///          a card readable. The `id` is a GUID the editor generated, not a
    ///          name someone typed, so two nodes carrying the same one are the
    ///          same node however far apart they have moved: a promise generic
    ///          XML cannot make about an attribute called `id`.
    static Ref addNode(Ref& owner, const DomNode& element) {
        const auto type = element.attribute(kTypeAttribute);
        Ref node = owner.child(type.has_value() && !type->empty() ? *type : kUntypedKind);
        node.setSpan(element.span());
        node.setSource(element.id());
        copyAttributes(node, element);

        const auto identifier = element.attribute(kIdAttribute);
        if (identifier.has_value() && !identifier->empty()) {
            node.setIdentity(*identifier, Identity::Strong);
        }
        if (type.has_value() &&
            std::find(kDecoratorTypes.begin(), kDecoratorTypes.end(), *type) !=
                kDecoratorTypes.end()) {
            node.setStacked(true);
        }
        return node;
    }

    /// \brief Folds one `<property>` element into its owning node.
    ///
    /// \param out The builder, for marking the subtree represented.
    /// \param owner The node to attach the property to.
    /// \param element The `<property>` element.
    /// \param lastInside The id of the last element inside it, or its own.
    ///
    /// \remarks A property whose content is elements is a property with
    ///          parts. Reading it as text would find nothing there and
    ///          quietly lose everything inside, which is the one failure this
    ///          format must not have. The name and value attributes are the
    ///          element's own bookkeeping rather than part of what it
    ///          describes, so they are not repeated among the parts.
    ///
    ///          A property that carries a value attribute is that value, and
    ///          any elements inside it are not read. They are then reported
    ///          as unrepresented rather than silently gone.
    static void foldProperty(TreeBuilder& out, Ref& owner, const DomNode& element,
                             DomId lastInside) {
        const std::string name(*element.attribute(kNameAttribute));
        const auto value = element.attribute(kValueAttribute);

        if (!value.has_value() && element.childCount() > 0) {
            Property nested = foldSubtree(element, name);
            std::erase_if(nested.children, [](const Property& part) {
                return part.name == kNameAttribute || part.name == kValueAttribute;
            });
            owner.property(nested);
            for (DomId inside = element.id(); inside <= lastInside; ++inside) {
                out.represent(inside);
            }
            return;
        }

        Ref made = owner.property(name, value.has_value() ? *value : element.text());
        made.setSpan(element.span());
        made.setSource(element.id());
    }

    /// \brief Keeps an element this format does not recognise.
    ///
    /// \param owner The node to attach the property to.
    /// \param element The element that is neither a node nor a property.
    ///
    /// \remarks Named after the element and holding a part per attribute, so
    ///          a wrapper carrying something of its own keeps it. An element
    ///          with nothing but a name still leaves a property behind, because
    ///          its presence is a fact about the file and its removal is a
    ///          change worth reporting.
    ///
    ///          Attributes only, deliberately. The walk carries on into this
    ///          element's children straight after this, so recording them here
    ///          as well would represent everything inside it twice.
    static void foldUnknown(Ref& owner, const DomNode& element) {
        Property folded = startProperty(element, std::string(element.name()));
        if (folded.children.size() == 1) {
            folded.value = std::move(folded.children.front().value);
            folded.children.clear();
        } else if (!folded.children.empty()) {
            folded.form = PropertyForm::Record;
        }
        Ref made = owner.property(folded);
        made.setSource(element.id());
    }

    std::unique_ptr<IFormatProvider> xml_ = makeGenericXmlProvider();
};

}  // namespace

std::unique_ptr<IFormatProvider> makeBehaviorTreeProvider() {
    return std::make_unique<BehaviorTreeProvider>();
}

}  // namespace nmxd
