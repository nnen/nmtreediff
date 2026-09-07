/// \file
/// \brief Implementation of the generic XML provider.

#include "formats/xml_generic.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "core/hash.h"
#include "formats/xml_shape.h"

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

    Result<Tree, ParseError> parse(const SourceFile& source, std::stop_token token) const override {
        // Generic XML is the default treatment and nothing more: every element
        // a node, every attribute a property, leaf text a property too. The
        // driver does the reading, the span recovery and the cancellation.
        DefaultShaper shaper;
        return shapeXmlDocument(source, shaper, *this, token);
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
};

}  // namespace

std::unique_ptr<IFormatProvider> makeGenericXmlProvider() {
    return std::make_unique<GenericXmlProvider>();
}

}  // namespace nmxd
