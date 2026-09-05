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

namespace nmxd {

namespace {

/// \brief Finds where a tag ends.
///
/// \param text The whole document.
/// \param from Offset of the tag's opening angle bracket.
///
/// \returns The offset just past the matching closing bracket, or the end of the
///          document when there is none.
///
/// \remarks pugixml reports where a node starts but not where it ends, and a
///          span needs both. Quoting is respected, so a `>` inside an attribute
///          value is not mistaken for the end of the tag.
std::uint32_t endOfTag(std::string_view text, std::uint32_t from) {
    bool inSingle = false;
    bool inDouble = false;
    for (std::uint32_t i = from; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (c == '"' && !inSingle) {
            inDouble = !inDouble;
        } else if (c == '>' && !inSingle && !inDouble) {
            return i + 1;
        }
    }
    return static_cast<std::uint32_t>(text.size());
}

/// \brief Finds where the next closing tag ends.
///
/// \param text The whole document.
/// \param from Offset at or before the closing tag.
///
/// \returns The offset just past the closing tag, or the end of the document
///          when there is none.
std::uint32_t endOfClosingTag(std::string_view text, std::uint32_t from) {
    for (std::uint32_t i = from; i + 1 < text.size(); ++i) {
        if (text[i] == '<' && text[i + 1] == '/') {
            return endOfTag(text, i);
        }
    }
    return static_cast<std::uint32_t>(text.size());
}

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

    Result<Tree, ParseError> parse(const SourceFile& source, std::stop_token token) const override {
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

        Tree tree;
        tree.setFormatName(std::string(name()));

        const pugi::xml_node root = document.first_child();
        if (root.type() != pugi::node_element) {
            return fail(ParseError::NotWellFormed);
        }

        build(tree, kInvalidNode, root, text, token);
        if (token.stop_requested()) {
            return fail(ParseError::Cancelled);
        }

        tree.finalize();
        computeHashes(tree, *this, token);
        return tree;
    }

    IdentityKey identity(const Tree& tree, NodeId id) const override {
        // Generic XML has no identifier it can trust. An `id` attribute might
        // be a stable key or might be a colour swatch name, so it stays a hint
        // and the matcher falls back to structure. A format that knows its own
        // schema returns a strong key instead.
        const Node& node = tree.node(id);
        std::string key = node.kind;
        if (const Property* p = node.findProperty("id")) {
            key += "#";
            key += p->value;
        }
        return IdentityKey{false, std::move(key)};
    }

    NodeStyle style(const Tree& tree, NodeId id) const override {
        const Node& node = tree.node(id);

        NodeStyle style;
        style.title = node.kind;

        // The first identifying attribute present, so cards are distinguishable
        // without opening them.
        for (const auto candidate : kLeadingProperties) {
            if (const Property* p = node.findProperty(candidate)) {
                style.subtitle = p->value;
                break;
            }
        }

        // Colour derived from the element name, so every <node> in a document
        // looks alike and a <property> looks different, consistently between
        // runs and between the two sides of a diff.
        const std::uint64_t h = hashBytes(node.kind);
        style.accent = Color{static_cast<std::uint8_t>(110 + (h & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 8) & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 16) & 0x3F)), 255};
        return style;
    }

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        if (propertyName == kTextProperty) {
            return static_cast<int>(kLeadingProperties.size()) + 1;
        }
        return rankFromList(kLeadingProperties, propertyName);
    }

    bool childrenOrdered(const Tree& tree, NodeId id) const override {
        (void)tree;
        (void)id;
        return true;  // element order is meaningful in XML
    }

private:
    /// \brief Builds one node and everything below it.
    ///
    /// \param tree The tree being built.
    /// \param parent The parent node's id, or kInvalidNode for the root.
    /// \param element The element to convert.
    /// \param text The whole document, used to recover spans.
    /// \param token Checked before each element.
    static void build(Tree& tree, NodeId parent, const pugi::xml_node& element,
                      std::string_view text, const std::stop_token& token) {
        if (token.stop_requested()) {
            return;
        }

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

        bool hasElementChild = false;
        for (const pugi::xml_node& child : element.children()) {
            if (child.type() == pugi::node_element) {
                hasElementChild = true;
                build(tree, id, child, text, token);
            }
        }

        // Text content only becomes a property on a leaf. On a node that also
        // has element children, mixed content is not what any of the target
        // formats mean, and folding it in would invent a difference.
        if (!hasElementChild) {
            const char* value = element.child_value();
            if (value != nullptr && *value != '\0') {
                const pugi::xml_node textNode = element.first_child();
                const auto textOffset = static_cast<std::uint32_t>(textNode.offset_debug());
                SourceSpan span{textOffset, textOffset};
                span.end = textOffset + static_cast<std::uint32_t>(std::string_view(value).size());
                tree.addProperty(id, std::string(kTextProperty), value, span);
            }
        }

        // Now that every child is placed, the element ends either at its own
        // self-closing tag or just past its closing tag.
        Node& node = tree.node(id);
        const bool selfClosing = startTagEnd >= 2 && text[startTagEnd - 2] == '/';
        if (selfClosing) {
            node.span.end = startTagEnd;
        } else {
            const std::uint32_t searchFrom =
                node.children.empty() ? startTagEnd : tree.node(node.children.back()).span.end;
            node.span.end = endOfClosingTag(text, searchFrom);
        }
    }

    /// \brief Recovers a span for every attribute in one start tag.
    ///
    /// \param text The whole document.
    /// \param tagBegin Offset of the tag's opening angle bracket.
    /// \param tagEnd Offset just past the tag's closing bracket.
    ///
    /// \returns One span per attribute, in document order, each covering the
    ///          name, the equals sign and the quoted value.
    ///
    /// \remarks pugixml reports offsets for nodes but not for attributes, so the
    ///          start tag is scanned once and the spans matched up with the
    ///          parsed attributes, which arrive in the same order.
    static std::vector<SourceSpan> scanAttributeSpans(std::string_view text, std::uint32_t tagBegin,
                                                      std::uint32_t tagEnd) {
        std::vector<SourceSpan> spans;
        const auto isSpace = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };

        std::uint32_t i = tagBegin + 1;  // past '<'
        while (i < tagEnd && !isSpace(text[i]) && text[i] != '>' && text[i] != '/') {
            ++i;  // past the element name
        }

        while (i < tagEnd) {
            while (i < tagEnd && isSpace(text[i])) {
                ++i;
            }
            if (i >= tagEnd || text[i] == '>' || text[i] == '/') {
                break;
            }

            const std::uint32_t nameBegin = i;
            while (i < tagEnd && !isSpace(text[i]) && text[i] != '=' && text[i] != '>' &&
                   text[i] != '/') {
                ++i;
            }
            std::uint32_t end = i;

            while (i < tagEnd && isSpace(text[i])) {
                ++i;
            }
            if (i < tagEnd && text[i] == '=') {
                ++i;
                while (i < tagEnd && isSpace(text[i])) {
                    ++i;
                }
                if (i < tagEnd && (text[i] == '"' || text[i] == '\'')) {
                    const char quote = text[i];
                    ++i;
                    while (i < tagEnd && text[i] != quote) {
                        ++i;
                    }
                    if (i < tagEnd) {
                        ++i;  // past the closing quote
                    }
                    end = i;
                }
            }
            spans.push_back(SourceSpan{nameBegin, end});
        }
        return spans;
    }
};

}  // namespace

std::unique_ptr<IFormatProvider> makeGenericXmlProvider() {
    return std::make_unique<GenericXmlProvider>();
}

}  // namespace nmxd
