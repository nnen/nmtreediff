/// \file
/// \brief Implementation of the generic XML provider.

#include "formats/xml_generic.h"

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

/// \brief Reports whether text begins with a prefix.
///
/// \param text The text to test.
/// \param prefix The prefix to look for.
///
/// \returns `true` when \p text starts with \p prefix.
bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/// \brief Returns a file's extension in lower case.
///
/// \param source The file to inspect.
///
/// \returns The extension including its leading dot, or an empty string when the
///          file has none.
std::string lowerExtension(const SourceFile& source) {
    std::string extension = source.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

/// \brief Attribute names worth seeing first on a node card.
///
/// \remarks Anything unlisted keeps document order behind them, and the text
///          content sorts last because it is usually the longest and the least
///          identifying.
constexpr std::array<std::string_view, 4> kLeadingProperties{"id", "name", "type", "key"};

/// \brief Extensions this format claims outright.
///
/// \remarks A studio asset with an unfamiliar suffix still reaches the sniffing
///          path, so this list does not have to be exhaustive.
constexpr std::array<std::string_view, 7> kExtensions{".xml",  ".xaml", ".svg",   ".xsd",
                                                      ".plist", ".resx", ".config"};

/// \brief Treats every XML element as a node and every attribute as a property.
class GenericXmlProvider final : public IFormatProvider {
public:
    std::string_view name() const override { return "xml"; }
    std::string_view displayName() const override { return "XML (generic)"; }

    std::span<const std::string_view> defaultExtensions() const override { return kExtensions; }

    int score(const SourceFile& source) const override {
        const std::string extension = lowerExtension(source);
        if (extension == ".xml") {
            return 90;
        }
        if (claimsExtension(source)) {
            return 80;
        }

        // No familiar extension, so look at the head of the file. Studios name
        // asset files whatever they like, and a behavior tree with a .bt
        // suffix is still XML underneath.
        std::string_view head = source.text().substr(0, std::min<std::size_t>(256, source.size()));
        while (!head.empty() && (head.front() == ' ' || head.front() == '\n' ||
                                 head.front() == '\r' || head.front() == '\t')) {
            head.remove_prefix(1);
        }
        if (startsWith(head, "<?xml")) {
            return 70;
        }
        if (!head.empty() && head.front() == '<' && head.size() > 1 &&
            (std::isalpha(static_cast<unsigned char>(head[1])) != 0 || head[1] == '_')) {
            return 30;
        }
        return 0;
    }

    Result<Tree, ParseError> read(const SourceFile& source, std::stop_token token) const override {
        if (source.empty()) {
            return fail(ParseError::Empty);
        }

        // Offsets are what spans are made of, so a document whose bytes do not
        // line up with the text is refused rather than silently mis-linked.
        const std::string_view text = source.text();
        if (startsWith(text, "\xFF\xFE") || startsWith(text, "\xFE\xFF")) {
            return fail(ParseError::UnsupportedEncoding);
        }

        pugi::xml_document document;
        const pugi::xml_parse_result result =
            document.load_buffer(text.data(), text.size(), pugi::parse_default,
                                 pugi::encoding_utf8);
        if (!result) {
            return fail(ParseError::NotWellFormed);
        }

        const pugi::xml_node root = document.first_child();
        if (root.type() != pugi::node_element) {
            return fail(ParseError::NotWellFormed);
        }

        Tree tree;
        tree.setFormatName(std::string(name()));
        if (!walk(tree, root, text, token)) {
            return fail(ParseError::Cancelled);
        }

        tree.finalize();
        computeHashes(tree, token);
        return tree;
    }

    Result<Tree, ParseError> parse(const SourceFile& source, std::stop_token token) const override {
        // Generic XML means exactly what it reads, so the shape is the
        // identity and copying a tree to change nothing would be the wrong
        // default for the most common file the tool opens.
        return read(source, token);
    }

    std::span<const std::string_view> subtitleProperties() const override {
        return kLeadingProperties;
    }

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        if (propertyName == kTextProperty) {
            return static_cast<int>(kLeadingProperties.size()) + 1;
        }
        return rankFromList(kLeadingProperties, propertyName);
    }

private:
    /// \brief One element whose children are still being placed.
    struct Frame {
        /// \brief The element.
        pugi::xml_node element;
        /// \brief The node it became.
        NodeId id = kInvalidNode;
        /// \brief Offset just past its start tag.
        std::uint32_t startTagEnd = 0;
        /// \brief The next child to look at.
        pugi::xml_node next;
        /// \brief Whether any child so far was an element.
        bool hasElementChild = false;
    };

    /// \brief Builds every node from the document element down.
    ///
    /// \param tree The tree being built.
    /// \param root The document element.
    /// \param text The whole document, used to recover spans.
    /// \param token Checked per element.
    ///
    /// \returns `false` when the token stopped the walk.
    ///
    /// \remarks Pre-order with an explicit stack of frames rather than a call
    ///          per element, so a document nested thousands of levels deep
    ///          costs memory rather than the process. A frame opens its node
    ///          on the way in, and closes its span on the way out once every
    ///          child is placed.
    static bool walk(Tree& tree, const pugi::xml_node& root, std::string_view text,
                     const std::stop_token& token) {
        std::vector<Frame> stack;
        stack.push_back(open(tree, kInvalidNode, root, text));

        while (!stack.empty()) {
            Frame& frame = stack.back();

            // Skip past whatever is not an element: text, comments and the
            // like are not nodes.
            while (frame.next && frame.next.type() != pugi::node_element) {
                frame.next = frame.next.next_sibling();
            }
            if (frame.next) {
                if (token.stop_requested()) {
                    return false;
                }
                const pugi::xml_node child = frame.next;
                frame.next = child.next_sibling();
                frame.hasElementChild = true;
                const NodeId parent = frame.id;
                stack.push_back(open(tree, parent, child, text));
                continue;
            }

            close(tree, frame, text);
            stack.pop_back();
        }
        return true;
    }

    /// \brief Adds one element as a node, with its attributes as properties.
    ///
    /// \param tree The tree being built.
    /// \param parent The parent node's id, or kInvalidNode for the root.
    /// \param element The element to convert.
    /// \param text The whole document, used to recover spans.
    ///
    /// \returns The frame to keep until the element's children are placed.
    static Frame open(Tree& tree, NodeId parent, const pugi::xml_node& element,
                      std::string_view text) {
        // offset_debug points at the element name, one byte past the '<'.
        const auto nameOffset = static_cast<std::uint32_t>(element.offset_debug());
        const std::uint32_t begin = nameOffset > 0 ? nameOffset - 1 : 0;
        const std::uint32_t startTagEnd = endOfTag(text, begin);

        const NodeId id = tree.add(parent, element.name(), SourceSpan{begin, startTagEnd});

        // pugixml reports offsets for nodes but not for attributes, so the
        // start tag is scanned once and its attribute spans are matched up
        // with the parsed attributes, which arrive in document order.
        const auto spans = scanAttributeSpans(text, begin, startTagEnd);
        std::size_t spanIndex = 0;
        for (const pugi::xml_attribute& attribute : element.attributes()) {
            const SourceSpan span = spanIndex < spans.size() ? spans[spanIndex] : SourceSpan{};
            ++spanIndex;
            tree.addProperty(id, attribute.name(), attribute.value(), span);
        }

        Frame frame;
        frame.element = element;
        frame.id = id;
        frame.startTagEnd = startTagEnd;
        frame.next = element.first_child();
        return frame;
    }

    /// \brief Finishes a node once every child is placed.
    ///
    /// \param tree The tree being built.
    /// \param frame The element and the node it became.
    /// \param text The whole document.
    ///
    /// \remarks Text content only becomes a property on a leaf. On a node
    ///          that also has element children, mixed content is not what any
    ///          of the target formats mean, and folding it in would invent a
    ///          difference. The span then closes either at the self-closing
    ///          tag or just past the closing tag.
    static void close(Tree& tree, const Frame& frame, std::string_view text) {
        if (!frame.hasElementChild) {
            const char* value = frame.element.child_value();
            if (value != nullptr && *value != '\0') {
                const pugi::xml_node textNode = frame.element.first_child();
                const auto textOffset = static_cast<std::uint32_t>(textNode.offset_debug());
                SourceSpan span{textOffset, textOffset};
                span.end = textOffset + static_cast<std::uint32_t>(std::string_view(value).size());
                tree.addProperty(frame.id, std::string(kTextProperty), value, span);
            }
        }

        Node& node = tree.node(frame.id);
        if (isSelfClosing(text, frame.startTagEnd)) {
            node.span.end = frame.startTagEnd;
            return;
        }
        // Searching from the last child rather than from the start tag, so a
        // closing tag belonging to a descendant is not mistaken for this one.
        const std::uint32_t searchFrom =
            node.children.empty() ? frame.startTagEnd : tree.node(node.children.back()).span.end;
        node.span.end = endOfClosingTag(text, searchFrom);
    }
};

}  // namespace

std::unique_ptr<IFormatProvider> makeGenericXmlProvider() {
    return std::make_unique<GenericXmlProvider>();
}

}  // namespace nmxd
