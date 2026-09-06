/// \file
/// \brief Implementation of subtree content hashing.

#include "core/hash.h"

#include <algorithm>
#include <vector>

namespace nmxd {

namespace {

/// \brief The FNV-1a offset basis.
///
/// \remarks FNV-1a is chosen because it is fixed, tiny, and has no
///          platform-dependent behaviour. This is not a security hash; it only
///          has to be stable and to spread ordinary document fragments.
constexpr std::uint64_t kOffsetBasis = 1469598103934665603ull;

/// \brief The FNV-1a multiplier.
constexpr std::uint64_t kPrime = 1099511628211ull;

/// \brief Folds a 64-bit value into a running hash, one byte at a time.
///
/// \param h The running hash.
/// \param value The value to fold in.
///
/// \returns The updated hash.
constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t value) noexcept {
    for (int byte = 0; byte < 8; ++byte) {
        h ^= (value >> (byte * 8)) & 0xFFull;
        h *= kPrime;
    }
    return h;
}

}  // namespace

std::uint64_t hashBytes(std::string_view bytes, std::uint64_t seed) noexcept {
    std::uint64_t h = seed == 0 ? kOffsetBasis : seed;
    for (const char c : bytes) {
        h ^= static_cast<std::uint8_t>(c);
        h *= kPrime;
    }
    return h;
}

/// \brief Hashes one property, including any parts it has.
///
/// \param property The property to hash.
///
/// \returns A hash covering the name, the value and the whole subtree.
///
/// \remarks A record's parts are folded in sorted order and a sequence's in
///          the order they appear, which is what makes reordering a transform's
///          fields invisible and reordering a list of tags a change.
///
///          A property with no parts hashes exactly as it did before properties
///          could have any, so nothing in the golden corpus moves except where a
///          format actually starts nesting.
[[nodiscard]] std::uint64_t hashProperty(const Property& property) {
    std::uint64_t h = hashBytes(property.name, kOffsetBasis);
    h = hashBytes("=", h);
    h = hashBytes(property.value, h);
    if (!property.hasParts()) {
        return h;
    }

    std::vector<std::uint64_t> parts;
    parts.reserve(property.children.size());
    for (const Property& child : property.children) {
        parts.push_back(hashProperty(child));
    }
    if (!property.ordered) {
        std::sort(parts.begin(), parts.end());
    }

    h = mix(h, parts.size());
    for (const std::uint64_t part : parts) {
        h = mix(h, part);
    }
    return h;
}

void computeHashes(Tree& tree, const IFormatProvider& provider, std::stop_token token) {
    if (tree.empty()) {
        return;
    }

    std::vector<std::uint64_t> propertyHashes;
    std::vector<std::uint64_t> childHashes;

    // Children always have a higher index than their parent, so walking
    // backwards visits every child before its parent. No recursion, and no
    // stack depth to worry about on a deep tree.
    for (std::size_t i = tree.size(); i-- > 0;) {
        if ((i & 0xFFFu) == 0 && token.stop_requested()) {
            return;
        }

        Node& node = tree.node(static_cast<NodeId>(i));

        std::uint64_t h = hashBytes(node.kind, kOffsetBasis);

        // Properties as a set: hash each pair, sort the hashes, then fold. A
        // reordered attribute list therefore hashes the same.
        propertyHashes.clear();
        propertyHashes.reserve(node.properties.size());
        for (const auto& property : node.properties) {
            propertyHashes.push_back(hashProperty(property));
        }
        std::sort(propertyHashes.begin(), propertyHashes.end());
        h = mix(h, propertyHashes.size());
        for (const auto ph : propertyHashes) {
            h = mix(h, ph);
        }

        childHashes.clear();
        childHashes.reserve(node.children.size());
        for (const auto child : node.children) {
            childHashes.push_back(tree.node(child).contentHash);
        }
        if (!provider.childrenOrdered(tree, node.id)) {
            std::sort(childHashes.begin(), childHashes.end());
        }
        h = mix(h, childHashes.size());
        for (const auto ch : childHashes) {
            h = mix(h, ch);
        }

        node.contentHash = h;
    }
}

}  // namespace nmxd
