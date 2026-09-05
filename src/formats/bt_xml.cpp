/// \file
/// \brief Implementation of the sample behavior-tree provider.

#include "formats/bt_xml.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <pugixml.hpp>

#include "core/hash.h"
#include "formats/xml_spans.h"

namespace nmxd {

namespace {

/// \brief The element name that becomes a node.
constexpr const char* kNodeElement = "node";

/// \brief The element name that becomes a property of its parent node.
constexpr const char* kPropertyElement = "property";

/// \brief The attribute naming a folded property.
constexpr const char* kNameAttribute = "name";

/// \brief The attribute holding a folded property's value.
constexpr const char* kValueAttribute = "value";

/// \brief The attribute holding a node's stable identifier.
constexpr const char* kIdAttribute = "id";

/// \brief The attribute holding the kind of behaviour a node performs.
constexpr const char* kTypeAttribute = "type";

/// \brief The kind given to a node element with no type attribute.
constexpr const char* kUntypedKind = "node";

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

/// \brief Reports whether text begins with a prefix.
///
/// \param text The text to test.
/// \param prefix The prefix to look for.
///
/// \returns `true` when \p text starts with \p prefix.
bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// \brief Reports whether an element has a given name.
///
/// \param element The element to test.
/// \param name The name to compare against.
///
/// \returns `true` when the names match exactly. XML is case-sensitive and so
///          is this.
bool isNamed(const pugi::xml_node& element, const char* name) {
    return std::string_view(element.name()) == name;
}

/// \brief Reports whether an element is a `<property>` this provider folds.
///
/// \param element The element to test.
///
/// \returns `true` for a `<property>` element carrying a name attribute.
///
/// \remarks A `<property>` with no name has nothing to be called, so it is
///          walked through as an ordinary container rather than folded into
///          something anonymous.
bool isFoldableProperty(const pugi::xml_node& element) {
    return isNamed(element, kPropertyElement) && !element.attribute(kNameAttribute).empty();
}

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
        if (source.empty()) {
            return fail(ParseError::Empty);
        }

        const std::string_view text = source.text();
        if (startsWith(text, "\xFF\xFE") || startsWith(text, "\xFE\xFF")) {
            return fail(ParseError::UnsupportedEncoding);
        }

        pugi::xml_document document;
        const pugi::xml_parse_result result = document.load_buffer(
            text.data(), text.size(), pugi::parse_default, pugi::encoding_utf8);
        if (!result) {
            return fail(ParseError::NotWellFormed);
        }

        const pugi::xml_node root = document.first_child();
        if (root.type() != pugi::node_element) {
            return fail(ParseError::NotWellFormed);
        }

        Tree tree;
        tree.setFormatName(std::string(name()));

        // The document element becomes the root node whatever it is called, so
        // that a tree always has somewhere to hang and the version attribute
        // stays visible.
        const NodeId rootId = addElement(tree, kInvalidNode, root, text, std::string(root.name()));
        collect(tree, rootId, root, text, token);
        if (token.stop_requested()) {
            return fail(ParseError::Cancelled);
        }
        closeSpan(tree, rootId, root, text);

        tree.finalize();
        computeHashes(tree, *this, token);
        return tree;
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

private:
    /// \brief Adds one element as a node, with its attributes as properties.
    ///
    /// \param tree The tree being built.
    /// \param parent The parent node's id, or kInvalidNode for the root.
    /// \param element The element to convert.
    /// \param text The whole document, used to recover spans.
    /// \param kind The kind to give the node.
    ///
    /// \returns The new node's id, whose span still has to be closed once its
    ///          children are placed.
    static NodeId addElement(Tree& tree, NodeId parent, const pugi::xml_node& element,
                             std::string_view text, std::string kind) {
        // offset_debug points at the element name, one byte past the '<'.
        const auto nameOffset = static_cast<std::uint32_t>(element.offset_debug());
        const std::uint32_t begin = nameOffset > 0 ? nameOffset - 1 : 0;
        const std::uint32_t startTagEnd = endOfTag(text, begin);

        const NodeId id = tree.add(parent, std::move(kind), SourceSpan{begin, startTagEnd});

        const auto spans = scanAttributeSpans(text, begin, startTagEnd);
        std::size_t spanIndex = 0;
        for (const pugi::xml_attribute& attribute : element.attributes()) {
            const SourceSpan span = spanIndex < spans.size() ? spans[spanIndex] : SourceSpan{};
            ++spanIndex;
            tree.addProperty(id, attribute.name(), attribute.value(), span);
        }
        return id;
    }

