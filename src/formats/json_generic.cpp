/// \file
/// \brief Implementation of the generic JSON provider.

#include "formats/json_generic.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

#include "core/hash.h"
#include "formats/json_shape.h"

namespace nmxd {

namespace {

/// \brief Member names worth seeing first on a node card.
///
/// \remarks Anything unlisted keeps document order behind them. The two
///          synthetic properties sort last because they describe the node
///          rather than identify it.
constexpr std::array<std::string_view, 4> kLeadingProperties{"id", "name", "type", "key"};

/// \brief Extensions this format claims outright.
///
/// \remarks A studio asset with an unfamiliar suffix still reaches the sniffing
///          path, so this list does not have to be exhaustive.
constexpr std::array<std::string_view, 3> kExtensions{".json", ".geojson", ".webmanifest"};

/// \brief Reports whether a byte is JSON whitespace.
///
/// \param c The byte to test.
///
/// \returns `true` for space, tab, carriage return and line feed.
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

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

/// \brief Reports whether a node is an array.
///
/// \param node The node to inspect.
///
/// \returns `true` when the node was built from a JSON array.
bool isArray(const Node& node) {
    const Property* type = node.findProperty(kJsonTypeProperty);
    return type != nullptr && type->value == kJsonArrayType;
}

/// \brief Treats every object, array and array element as a node.
class GenericJsonProvider final : public IFormatProvider {
public:
    std::string_view name() const override { return "json"; }
    std::string_view displayName() const override { return "JSON (generic)"; }

    std::span<const std::string_view> defaultExtensions() const override { return kExtensions; }

    int score(const SourceFile& source) const override {
        const std::string extension = lowerExtension(source);
        if (extension == ".json") {
            return 90;
        }
        if (claimsExtension(source)) {
            return 80;
        }

        // No familiar extension, so look at the head of the file. A game
        // configuration file called .cfg or .asset is still JSON underneath,
        // and an opening brace or bracket is a strong enough signal that no
        // other built-in format competes for it.
        std::string_view head = source.text().substr(0, std::min<std::size_t>(256, source.size()));
        if (startsWith(head, "\xEF\xBB\xBF")) {
            head.remove_prefix(3);
        }
        while (!head.empty() && isSpace(head.front())) {
            head.remove_prefix(1);
        }
        if (!head.empty() && (head.front() == '{' || head.front() == '[')) {
            return 60;
        }
        return 0;
    }

    Result<Tree, ParseError> parse(const SourceFile& source, std::stop_token token) const override {
        // Generic JSON is the default treatment and nothing more. The driver
        // does the reading, the spans and the cancellation.
        JsonDefaultShaper shaper;
        return shapeJsonDocument(source, shaper, *this, token);
    }

    IdentityKey identity(const Tree& tree, NodeId id) const override {
        // Generic JSON has no identifier it can trust. A member key is a hint
        // and nothing more, and an array element has not even that: its
        // position is the only thing naming it, and position is what the
        // matcher is trying to work out.
        const Node& node = tree.node(id);
        if (node.kind == kJsonElementKind) {
            return IdentityKey{};
        }
        return IdentityKey{false, node.kind};
    }

    NodeStyle style(const Tree& tree, NodeId id) const override {
        const Node& node = tree.node(id);

        NodeStyle style;
        style.title = node.kind;

        if (const Property* value = node.findProperty(kValueProperty)) {
            style.subtitle = value->value;
        } else {
            for (const auto candidate : kLeadingProperties) {
                if (const Property* p = node.findProperty(candidate)) {
                    style.subtitle = p->value;
                    break;
                }
            }
            if (style.subtitle.empty()) {
                style.subtitle =
                    isArray(node) ? std::string(kJsonArrayType) : std::string(kJsonObjectType);
            }
        }

        // Colour derived from the member key, so every node under "enemies"
        // looks alike and one under "props" looks different. Array elements
        // borrow their parent's key, because their own kind is the same
        // throughout the document and would otherwise paint every array in a
        // file the same colour.
        std::string_view palette = node.kind;
        if (node.kind == kJsonElementKind && node.parent != kInvalidNode) {
            palette = tree.node(node.parent).kind;
        }
        const std::uint64_t h = hashBytes(palette);
        style.accent = Color{static_cast<std::uint8_t>(110 + (h & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 8) & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 16) & 0x3F)), 255};
        return style;
    }

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        if (propertyName == kJsonTypeProperty || propertyName == kValueProperty) {
            return static_cast<int>(kLeadingProperties.size()) + 1;
        }
        return rankFromList(kLeadingProperties, propertyName);
    }

    bool childrenOrdered(const Tree& tree, NodeId id) const override {
        // The hook that generic XML never exercises. Reordering the members of
        // an object changes nothing about the document, so a reordering there
        // is not a move and should not be reported as one. Reordering an array
        // changes the document, so it is.
        return isArray(tree.node(id));
    }
};

}  // namespace

std::unique_ptr<IFormatProvider> makeGenericJsonProvider() {
    return std::make_unique<GenericJsonProvider>();
}

}  // namespace nmxd
