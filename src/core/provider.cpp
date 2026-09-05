#include "core/provider.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <string>

namespace nmxd {

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
