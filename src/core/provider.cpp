/// \file
/// \brief Shared helpers for format providers.

#include "core/provider.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <string>

#include "core/dom.h"
#include "core/hash.h"
#include "core/shape.h"

namespace nmxd {

namespace {

/// \brief Unpacks a colour a handle recorded.
///
/// \param rgb The colour as 0xRRGGBB.
[[nodiscard]] Color unpackAccent(std::uint32_t rgb) {
    return Color{static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
                 static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
                 static_cast<std::uint8_t>(rgb & 0xFF), 255};
}

}  // namespace

std::uint32_t accentForKind(std::string_view kind) noexcept {
    // Mid-toned, so that the diff status tinted on top stays legible.
    const std::uint64_t h = hashBytes(kind);
    const auto r = static_cast<std::uint32_t>(110 + (h & 0x3F));
    const auto g = static_cast<std::uint32_t>(110 + ((h >> 8) & 0x3F));
    const auto b = static_cast<std::uint32_t>(110 + ((h >> 16) & 0x3F));
    return (r << 16) | (g << 8) | b;
}

void IFormatProvider::shape(ShapeContext& context) const {
    copyDocument(context);
}

Result<Tree, ParseError> IFormatProvider::parse(const SourceFile& source,
                                                std::stop_token token) const {
    auto document = read(source, token);
    if (!document.ok()) {
        return fail(document.error());
    }
    // The document stays alive for the whole pass, because the DOM is a view
    // over it and every handle the shape makes points into it.
    const Tree read = std::move(document).value();
    if (read.empty()) {
        return fail(ParseError::Empty);
    }
    const Dom dom(read);
    return shapeTree(dom, std::string(name()),
                     [this](ShapeContext& context) { shape(context); }, token);
}

IdentityKey IFormatProvider::identity(const Tree& tree, NodeId id) const {
    const NodeAnnotation& annotation = tree.annotation(id);
    if (annotation.identity.empty()) {
        return IdentityKey{};
    }
    return IdentityKey{annotation.strongIdentity, annotation.identity};
}

NodeStyle IFormatProvider::style(const Tree& tree, NodeId id) const {
    const Node& node = tree.node(id);
    const NodeAnnotation& annotation = tree.annotation(id);

    NodeStyle style;
    style.title = annotation.title.empty() ? node.kind : annotation.title;
    style.subtitle = annotation.subtitle;

    // The first candidate the node has, so cards are distinguishable without
    // opening them and a format that shapes nothing still gets a second line.
    if (style.subtitle.empty()) {
        for (const std::string_view candidate : subtitleProperties()) {
            if (const Property* p = node.findProperty(candidate)) {
                style.subtitle = p->value;
                break;
            }
        }
    }

    style.accent = unpackAccent(annotation.accent != 0 ? annotation.accent
                                                       : accentForKind(node.kind));
    return style;
}

const char* describe(ParseError error) noexcept {
    switch (error) {
        case ParseError::NotWellFormed:
            return "the document is not well formed";
        case ParseError::UnsupportedEncoding:
            return "the encoding is not supported; only UTF-8 is read today";
        case ParseError::Empty:
            return "the document is empty";
        case ParseError::Cancelled:
            return "parsing was cancelled";
        case ParseError::ShapeFailed:
            return "the format could not shape the document";
    }
    return "unknown parse error";
}

bool IFormatProvider::claimsExtension(const SourceFile& source) const {
    std::string extension = source.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension.empty()) {
        return false;
    }
    const auto extensions = defaultExtensions();
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

GraphDirection resolveDirection(const IFormatProvider& provider, GraphDirection fallback) {
    const GraphDirection stated = provider.graphDirection();
    return stated == GraphDirection::Inherit ? fallback : stated;
}

int rankFromList(std::span<const std::string_view> order, std::string_view name) {
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == name) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(order.size());
}

std::vector<std::uint32_t> propertyDisplayOrder(const IFormatProvider& provider, const Tree& tree,
                                                NodeId id) {
    const auto& properties = tree.node(id).properties;

    std::vector<std::uint32_t> order(properties.size());
    std::iota(order.begin(), order.end(), 0u);

    // Stable, so properties the provider ranks equally stay in document order
    // rather than being shuffled by the sort.
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return provider.propertyRank(tree, id, properties[a].name) <
               provider.propertyRank(tree, id, properties[b].name);
    });
    return order;
}

}  // namespace nmxd