    /// \brief Walks an element's children, folding properties and nesting nodes.
    ///
    /// \param tree The tree being built.
    /// \param owner The node that folded properties belong to.
    /// \param element The element whose children to walk.
    /// \param text The whole document, used to recover spans.
    /// \param token Checked before each element.
    ///
    /// \remarks An element that is neither a node nor a foldable property is
    ///          walked through rather than represented, so a wrapper such as
    ///          `<children>` does not break the tree. That is the one place
    ///          this sample is simpler than a studio provider would be: it
    ///          silently drops whatever such an element carried of its own.
    static void collect(Tree& tree, NodeId owner, const pugi::xml_node& element,
                        std::string_view text, const std::stop_token& token) {
        if (token.stop_requested()) {
            return;
        }

        for (const pugi::xml_node& child : element.children()) {
            if (child.type() != pugi::node_element) {
                continue;
            }

            if (isNamed(child, kNodeElement)) {
                // The type attribute becomes the kind, which is what makes the
                // matcher refuse to pair a Sequence with a MoveTo and what
                // makes a card readable.
                const pugi::xml_attribute type = child.attribute(kTypeAttribute);
                std::string kind = type.empty() ? std::string(kUntypedKind) : type.value();

                const NodeId id = addElement(tree, owner, child, text, std::move(kind));
                collect(tree, id, child, text, token);
                if (token.stop_requested()) {
                    return;
                }
                closeSpan(tree, id, child, text);
            } else if (isFoldableProperty(child)) {
                foldProperty(tree, owner, child, text);
            } else {
                // Not ours, but something below it might be.
                collect(tree, owner, child, text, token);
            }
        }
    }

    /// \brief Folds one `<property>` element into its owning node.
    ///
    /// \param tree The tree being built.
    /// \param owner The node to attach the property to.
    /// \param element The `<property>` element.
    /// \param text The whole document, used to recover spans.
    ///
    /// \remarks The span covers the whole element rather than an attribute,
    ///          because that is what a reader would expect to see highlighted
    ///          for a property that is written as an element. A property whose
    ///          name collides with one of the node's own attributes is kept
    ///          rather than dropped, so nothing in the file goes unreported.
    static void foldProperty(Tree& tree, NodeId owner, const pugi::xml_node& element,
                             std::string_view text) {
        const auto nameOffset = static_cast<std::uint32_t>(element.offset_debug());
        const std::uint32_t begin = nameOffset > 0 ? nameOffset - 1 : 0;
        const std::uint32_t startTagEnd = endOfTag(text, begin);
        const std::uint32_t end =
            isSelfClosing(text, startTagEnd) ? startTagEnd : endOfClosingTag(text, startTagEnd);

        const pugi::xml_attribute value = element.attribute(kValueAttribute);
        const char* content = value.empty() ? element.child_value() : value.value();

        tree.addProperty(owner, element.attribute(kNameAttribute).value(),
                         content == nullptr ? "" : content, SourceSpan{begin, end});
    }

    /// \brief Extends a node's span to cover everything inside it.
    ///
    /// \param tree The tree being built.
    /// \param id The node to close.
    /// \param element The element the node came from.
    /// \param text The whole document.
    ///
    /// \remarks Called only after every child is placed, because adding one
    ///          invalidates any reference taken before it.
    static void closeSpan(Tree& tree, NodeId id, const pugi::xml_node& element,
                          std::string_view text) {
        const auto nameOffset = static_cast<std::uint32_t>(element.offset_debug());
        const std::uint32_t begin = nameOffset > 0 ? nameOffset - 1 : 0;
        const std::uint32_t startTagEnd = endOfTag(text, begin);

        Node& node = tree.node(id);
        if (isSelfClosing(text, startTagEnd)) {
            node.span.end = startTagEnd;
            return;
        }

        // Searching from the last child rather than from the start tag, so a
        // closing tag belonging to a descendant is not mistaken for this one.
        const std::uint32_t searchFrom =
            node.children.empty() ? startTagEnd : tree.node(node.children.back()).span.end;
        node.span.end = endOfClosingTag(text, searchFrom);
    }
};

}  // namespace

std::unique_ptr<IFormatProvider> makeBehaviorTreeProvider() {
    return std::make_unique<BehaviorTreeProvider>();
}

}  // namespace nmxd
